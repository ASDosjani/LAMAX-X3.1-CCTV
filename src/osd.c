/* On-screen text burned into the video frames (NV12): white text with a black
 * outline, so it stays readable on bright and dark scenes. The text is rendered
 * into a small mask once per second; each frame only copies the mask. */
#include "osd.h"
#include "font8x16.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OSD_MAXCHARS 64
#define Y_TEXT 235
#define Y_EDGE 16

struct osd {
    int fw, fh;             /* frame size */
    int scale, edge;        /* font scale, outline thickness (pixels) */
    int x, y;               /* mask position in the frame (even) */
    int right, bottom;      /* anchored to the right / bottom edge */
    char name[32];
    time_t shown;           /* second the mask was made for */
    int mw, mh;             /* mask size */
    uint8_t *mask;          /* 0 = video, 1 = outline, 2 = text */
};

osd_t *osd_create(int width, int height, const char *position, const char *name)
{
    osd_t *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->fw = width; o->fh = height;
    o->scale = height >= 1000 ? 3 : 2;            /* 48 px text at 1080p, 32 px at 720p */
    o->edge = o->scale >= 3 ? 2 : 1;
    o->right = position && strstr(position, "right") != NULL;
    o->bottom = position && strstr(position, "bottom") != NULL;
    if (name) {
        /* the font is code page 437: keep printable ASCII, replace the rest */
        int n = 0;
        for (const unsigned char *p = (const unsigned char *)name; *p && n < (int)sizeof(o->name) - 1; p++) {
            if (*p >= 0x80 && (*p & 0xc0) == 0x80) continue;        /* UTF-8 continuation byte */
            o->name[n++] = (*p >= 32 && *p < 127) ? (char)*p : '?';
        }
    }
    o->shown = -1;
    o->mask = malloc((size_t)(OSD_MAXCHARS * 8 * o->scale + 2 * o->edge) * (16 * o->scale + 2 * o->edge));
    if (!o->mask) { free(o); return NULL; }
    return o;
}

static void render(osd_t *o, const char *text)
{
    int n = (int)strlen(text);
    if (n > OSD_MAXCHARS) n = OSD_MAXCHARS;
    const int sc = o->scale, ed = o->edge;
    o->mw = n * 8 * sc + 2 * ed;
    o->mh = 16 * sc + 2 * ed;
    memset(o->mask, 0, (size_t)o->mw * o->mh);
    /* glyphs */
    for (int i = 0; i < n; i++) {
        const unsigned char *g = font8x16 + (unsigned char)text[i] * 16;
        for (int gy = 0; gy < 16; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (0x80 >> gx))
                    for (int dy = 0; dy < sc; dy++)
                        memset(o->mask + (size_t)(ed + gy * sc + dy) * o->mw + ed + (i * 8 + gx) * sc, 2, sc);
    }
    /* outline: every video pixel within `edge` of a text pixel */
    for (int y = 0; y < o->mh; y++)
        for (int x = 0; x < o->mw; x++) {
            if (o->mask[y * o->mw + x] != 2) continue;
            for (int dy = -ed; dy <= ed; dy++)
                for (int dx = -ed; dx <= ed; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy >= 0 && yy < o->mh && xx >= 0 && xx < o->mw && o->mask[yy * o->mw + xx] == 0)
                        o->mask[yy * o->mw + xx] = 1;
                }
        }
    /* position, 2 % of the height from the edges, even for the 2x2 chroma */
    int margin = (o->fh / 50) & ~1;
    o->x = o->right ? o->fw - o->mw - margin : margin;
    o->y = o->bottom ? o->fh - o->mh - margin : margin;
    if (o->x < 0) o->x = 0;
    if (o->y < 0) o->y = 0;
    o->x &= ~1; o->y &= ~1;
}

void osd_draw(osd_t *o, uint8_t *y, uint8_t *uv, int stride)
{
    if (!o) return;
    time_t now = time(NULL);
    if (now != o->shown) {
        char text[OSD_MAXCHARS + 1], when[32];
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_year + 1900 < 2024)                   /* clock never set (no network time yet) */
            strcpy(when, "----------  --:--:--");
        else
            strftime(when, sizeof(when), "%Y-%m-%d  %H:%M:%S", &tm);
        if (o->name[0]) snprintf(text, sizeof(text), "%s  %s", o->name, when);
        else snprintf(text, sizeof(text), "%s", when);
        render(o, text);
        o->shown = now;
    }
    int w = o->mw, h = o->mh;
    if (o->x + w > o->fw) w = o->fw - o->x;
    if (o->y + h > o->fh) h = o->fh - o->y;
    for (int r = 0; r < h; r++) {
        const uint8_t *m = o->mask + (size_t)r * o->mw;
        uint8_t *py = y + (size_t)(o->y + r) * stride + o->x;
        for (int c = 0; c < w; c++)
            if (m[c]) py[c] = m[c] == 2 ? Y_TEXT : Y_EDGE;
    }
    /* neutral colour (grey axis) under the text, one chroma sample per 2x2 pixels */
    for (int r = 0; r + 1 < h; r += 2) {
        const uint8_t *m0 = o->mask + (size_t)r * o->mw, *m1 = m0 + o->mw;
        uint8_t *puv = uv + (size_t)((o->y + r) / 2) * stride + o->x;
        for (int c = 0; c + 1 < w; c += 2)
            if (m0[c] | m0[c + 1] | m1[c] | m1[c + 1]) { puv[c] = 128; puv[c + 1] = 128; }
    }
}

void osd_free(osd_t *o)
{
    if (!o) return;
    free(o->mask);
    free(o);
}
