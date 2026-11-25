/* camd: camera -> hardware H.264 -> RTSP server.
 * usage: camd [config.txt]   (default /mnt/extsd/cam/config.txt)
 * Writes a one-line status file (/dev/cam/camd.status) for camui. */
#include "config.h"
#include "h264enc.h"
#include "log.h"
#include "osd.h"
#include "rtsp.h"
#include "v4l2cap.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <linux/watchdog.h>

#define STATUS_FILE "/dev/cam/camd.status"

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { g_stop = 1; }

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void set_ctrl(int fd, unsigned id, int val, const char *name)
{
    struct v4l2_control c = { .id = id, .value = val };
    if (ioctl(fd, VIDIOC_S_CTRL, &c) < 0) LOGW("camera: %s=%d not supported (%s)", name, val, strerror(errno));
    else LOGI("camera: %s=%d", name, val);
}

static int profile_of(const char *p)
{
    if (!strcasecmp(p, "baseline")) return 66;
    if (!strcasecmp(p, "high")) return 100;
    return 77;
}

static void write_status(const char *s)
{
    FILE *f = fopen(STATUS_FILE ".tmp", "w");
    if (!f) return;
    fputs(s, f);
    fclose(f);
    rename(STATUS_FILE ".tmp", STATUS_FILE);
}

