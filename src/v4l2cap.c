/* V4L2 capture for the Allwinner V3 "vfe_v4l2" driver (sun8i, kernel 3.4). */
#include "v4l2cap.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

/* Allwinner private capture modes (sunxi videodev2.h) */
#ifndef V4L2_MODE_VIDEO
#define V4L2_MODE_VIDEO   0x0002
#define V4L2_MODE_IMAGE   0x0003
#define V4L2_MODE_PREVIEW 0x0004
#endif

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static void fourcc_str(uint32_t f, char out[5])
{
    out[0] = f & 0xff; out[1] = (f >> 8) & 0xff; out[2] = (f >> 16) & 0xff; out[3] = (f >> 24) & 0xff; out[4] = 0;
}

int cap_open(cap_t *c, const char *dev, int w, int h, int fps, int verbose)
{
    memset(c, 0, sizeof(*c));
    c->fd = open(dev, O_RDWR | O_NONBLOCK);
    if (c->fd < 0) { LOGE("open %s: %s", dev, strerror(errno)); return -1; }

    struct v4l2_capability cap;
    if (xioctl(c->fd, VIDIOC_QUERYCAP, &cap) == 0)
        LOGI("v4l2: driver=%s card=%s caps=0x%x", cap.driver, cap.card, cap.capabilities);

    struct v4l2_input inp; memset(&inp, 0, sizeof(inp));
    inp.index = 0;
    if (xioctl(c->fd, VIDIOC_S_INPUT, &inp) < 0) LOGW("VIDIOC_S_INPUT: %s", strerror(errno));

    if (verbose) {
        struct v4l2_fmtdesc fd; memset(&fd, 0, sizeof(fd));
        fd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (fd.index = 0; xioctl(c->fd, VIDIOC_ENUM_FMT, &fd) == 0; fd.index++) {
            char s[5]; fourcc_str(fd.pixelformat, s);
            LOGI("v4l2: fmt[%d] %s (%s)", fd.index, s, fd.description);
            struct v4l2_frmsizeenum fs; memset(&fs, 0, sizeof(fs));
            fs.pixel_format = fd.pixelformat;
            for (fs.index = 0; xioctl(c->fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0 && fs.index < 32; fs.index++) {
                if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                    LOGI("v4l2:    size %ux%u", fs.discrete.width, fs.discrete.height);
                else { LOGI("v4l2:    size %u..%u x %u..%u", fs.stepwise.min_width, fs.stepwise.max_width,
                            fs.stepwise.min_height, fs.stepwise.max_height); break; }
            }
        }
    }

    struct v4l2_streamparm parm; memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.capturemode = V4L2_MODE_VIDEO;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    if (xioctl(c->fd, VIDIOC_S_PARM, &parm) < 0) LOGW("VIDIOC_S_PARM: %s", strerror(errno));

    struct v4l2_format fmt; memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) { LOGE("VIDIOC_S_FMT: %s", strerror(errno)); goto fail; }
    if (xioctl(c->fd, VIDIOC_G_FMT, &fmt) < 0) LOGW("VIDIOC_G_FMT: %s", strerror(errno));
    c->width = fmt.fmt.pix.width;
    c->height = fmt.fmt.pix.height;
    c->bytesperline = fmt.fmt.pix.bytesperline ? (int)fmt.fmt.pix.bytesperline : c->width;
    c->pixfmt = fmt.fmt.pix.pixelformat;
    { char s[5]; fourcc_str(c->pixfmt, s);
      LOGI("v4l2: format %dx%d %s bpl=%d size=%u", c->width, c->height, s, c->bytesperline, fmt.fmt.pix.sizeimage); }

    struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof(rb));
    rb.count = 4; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_REQBUFS, &rb) < 0) { LOGE("VIDIOC_REQBUFS: %s", strerror(errno)); goto fail; }
    c->nbufs = rb.count > CAP_MAX_BUFS ? CAP_MAX_BUFS : (int)rb.count;
    for (int i = 0; i < c->nbufs; i++) {
        struct v4l2_buffer b; memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) { LOGE("VIDIOC_QUERYBUF: %s", strerror(errno)); goto fail; }
        c->length[i] = b.length;
        c->phys[i] = b.m.offset;
        c->start[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, b.m.offset);
        if (c->start[i] == MAP_FAILED) { c->start[i] = NULL; LOGE("mmap: %s", strerror(errno)); goto fail; }
        if (verbose) LOGI("v4l2: buf %d len=%u offset=0x%08x", i, b.length, b.m.offset);
    }
    /* The vfe driver places the UV plane after a 16-aligned number of luma
     * rows (1080 -> 1088); using 1080 shifts the chroma (green band on top). */
    {
        int al = (c->height + 15) & ~15;
        size_t need = (size_t)c->bytesperline * al * 3 / 2;
        c->uv_rows = (c->length[0] >= need) ? al : c->height;
        LOGI("v4l2: buffer %u bytes, UV plane after %d rows", (unsigned)c->length[0], c->uv_rows);
    }
    return 0;
fail:
    cap_close(c);
    return -1;
}

int cap_start(cap_t *c)
{
    for (int i = 0; i < c->nbufs; i++) {
        struct v4l2_buffer b; memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0) { LOGE("VIDIOC_QBUF: %s", strerror(errno)); return -1; }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(c->fd, VIDIOC_STREAMON, &type) < 0) { LOGE("VIDIOC_STREAMON: %s", strerror(errno)); return -1; }
    return 0;
}

int cap_get(cap_t *c, cap_frame_t *f, int timeout_ms)
{
    struct pollfd p = { .fd = c->fd, .events = POLLIN };
    int r = poll(&p, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;
    struct v4l2_buffer b; memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_DQBUF, &b) < 0) {
        if (errno == EAGAIN) return 0;
        LOGE("VIDIOC_DQBUF: %s", strerror(errno));
        return -1;
    }
    f->index = b.index;
    f->data = c->start[b.index];
    f->phys = c->phys[b.index];
    f->bytesused = b.bytesused;
    f->ts_us = (int64_t)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
    return 1;
}

int cap_put(cap_t *c, cap_frame_t *f)
{
    struct v4l2_buffer b; memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = f->index;
    return xioctl(c->fd, VIDIOC_QBUF, &b);
}

void cap_close(cap_t *c)
{
    if (c->fd <= 0) return;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(c->fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < c->nbufs; i++)
        if (c->start[i]) munmap(c->start[i], c->length[i]);
    close(c->fd);
    c->fd = -1;
}
