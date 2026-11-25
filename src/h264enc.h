#ifndef H264ENC_H
#define H264ENC_H
#include <stdint.h>
#include <stddef.h>

typedef struct h264enc h264enc_t;

typedef struct {
    int width, height;   /* input == output size */
    int stride;          /* luma bytes per line of the source */
    int fps;
    int bitrate;         /* bit/s */
    int gop;             /* key frame interval in frames */
    int profile;         /* 66 baseline, 77 main, 100 high */
    int level;           /* 31, 40, 41 ... */
} h264enc_cfg_t;

/* Loads /system/lib/libvencoder.so at runtime (no link-time dependency). */
h264enc_t *h264enc_open(const h264enc_cfg_t *cfg);
/* SPS+PPS in Annex-B form (start codes included). */
int  h264enc_headers(h264enc_t *e, const uint8_t **buf, size_t *len);
/* Encode one NV12 frame. On success out/outlen point to an internal Annex-B
 * buffer valid until the next call; *key is set for IDR frames. */
int  h264enc_encode(h264enc_t *e, const uint8_t *nv12, int src_stride, int src_h,
                    int64_t pts_us, const uint8_t **out, size_t *outlen, int *key);
void h264enc_force_key(h264enc_t *e);
/* Optional hook called with the encoder's own copy of each frame (NV12,
 * stride = width) before it is encoded, e.g. to draw a time stamp. */
typedef void (*h264enc_overlay_fn)(void *ctx, uint8_t *y, uint8_t *uv, int stride);
void h264enc_set_overlay(h264enc_t *e, h264enc_overlay_fn fn, void *ctx);
void h264enc_close(h264enc_t *e);

#endif
