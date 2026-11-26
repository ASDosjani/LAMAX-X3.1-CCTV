/* On-screen text burned into the video frames (NV12): white text with a black
 * outline, so it stays readable on bright and dark scenes. The text is rendered
 * into a small mask once per second; each frame only copies the mask. */
#include "osd.h"
#include "font8x16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OSD_MAXCHARS 64
#define Y_TEXT 235
#define Y_EDGE 16

struct osd {
    int fw, fh;             /* frame size */
    int s2, edge;           /* font scale in half steps (3 = 1.5x), outline thickness (pixels) */
    int cw, ch;             /* character cell size in the frame */
    int x, y;               /* mask position in the frame (even) */
    int right, bottom;      /* anchored to the right / bottom edge */
    char name[32];
    int battery;            /* show the battery percentage while on battery */
    time_t shown;           /* second the mask was made for */
    int mw, mh;             /* mask size */
    uint8_t *mask;          /* 0 = video, 1 = outline, 2 = text */
};

osd_t *osd_create(int width, int height, const char *position, const char *name, int battery)
{
    osd_t *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->battery = battery;
    o->fw = width; o->fh = height;
    /* text about 3.2 % of the frame height, in half steps of the 8x16 font:
     * 1520p 48 px (3x), 1080p 32 px (2x), 720p 24 px (1.5x) */
    o->s2 = (height * 6 + 760) / 1520;
    if (o->s2 < 2) o->s2 = 2;
    o->edge = o->s2 >= 6 ? 2 : 1;
    o->cw = 4 * o->s2;                            /* 8 * s2 / 2 */
    o->ch = 8 * o->s2;                            /* 16 * s2 / 2 */
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
    o->mask = malloc((size_t)(OSD_MAXCHARS * o->cw + 2 * o->edge) * (o->ch + 2 * o->edge));
    if (!o->mask) { free(o); return NULL; }
    return o;
}

static int read_int(const char *path, int dflt)
{
    FILE *f = fopen(path, "r");
    int v = dflt;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = dflt; fclose(f); }
    return v;
}

/* battery percentage while the camera runs from its battery, else -1
 * (external power, or no battery fitted) */
static int battery_pct(void)
{
#define PS "/sys/class/power_supply/"
    if (read_int(PS "battery/present", 1) == 0) return -1;
    if (read_int(PS "usb/online", 0) || read_int(PS "ac/online", 0)) return -1;
    return read_int(PS "battery/capacity", -1);
#undef PS
}

static void render(osd_t *o, const char *text)
{
    int n = (int)strlen(text);
    if (n > OSD_MAXCHARS) n = OSD_MAXCHARS;
    const int ed = o->edge, s2 = o->s2;
    o->mw = n * o->cw + 2 * ed;
    o->mh = o->ch + 2 * ed;
    memset(o->mask, 0, (size_t)o->mw * o->mh);
    /* glyphs, nearest neighbour (at 1.5x every other font pixel is 2 wide) */
    for (int i = 0; i < n; i++) {
        const unsigned char *g = font8x16 + (unsigned char)text[i] * 16;
        for (int oy = 0; oy < o->ch; oy++) {
            unsigned char bits = g[oy * 2 / s2];
            uint8_t *row = o->mask + (size_t)(ed + oy) * o->mw + ed + i * o->cw;
            for (int ox = 0; ox < o->cw; ox++)
                if (bits & (0x80 >> (ox * 2 / s2))) row[ox] = 2;
        }
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
        char bat[16] = "";
        int pct = o->battery ? battery_pct() : -1;
        if (pct >= 0) snprintf(bat, sizeof(bat), "  BAT %d%%", pct);
        snprintf(text, sizeof(text), "%s%s%s%s", o->name, o->name[0] ? "  " : "", when, bat);
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
