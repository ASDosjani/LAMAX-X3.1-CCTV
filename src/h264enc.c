/* Hardware H.264 encoder wrapper around the camera's own libvencoder.so
 * (Allwinner CedarX "vencoder" API, see vencoder.h). The library is loaded
 * with dlopen so we don't need it at link time. */
#include "h264enc.h"
#include "log.h"
#include "vencoder.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The camera's libvencoder.so is newer than the public vencoder.h: four index
 * values were inserted before H264Param (found by disassembling
 * H264SetParameter/H264GetParameter in the device library). */
#define IDX_H264PARAM ((VENC_INDEXTYPE)22)
static const int SPSPPS_CANDIDATES[] = { 23, 19, 14, 16, 20, 21, 24 };

static int addr_mapped(const void *p, size_t len)
{
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(pg - 1);
    unsigned char v[4];
    if ((uintptr_t)p < 0x10000) return 0;
    size_t span = ((uintptr_t)p + len) - a;
    if (span > (size_t)pg * 4) span = pg * 4;
    return mincore((void *)a, span, v) == 0;
}

static int looks_like_annexb(const unsigned char *p, unsigned len)
{
    if (len < 8 || len > 512 || !addr_mapped(p, len)) return 0;
    return (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1 && (p[4] & 0x1f) == 7) ||
           (p[0] == 0 && p[1] == 0 && p[2] == 1 && (p[3] & 0x1f) == 7);
}

/* The device library may be a slightly newer build than our header; give every
 * struct we hand to it some slack so a few extra fields can't corrupt memory. */
#define SLACK 256
typedef union { VencInputBuffer b;  unsigned char pad[sizeof(VencInputBuffer) + SLACK]; } inbuf_u;
typedef union { VencOutputBuffer b; unsigned char pad[sizeof(VencOutputBuffer) + SLACK]; } outbuf_u;
typedef union { VencHeaderData b;   unsigned char pad[sizeof(VencHeaderData) + SLACK]; } hdr_u;

typedef struct {
    VideoEncoder *(*Create)(VENC_CODEC_TYPE);
    void (*Destroy)(VideoEncoder *);
    int (*Init)(VideoEncoder *, VencBaseConfig *);
    int (*UnInit)(VideoEncoder *);
    int (*AllocInputBuffer)(VideoEncoder *, VencAllocateBufferParam *);
    int (*GetOneAllocInputBuffer)(VideoEncoder *, VencInputBuffer *);
    int (*FlushCacheAllocInputBuffer)(VideoEncoder *, VencInputBuffer *);
    int (*ReturnOneAllocInputBuffer)(VideoEncoder *, VencInputBuffer *);
    int (*ReleaseAllocInputBuffer)(VideoEncoder *);
    int (*AddOneInputBuffer)(VideoEncoder *, VencInputBuffer *);
    int (*EncodeOneFrame)(VideoEncoder *);
    int (*AlreadyUsedInputBuffer)(VideoEncoder *, VencInputBuffer *);
    int (*GetOneBitstreamFrame)(VideoEncoder *, VencOutputBuffer *);
    int (*FreeOneBitStreamFrame)(VideoEncoder *, VencOutputBuffer *);
    int (*GetParameter)(VideoEncoder *, VENC_INDEXTYPE, void *);
    int (*SetParameter)(VideoEncoder *, VENC_INDEXTYPE, void *);
} venc_api_t;

struct h264enc {
    void *lib;
    venc_api_t api;
    VideoEncoder *enc;
    h264enc_cfg_t cfg;
    uint8_t *hdr; size_t hdrlen;
    uint8_t *out; size_t outcap;
    int force_key;
    h264enc_overlay_fn overlay;
    void *overlay_ctx;
};

#define SYM(name, field) do { \
        *(void **)&e->api.field = dlsym(e->lib, name); \
        if (!e->api.field) { LOGE("venc: missing symbol %s", name); goto fail; } } while (0)