int main(int argc, char **argv)
{
    const char *cfgpath = argc > 1 ? argv[1] : "/mnt/extsd/cam/config.txt";
    config_t *cfg = config_load(cfgpath);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);

    int w = config_int(cfg, "VIDEO_WIDTH", 1920);
    int h = config_int(cfg, "VIDEO_HEIGHT", 1080);
    int fps = config_int(cfg, "VIDEO_FPS", 25);
    int kbps = config_int(cfg, "VIDEO_BITRATE_KBPS", 4000);
    int gop_s = config_int(cfg, "VIDEO_GOP_SECONDS", 2);
    int flip = config_int(cfg, "VIDEO_FLIP", 0);
    int use_wd = config_int(cfg, "WATCHDOG", 1);
    if (fps < 1 || fps > 30) fps = 25;
    if (kbps < 200) kbps = 200;
    if (kbps > 12000) kbps = 12000;

    /* watchdog: the stock sdv app normally feeds it; we take over. If camd
     * hangs or crashes the camera reboots and autorun.sh starts us again. */
    int wd = -1;
    if (use_wd) {
        wd = open("/dev/watchdog", O_WRONLY);
        if (wd < 0) LOGW("watchdog: %s", strerror(errno));
        else {
            int t = 0; ioctl(wd, WDIOC_GETTIMEOUT, &t);
            LOGI("watchdog: enabled, timeout %ds", t);
        }
    }

    cap_t cap;
    int tries = 0;
    /* The vfe driver picks the sensor mode from the requested frame rate: 30 fps
     * gives the normal 1080p/720p modes, other values may select high-speed
     * binned modes. So always capture at 30 and drop frames to VIDEO_FPS. */
    while (cap_open(&cap, "/dev/video0", w, h, 30, 0) < 0) {
        if (++tries > 20 || g_stop) { LOGE("camera unavailable, giving up"); return 1; }
        if (wd >= 0) write(wd, "1", 1);
        sleep(1);
    }
    if (flip) {
        set_ctrl(cap.fd, V4L2_CID_HFLIP, 1, "hflip");
        set_ctrl(cap.fd, V4L2_CID_VFLIP, 1, "vflip");
    }
    if (cap_start(&cap) < 0) return 1;

    h264enc_cfg_t ec = {
        .width = cap.width, .height = cap.height, .stride = cap.bytesperline, .fps = fps,
        .bitrate = kbps * 1000, .gop = fps * (gop_s > 0 ? gop_s : 2),
        .profile = profile_of(config_get(cfg, "VIDEO_PROFILE", "main")),
        .level = cap.width * cap.height > 1280 * 720 ? 40 : 31,
    };
    h264enc_t *enc = h264enc_open(&ec);
    if (!enc) return 1;

    /* date/time burned into the picture (the clock is set by "camnet ntp") */
    osd_t *osd = NULL;
    if (config_int(cfg, "OSD_TIME", 1)) {
        setenv("TZ", config_get(cfg, "TIMEZONE", "CET-1CEST,M3.5.0,M10.5.0/3"), 1);
        tzset();
        osd = osd_create(cap.width, cap.height, config_get(cfg, "OSD_POSITION", "top-left"),
                         config_int(cfg, "OSD_NAME", 0) ? config_get(cfg, "CAMERA_NAME", "") : NULL);
        if (osd) h264enc_set_overlay(enc, (h264enc_overlay_fn)osd_draw, osd);
        LOGI("osd: time stamp %s, TZ=%s", config_get(cfg, "OSD_POSITION", "top-left"), getenv("TZ"));
    }

    rtsp_cfg_t rc = {
        .port = config_int(cfg, "RTSP_PORT", 554),
        .path = config_get(cfg, "RTSP_PATH", "stream"),
        .user = config_get(cfg, "RTSP_USER", ""),
        .password = config_get(cfg, "RTSP_PASSWORD", ""),
        .name = config_get(cfg, "CAMERA_NAME", "LAMAX"),
        .fps = fps,
        .max_clients = config_int(cfg, "RTSP_MAX_CLIENTS", 4),
    };
    rtsp_server_t *srv = rtsp_create(&rc);
    if (!srv) return 1;
    const uint8_t *hd; size_t hl;
    h264enc_headers(enc, &hd, &hl);
    rtsp_set_headers(srv, hd, hl);

    if (cap.width != w || cap.height != h)
        LOGW("camera: requested %dx%d, got %dx%d", w, h, cap.width, cap.height);
    int64_t last_slot = -1;
    double last_wd = 0, last_stat = now_s();
    int frames = 0, encoded = 0;
    uint64_t sent_bytes = 0;
    char status[256];
    while (!g_stop) {
        struct pollfd pf[16];
        pf[0].fd = cap.fd; pf[0].events = POLLIN; pf[0].revents = 0;
        int n = 1 + rtsp_pollfds(srv, pf + 1, 15);
        int r = poll(pf, n, 1000);
        double t = now_s();
        if (wd >= 0 && t - last_wd >= 2) { write(wd, "1", 1); last_wd = t; }
        if (r < 0 && errno != EINTR) break;
        if (r > 0) rtsp_process(srv, pf + 1, n - 1);

        if (pf[0].revents & POLLIN) {
            cap_frame_t f;
            if (cap_get(&cap, &f, 0) == 1) {
                frames++;
                int playing = rtsp_playing(srv);
                /* frame rate limiter: keep one frame per 1/fps time slot */
                int64_t slot = (f.ts_us + 5000) * fps / 1000000;
                if (playing && slot != last_slot) {
                    last_slot = slot;
                    if (rtsp_want_keyframe(srv)) h264enc_force_key(enc);
                    const uint8_t *bs; size_t bl; int key;
                    if (h264enc_encode(enc, f.data, cap.bytesperline, cap.uv_rows, f.ts_us, &bs, &bl, &key) == 0) {
                        rtsp_send_frame(srv, bs, bl, key, f.ts_us);
                        encoded++;
                    }
                }
                cap_put(&cap, &f);
            }
        }

        if (t - last_stat >= 2) {
            double dt = t - last_stat;
            sent_bytes = rtsp_take_bytes(srv);
            snprintf(status, sizeof(status), "res=%dx%d cap_fps=%.1f enc_fps=%.1f kbps=%.0f clients=%d port=%d path=%s\n",
                     cap.width, cap.height, frames / dt, encoded / dt, sent_bytes * 8 / dt / 1000,
                     rtsp_playing(srv), rc.port, rc.path);
            write_status(status);
            frames = encoded = 0;
            last_stat = t;
        }
    }
    LOGI("camd: stopping");
    rtsp_destroy(srv);
    h264enc_close(enc);
    osd_free(osd);
    cap_close(&cap);
    if (wd >= 0) { write(wd, "V", 1); close(wd); }  /* magic close: disarm on clean exit */
    unlink(STATUS_FILE);
    config_free(cfg);
    return 0;
}
