#ifndef FB_H
#define FB_H
#include <stdint.h>
#include <linux/fb.h>

/* Tiny framebuffer canvas for the camera's 2" LCD (/dev/graphics/fb0).
 * Draws into a back buffer; fb_present() copies it to the visible page. */
typedef struct {
    int fd;
    int w, h, bpp, stride;
    uint8_t *mem;      /* mmap of the device */
    uint32_t *back;    /* w*h ARGB back buffer */
    int blanked;
    int pan_warned;
    int disp_fd;       /* /dev/disp, 0 = not opened yet */
    int layer_ch, layer_id;
    int brightness;    /* saved LCD brightness while the screen is off */
    int bl_logged;
    struct fb_var_screeninfo var;
} fb_t;

#define RGB(r, g, b) (0xff000000u | ((r) << 16) | ((g) << 8) | (b))
#define C_BLACK  RGB(0, 0, 0)
#define C_WHITE  RGB(255, 255, 255)
#define C_GRAY   RGB(140, 140, 140)
#define C_DGRAY  RGB(50, 50, 50)
#define C_GREEN  RGB(40, 200, 70)
#define C_YELLOW RGB(240, 200, 40)
#define C_ORANGE RGB(255, 130, 30)
#define C_RED    RGB(230, 50, 40)
#define C_BLUE   RGB(40, 120, 230)

int  fb_open(fb_t *f, const char *dev);
void fb_clear(fb_t *f, uint32_t c);
void fb_rect(fb_t *f, int x, int y, int w, int h, uint32_t c);
/* UTF-8 text (Hungarian accents mapped to the CP437 font); scale 1 or 2.
 * Returns the x after the last character. */
int  fb_text(fb_t *f, int x, int y, const char *utf8, uint32_t fg, int scale);
int  fb_text_width(const char *utf8, int scale);
void fb_present(fb_t *f);
void fb_blank(fb_t *f, int blank);
void fb_close(fb_t *f);
/* Close the boot-logo display layer (normally done by the stock app). */
void fb_hide_boot_logo(void);
/* Debug: log the raw config of every enabled display layer. */
void fb_log_layers(const char *when);
/* Write back the config of enabled UI layers to force the display engine to apply it. */
void fb_commit_layers(void);

#endif
