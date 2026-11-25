/* camtest: capture N frames, encode with the hardware encoder, write a raw
 * .h264 file. Used to validate capture + encoder on the camera.
 * usage: camtest [-W w] [-H h] [-f fps] [-b kbit] [-n frames] [-o out.h264] [-r raw.nv12] [-v] */
#include "h264enc.h"
#include "log.h"
#include "v4l2cap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    int w = 1280, h = 720, fps = 30, kbit = 3000, n = 90, verbose = 0, opt;
    const char *out = "/dev/cam/test.h264", *raw = NULL;
    while ((opt = getopt(argc, argv, "W:H:f:b:n:o:r:v")) != -1) {
        switch (opt) {
        case 'W': w = atoi(optarg); break;
        case 'H': h = atoi(optarg); break;
        case 'f': fps = atoi(optarg); break;
        case 'b': kbit = atoi(optarg); break;
        case 'n': n = atoi(optarg); break;
        case 'o': out = optarg; break;
        case 'r': raw = optarg; break;
        case 'v': verbose = 1; break;
        default: fprintf(stderr, "bad args\n"); return 2;
        }
    }
    cap_t cap;
    if (cap_open(&cap, "/dev/video0", w, h, fps, verbose) < 0) return 1;
    if (cap_start(&cap) < 0) return 1;

    h264enc_cfg_t ec = { .width = cap.width, .height = cap.height, .stride = cap.bytesperline,
                         .fps = fps, .bitrate = kbit * 1000, .gop = fps * 2, .profile = 77, .level = 40 };
    h264enc_t *enc = h264enc_open(&ec);
    if (!enc) return 1;

    FILE *fo = fopen(out, "wb");
    if (!fo) { LOGE("cannot open %s", out); return 1; }
    double t0 = now_s(), tenc = 0;
    size_t total = 0; int keys = 0, got = 0;
    while (got < n) {
        cap_frame_t f;
        int r = cap_get(&cap, &f, 2000);
        if (r == 0) { LOGW("capture timeout"); continue; }
        if (r < 0) break;
        if (raw && got == 10) {
            FILE *fr = fopen(raw, "wb");
            if (fr) { fwrite(f.data, 1, (size_t)cap.bytesperline * cap.uv_rows * 3 / 2, fr); fclose(fr); }
        }
        const uint8_t *bs; size_t bl; int key;
        double te = now_s();
        int er = h264enc_encode(enc, f.data, cap.bytesperline, cap.uv_rows, f.ts_us, &bs, &bl, &key);
        tenc += now_s() - te;
        cap_put(&cap, &f);
        if (er == 0) { fwrite(bs, 1, bl, fo); total += bl; keys += key; }
        got++;
        if (verbose && got % 30 == 0) LOGI("%d frames", got);
    }
    double dt = now_s() - t0;
    fclose(fo);
    LOGI("done: %d frames in %.2fs = %.1f fps, encode avg %.1f ms, %zu bytes (%.0f kbit/s), %d keyframes",
         got, dt, got / dt, tenc * 1000 / (got ? got : 1), total, total * 8 / dt / 1000, keys);
    h264enc_close(enc);
    cap_close(&cap);
    return 0;
}
