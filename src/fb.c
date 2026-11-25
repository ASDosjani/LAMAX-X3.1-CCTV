#include "fb.h"
#include "font8x16.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int fb_open(fb_t *f, const char *dev)
{
    memset(f, 0, sizeof(*f));
    f->fd = open(dev, O_RDWR);
    if (f->fd < 0) { LOGE("fb: open %s: %s", dev, strerror(errno)); return -1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo x;
    if (ioctl(f->fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(f->fd, FBIOGET_FSCREENINFO, &x) < 0) {
        LOGE("fb: ioctl: %s", strerror(errno)); close(f->fd); return -1;
    }
    f->w = v.xres; f->h = v.yres; f->bpp = v.bits_per_pixel; f->stride = x.line_length;
    LOGI("fb: %dx%d (virtual %dx%d) %dbpp stride %d, offset %d", v.xres, v.yres, v.xres_virtual, v.yres_virtual,
         v.bits_per_pixel, x.line_length, v.yoffset);
    f->mem = mmap(NULL, x.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->mem == MAP_FAILED) { LOGE("fb: mmap: %s", strerror(errno)); close(f->fd); return -1; }
    f->back = calloc((size_t)f->w * f->h, 4);
    f->var = v;
    return 0;
}

void fb_clear(fb_t *f, uint32_t c)
{
    for (int i = 0; i < f->w * f->h; i++) f->back[i] = c;
}

void fb_rect(fb_t *f, int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > f->w) w = f->w - x;
    if (y + h > f->h) h = f->h - y;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) f->back[(y + j) * f->w + x + i] = c;
}

/* decode one UTF-8 char and map it to a CP437 glyph index */
static int next_glyph(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned cp;
    if (s[0] < 0x80) { cp = s[0]; *ps += 1; }
    else if ((s[0] & 0xe0) == 0xc0 && s[1]) { cp = ((s[0] & 0x1f) << 6) | (s[1] & 0x3f); *ps += 2; }
    else if ((s[0] & 0xf0) == 0xe0 && s[1] && s[2]) { cp = ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); *ps += 3; }
    else { *ps += 1; return '?'; }
    if (cp < 0x80) return cp;
    switch (cp) {
    case 0xe1: return 0xa0; /* á */  case 0xe9: return 0x82; /* é */  case 0xed: return 0xa1; /* í */
    case 0xf3: return 0xa2; /* ó */  case 0xf6: return 0x94; /* ö */  case 0x151: return 0x94; /* ő -> ö */
    case 0xfa: return 0xa3; /* ú */  case 0xfc: return 0x81; /* ü */  case 0x171: return 0x81; /* ű -> ü */
    case 0xc1: return 'A'; case 0xc9: return 0x90; /* É */ case 0xcd: return 'I'; case 0xd3: return 'O';
    case 0xd6: return 0x99; /* Ö */ case 0x150: return 0x99; case 0xda: return 'U'; case 0xdc: return 0x9a; /* Ü */
    case 0x170: return 0x9a;
    case 0xb0: return 0xf8; /* ° */
    case 0x2588: return 0xdb; /* █ */ case 0x2591: return 0xb0; /* ░ */
    case 0x25b2: return 0x1e; /* ▲ */ case 0x25bc: return 0x1f; /* ▼ */
    default: return '?';
    }
}

int fb_text_width(const char *s, int scale)
{
    int n = 0;
    while (*s) { next_glyph(&s); n++; }
    return n * 8 * scale;
}

int fb_text(fb_t *f, int x, int y, const char *s, uint32_t fg, int scale)
{
    while (*s) {
        int g = next_glyph(&s);
        const unsigned char *bm = &font8x16[g * 16];
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 8; c++)
                if (bm[r] & (0x80 >> c)) {
                    if (scale == 1) {
                        int px = x + c, py = y + r;
                        if (px >= 0 && py >= 0 && px < f->w && py < f->h) f->back[py * f->w + px] = fg;
                    } else fb_rect(f, x + c * scale, y + r * scale, scale, scale, fg);
                }
        x += 8 * scale;
        if (x >= f->w) break;
    }
    return x;
}

static void layer_commit(fb_t *f);

