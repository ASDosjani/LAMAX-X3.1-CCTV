#ifndef V4L2CAP_H
#define V4L2CAP_H
#include <stddef.h>
#include <stdint.h>

#define CAP_MAX_BUFS 6

typedef struct {
    int fd;
    int width, height;      /* actual negotiated size */
    int bytesperline;
    int uv_rows;            /* luma rows before the UV plane (height rounded up to 16 if the buffer allows) */
    uint32_t pixfmt;
    int nbufs;
    void *start[CAP_MAX_BUFS];
    size_t length[CAP_MAX_BUFS];
    uint32_t phys[CAP_MAX_BUFS]; /* m.offset (Allwinner: physical address) */
} cap_t;

typedef struct {
    int index;
    void *data;
    uint32_t phys;
    size_t bytesused;
    int64_t ts_us;
} cap_frame_t;

int  cap_open(cap_t *c, const char *dev, int w, int h, int fps, int verbose);
int  cap_start(cap_t *c);
/* timeout_ms < 0: block. returns 1 = frame, 0 = timeout, -1 = error */
int  cap_get(cap_t *c, cap_frame_t *f, int timeout_ms);
int  cap_put(cap_t *c, cap_frame_t *f);
void cap_close(cap_t *c);

#endif