h264enc_t *h264enc_open(const h264enc_cfg_t *cfg)
{
    h264enc_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->cfg = *cfg;
    e->lib = dlopen("libvencoder.so", RTLD_NOW | RTLD_GLOBAL);
    if (!e->lib) { LOGE("venc: dlopen libvencoder.so: %s", dlerror()); goto fail; }
    SYM("VideoEncCreate", Create);
    SYM("VideoEncDestroy", Destroy);
    SYM("VideoEncInit", Init);
    SYM("VideoEncUnInit", UnInit);
    SYM("AllocInputBuffer", AllocInputBuffer);
    SYM("GetOneAllocInputBuffer", GetOneAllocInputBuffer);
    SYM("FlushCacheAllocInputBuffer", FlushCacheAllocInputBuffer);
    SYM("ReturnOneAllocInputBuffer", ReturnOneAllocInputBuffer);
    SYM("ReleaseAllocInputBuffer", ReleaseAllocInputBuffer);
    SYM("AddOneInputBuffer", AddOneInputBuffer);
    SYM("VideoEncodeOneFrame", EncodeOneFrame);
    SYM("AlreadyUsedInputBuffer", AlreadyUsedInputBuffer);
    SYM("GetOneBitstreamFrame", GetOneBitstreamFrame);
    SYM("FreeOneBitStreamFrame", FreeOneBitStreamFrame);
    SYM("VideoEncGetParameter", GetParameter);
    SYM("VideoEncSetParameter", SetParameter);

    e->enc = e->api.Create(VENC_CODEC_H264);
    if (!e->enc) { LOGE("venc: VideoEncCreate failed"); goto fail; }

    VencH264Param p; memset(&p, 0, sizeof(p));
    p.sProfileLevel.nProfile = (VENC_H264PROFILETYPE)(cfg->profile ? cfg->profile : VENC_H264ProfileMain);
    p.sProfileLevel.nLevel = (VENC_H264LEVELTYPE)(cfg->level ? cfg->level : VENC_H264Level4);
    p.bEntropyCodingCABAC = p.sProfileLevel.nProfile != VENC_H264ProfileBaseline;
    p.sQPRange.nMinqp = 10;
    p.sQPRange.nMaxqp = 45;
    p.nFramerate = cfg->fps;
    p.nBitrate = cfg->bitrate;
    p.nMaxKeyInterval = cfg->gop;
    p.nCodingMode = VENC_FRAME_CODING;
    if (e->api.SetParameter(e->enc, IDX_H264PARAM, &p) != 0) LOGW("venc: set H264Param failed");
    int v = 0;
    e->api.SetParameter(e->enc, VENC_IndexParamIfilter, &v);
    e->api.SetParameter(e->enc, VENC_IndexParamRotation, &v);

    VencBaseConfig bc; memset(&bc, 0, sizeof(bc));
    bc.nInputWidth = cfg->width;
    bc.nInputHeight = cfg->height;
    bc.nDstWidth = cfg->width;
    bc.nDstHeight = cfg->height;
    bc.nStride = cfg->width;
    bc.eInputFormat = VENC_PIXEL_YUV420SP; /* NV12 */
    if (e->api.Init(e->enc, &bc) != 0) { LOGE("venc: VideoEncInit failed"); goto fail; }

    hdr_u h;
    int found = -1;
    for (unsigned i = 0; i < sizeof(SPSPPS_CANDIDATES) / sizeof(SPSPPS_CANDIDATES[0]); i++) {
        memset(&h, 0, sizeof(h));
        int r = e->api.GetParameter(e->enc, (VENC_INDEXTYPE)SPSPPS_CANDIDATES[i], &h.b);
        unsigned *w = (unsigned *)h.pad;
        LOGI("venc: GetParameter(%d) = %d  [%08x %08x %08x %08x]", SPSPPS_CANDIDATES[i], r, w[0], w[1], w[2], w[3]);
        if (r == 0 && looks_like_annexb(h.b.pBuffer, h.b.nLength)) { found = SPSPPS_CANDIDATES[i]; break; }
    }
    if (found < 0) { LOGE("venc: cannot read SPS/PPS"); goto fail; }
    LOGI("venc: SPS/PPS index = %d", found);
    e->hdrlen = h.b.nLength;
    e->hdr = malloc(e->hdrlen);
    memcpy(e->hdr, h.b.pBuffer, e->hdrlen);

    VencAllocateBufferParam bp; memset(&bp, 0, sizeof(bp));
    bp.nBufferNum = 2;
    bp.nSizeY = cfg->width * cfg->height;
    bp.nSizeC = cfg->width * cfg->height / 2;
    if (e->api.AllocInputBuffer(e->enc, &bp) != 0) { LOGE("venc: AllocInputBuffer failed"); goto fail; }

    e->outcap = 512 * 1024;
    e->out = malloc(e->outcap);
    LOGI("venc: H.264 %dx%d %dfps %dkbit/s gop=%d profile=%d level=%d, header %u bytes",
         cfg->width, cfg->height, cfg->fps, cfg->bitrate / 1000, cfg->gop,
         p.sProfileLevel.nProfile, p.sProfileLevel.nLevel, (unsigned)e->hdrlen);
    return e;
fail:
    h264enc_close(e);
    return NULL;
}