void fb_present(fb_t *f)
{
    /* The display layer (ch2/l0) scans page 0 of fb0 (0x42600000). The 2"
     * LCD keeps its own picture and is only refreshed when the layer config
     * is applied again, so draw into page 0 and re-commit the layer. */
    uint8_t *base = f->mem;
    for (int y = 0; y < f->h; y++) {
        uint8_t *row = base + (size_t)y * f->stride;
        const uint32_t *src = f->back + (size_t)y * f->w;
        if (f->bpp == 32) memcpy(row, src, (size_t)f->w * 4);
        else if (f->bpp == 16) {
            uint16_t *d = (uint16_t *)row;
            for (int x = 0; x < f->w; x++) {
                uint32_t p = src[x];
                d[x] = ((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f);
            }
        }
    }
    __builtin___clear_cache((char *)base, (char *)base + (size_t)f->h * f->stride);
    msync(base, (size_t)f->h * f->stride, MS_SYNC);
    layer_commit(f);
}

/* Screen off/on. On this panel neither FBIOBLANK nor the brightness ioctl
 * turns the backlight off, so switch the whole LCD output off like the
 * camera's sleep does: disp2 DISP_DEVICE_SWITCH (0x0F) {screen, type}, with
 * type 0 = none, 1 = LCD (verified on the device: backlight goes dark and
 * the picture comes back on the next layer commit). */
#define DISP_DEVICE_SWITCH 0x0F

void fb_blank(fb_t *f, int blank)
{
    if (f->blanked == blank) return;
    int fd = open("/dev/disp", O_RDWR);
    if (fd >= 0) {
        unsigned long arg[4] = { 0, blank ? 0 : 1, 0, 0 };
        if (ioctl(fd, DISP_DEVICE_SWITCH, arg) < 0) LOGW("disp: output switch %d: %s", !blank, strerror(errno));
        close(fd);
    }
    f->blanked = blank;
    if (!blank) layer_commit(f);   /* push the current picture again */
}

/* Allwinner disp2 driver (/dev/disp). Values taken from the camera's own
 * libhwdisp.so: DISP_LAYER_SET_CONFIG = 0x47, DISP_LAYER_GET_CONFIG = 0x48,
 * struct disp_layer_config is 184 bytes: enable (u8) at 168, channel at 172,
 * layer_id at 176. The ioctl argument is {screen, &config, count, 0}. */
#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48
#define LAYER_CFG_SIZE 184

static int layer_get(int fd, int ch, int id, unsigned char *cfg)
{
    memset(cfg, 0, LAYER_CFG_SIZE);
    *(uint32_t *)(cfg + 172) = ch;
    *(uint32_t *)(cfg + 176) = id;
    unsigned long arg[4] = { 0, (unsigned long)cfg, 1, 0 };
    return ioctl(fd, DISP_LAYER_GET_CONFIG, arg);
}

/* Re-apply the UI layer config (found once: the enabled layer in channels
 * 1..3 that scans fb0's memory). This pushes the new frame to the panel. */
static void layer_commit(fb_t *f)
{
    if (f->disp_fd == 0) {
        f->disp_fd = open("/dev/disp", O_RDWR);
        if (f->disp_fd < 0) { LOGW("disp: open: %s", strerror(errno)); return; }
        f->layer_ch = -1;
        unsigned char cfg[LAYER_CFG_SIZE + 64];
        for (int ch = 1; ch < 4 && f->layer_ch < 0; ch++)
            for (int id = 0; id < 4; id++)
                if (layer_get(f->disp_fd, ch, id, cfg) == 0 && cfg[168]) {
                    f->layer_ch = ch; f->layer_id = id;
                    LOGI("disp: UI layer is ch%d/l%d", ch, id);
                    break;
                }
        if (f->layer_ch < 0) { f->layer_ch = 2; f->layer_id = 0; LOGW("disp: no enabled UI layer, using ch2/l0"); }
    }
    if (f->disp_fd < 0) return;
    unsigned char cfg[LAYER_CFG_SIZE + 64];
    if (layer_get(f->disp_fd, f->layer_ch, f->layer_id, cfg) != 0) return;
    cfg[168] = 1;
    unsigned long arg[4] = { 0, (unsigned long)cfg, 1, 0 };
    if (ioctl(f->disp_fd, DISP_LAYER_SET_CONFIG, arg) < 0 && !f->pan_warned) {
        LOGW("disp: commit: %s", strerror(errno));
        f->pan_warned = 1;
    }
}

void fb_hide_boot_logo(void)
{
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) { LOGW("disp: open: %s", strerror(errno)); return; }
    unsigned char cfg[LAYER_CFG_SIZE + 64];
    char on[128] = "";
    for (int ch = 0; ch < 4; ch++)
        for (int id = 0; id < 4; id++)
            if (layer_get(fd, ch, id, cfg) == 0 && cfg[168]) {
                char s[16]; snprintf(s, sizeof(s), " ch%d/l%d", ch, id);
                strncat(on, s, sizeof(on) - strlen(on) - 1);
            }
    LOGI("disp: enabled layers:%s", on[0] ? on : " none");
    /* the stock app closes channel 0 / layer 0 ("shut down logo") once its UI is up */
    if (layer_get(fd, 0, 0, cfg) == 0 && cfg[168]) {
        cfg[168] = 0;
        unsigned long arg[4] = { 0, (unsigned long)cfg, 1, 0 };
        if (ioctl(fd, DISP_LAYER_SET_CONFIG, arg) == 0) LOGI("disp: boot logo layer ch0/l0 closed");
        else LOGW("disp: closing ch0/l0: %s", strerror(errno));
    }
    close(fd);
}

void fb_commit_layers(void)
{
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) return;
    unsigned char cfg[LAYER_CFG_SIZE + 64];
    for (int ch = 1; ch < 4; ch++)
        for (int id = 0; id < 4; id++)
            if (layer_get(fd, ch, id, cfg) == 0 && cfg[168]) {
                unsigned long arg[4] = { 0, (unsigned long)cfg, 1, 0 };
                int r = ioctl(fd, DISP_LAYER_SET_CONFIG, arg);
                LOGI("disp: re-commit ch%d/l%d -> %d", ch, id, r);
            }
    close(fd);
}

void fb_log_layers(const char *when)
{
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) return;
    unsigned char cfg[LAYER_CFG_SIZE + 64];
    for (int ch = 0; ch < 4; ch++)
        for (int id = 0; id < 4; id++)
            if (layer_get(fd, ch, id, cfg) == 0 && cfg[168]) {
                char hex[LAYER_CFG_SIZE / 4 * 9 + 1], *p = hex;
                for (int i = 0; i < LAYER_CFG_SIZE / 4; i++) p += sprintf(p, "%08x ", ((uint32_t *)cfg)[i]);
                LOGI("disp %s ch%d/l%d: %s", when, ch, id, hex);
            }
    close(fd);
}

void fb_close(fb_t *f)
{
    if (f->fd <= 0) return;
    free(f->back);
    close(f->fd);
    f->fd = -1;
}