int h264enc_headers(h264enc_t *e, const uint8_t **buf, size_t *len)
{
    *buf = e->hdr; *len = e->hdrlen;
    return 0;
}

void h264enc_force_key(h264enc_t *e) { e->force_key = 1; }

static int out_append(h264enc_t *e, size_t *pos, const void *p, size_t n)
{
    if (!n) return 0;
    if (*pos + n > e->outcap) {
        size_t nc = (*pos + n) * 2;
        uint8_t *q = realloc(e->out, nc);
        if (!q) return -1;
        e->out = q; e->outcap = nc;
    }
    memcpy(e->out + *pos, p, n);
    *pos += n;
    return 0;
}

void h264enc_set_overlay(h264enc_t *e, h264enc_overlay_fn fn, void *ctx)
{
    e->overlay = fn; e->overlay_ctx = ctx;
}

int h264enc_encode(h264enc_t *e, const uint8_t *nv12, int src_stride, int src_h,
                   int64_t pts_us, const uint8_t **out, size_t *outlen, int *key)
{
    const int w = e->cfg.width, h = e->cfg.height;
    inbuf_u in; memset(&in, 0, sizeof(in));
    if (e->api.GetOneAllocInputBuffer(e->enc, &in.b) != 0) { LOGE("venc: no input buffer"); return -1; }

    /* copy NV12 (Y plane then interleaved UV plane) into the encoder buffer */
    const uint8_t *sy = nv12, *suv = nv12 + (size_t)src_stride * src_h;
    if (src_stride == w) {
        memcpy(in.b.pAddrVirY, sy, (size_t)w * h);
        memcpy(in.b.pAddrVirC, suv, (size_t)w * h / 2);
    } else {
        for (int y = 0; y < h; y++) memcpy(in.b.pAddrVirY + (size_t)y * w, sy + (size_t)y * src_stride, w);
        for (int y = 0; y < h / 2; y++) memcpy(in.b.pAddrVirC + (size_t)y * w, suv + (size_t)y * src_stride, w);
    }
    if (e->overlay) e->overlay(e->overlay_ctx, in.b.pAddrVirY, in.b.pAddrVirC, w);
    in.b.nPts = pts_us;
    in.b.bEnableCorp = 0;
    e->api.FlushCacheAllocInputBuffer(e->enc, &in.b);

    if (e->force_key) {
        int one = 1;
        e->api.SetParameter(e->enc, VENC_IndexParamForceKeyFrame, &one);
        e->force_key = 0;
    }

    e->api.AddOneInputBuffer(e->enc, &in.b);
    int r = e->api.EncodeOneFrame(e->enc);
    e->api.AlreadyUsedInputBuffer(e->enc, &in.b);
    e->api.ReturnOneAllocInputBuffer(e->enc, &in.b);
    if (r != VENC_RESULT_OK) { LOGW("venc: encode result %d", r); return -1; }

    outbuf_u ob; memset(&ob, 0, sizeof(ob));
    if (e->api.GetOneBitstreamFrame(e->enc, &ob.b) != 0) { LOGW("venc: no bitstream"); return -1; }
    size_t pos = 0;
    int k = (ob.b.nFlag & VENC_BUFFERFLAG_KEYFRAME) != 0;
    if (k) out_append(e, &pos, e->hdr, e->hdrlen); /* repeat SPS/PPS before every IDR */
    out_append(e, &pos, ob.b.pData0, ob.b.nSize0);
    if (ob.b.pData1 && ob.b.nSize1) out_append(e, &pos, ob.b.pData1, ob.b.nSize1);
    e->api.FreeOneBitStreamFrame(e->enc, &ob.b);

    *out = e->out; *outlen = pos; *key = k;
    return 0;
}

void h264enc_close(h264enc_t *e)
{
    if (!e) return;
    if (e->enc) {
        if (e->api.ReleaseAllocInputBuffer) e->api.ReleaseAllocInputBuffer(e->enc);
        if (e->api.UnInit) e->api.UnInit(e->enc);
        if (e->api.Destroy) e->api.Destroy(e->enc);
    }
    if (e->lib) dlclose(e->lib);
    free(e->hdr);
    free(e->out);
    free(e);
}
