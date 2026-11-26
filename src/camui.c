/* camui: the camera's own screen and buttons.
 *  - Status page: WiFi network, live signal strength (for finding a good
 *    mounting spot), IP address, RTSP clients and bitrate.
 *  - WiFi scan page: nearby networks sorted by signal strength, refreshed
 *    every 10 s; results are also written to the SD card (cam/wifi_scan.txt).
 * Buttons: any button wakes the screen; a non-power button switches between
 * the status and scan pages; power short press = screen off, hold 3 s = power off.
 * usage: camui [config.txt] */
#include "config.h"
#include "fb.h"
#include "log.h"
#include "wpactrl.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WPA_CTRL   "/dev/cam/wpa/wlan0"
#define CAMD_STAT  "/dev/cam/camd.status"
#define SCAN_FILE  "/mnt/extsd/cam/wifi_scan.txt"
#define MAX_INPUTS 8
#define MAX_NETS   48

enum { PAGE_STATUS, PAGE_SCAN };

typedef struct { char ssid[33]; int level; int freq; int count; char flags[64]; double seen; } net_t;

#define SCAN_WAIT   8.0   /* s from SCAN to SCAN_RESULTS: WEXT drops the old list when a scan starts */
#define SCAN_EVERY 12.0   /* s between scans */
#define SCAN_KEEP  35.0   /* s a network stays listed after it was last seen (covers incomplete scans) */

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { g_stop = 1; }

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* ------------------------------------------------------------- helpers */

static int quality(int dbm)
{
    if (dbm == 0) return -1;
    if (dbm >= -55) return 4;
    if (dbm >= -65) return 3;
    if (dbm >= -72) return 2;
    if (dbm >= -80) return 1;
    return 0;
}
static const char *quality_text(int q)
{
    static const char *t[] = { "Very weak", "Weak", "Fair", "Good", "Excellent" };
    return q < 0 ? "-" : t[q];
}
static uint32_t quality_color(int q)
{
    static const uint32_t c[] = { C_RED, C_ORANGE, C_YELLOW, C_GREEN, C_GREEN };
    return q < 0 ? C_GRAY : c[q];
}
static int freq_to_ch(int f)
{
    if (f >= 2412 && f <= 2472) return (f - 2407) / 5;
    if (f == 2484) return 14;
    if (f >= 5000) return (f - 5000) / 5;
    return 0;
}

static void bars(fb_t *fb, int x, int y, int q, int scale)
{
    for (int i = 0; i < 4; i++) {
        int h = (4 + i * 4) * scale, w = 4 * scale;
        fb_rect(fb, x + i * (w + scale * 2), y + 16 * scale - h, w, h, i < q ? quality_color(q) : C_DGRAY);
    }
}

static int iface_ip(const char *ifn, char *out, size_t n)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq r; memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", ifn);
    int ok = ioctl(s, SIOCGIFADDR, &r) == 0;
    close(s);
    if (!ok) { out[0] = 0; return -1; }
    snprintf(out, n, "%s", inet_ntoa(((struct sockaddr_in *)&r.ifr_addr)->sin_addr));
    return 0;
}

/* signal level in dBm from /proc/net/wireless (WEXT) */
static int proc_wireless_level(void)
{
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) return 0;
    char line[256]; int lvl = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "wlan0:");
        if (!p) continue;
        int st; float link, level;
        if (sscanf(p + 6, "%x %f %f", &st, &link, &level) == 3) {
            lvl = (int)level;
            if (lvl > 0) lvl -= 256;
        }
    }
    fclose(f);
    return lvl;
}

static void kv(const char *text, const char *key, char *out, size_t n)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            size_t l = strcspn(p + kl + 1, "\n ");
            if (l >= n) l = n - 1;
            memcpy(out, p + kl + 1, l); out[l] = 0;
            return;
        }
    }
}

/* ------------------------------------------------------------- scanning */

static int cmp_net(const void *a, const void *b) { return ((const net_t *)b)->level - ((const net_t *)a)->level; }

static int scan_results(wpactrl_t *w, net_t *nets, int max)
{
    static char buf[16384];
    if (wpactrl_cmd(w, "SCAN_RESULTS", buf, sizeof(buf), 3000) <= 0) return -1;
    int n = 0;
    char *save, *line = strtok_r(buf, "\n", &save);
    line = strtok_r(NULL, "\n", &save); /* skip header */
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        char bssid[32], flags[128] = "", ssid[64] = "";
        int freq, level;
        /* bssid \t freq \t level \t flags \t ssid */
        char *f[5] = { 0 }; int k = 0;
        for (char *p = line; k < 5; ) { f[k++] = p; p = strchr(p, '\t'); if (!p) break; *p++ = 0; }
        if (k < 4) continue;
        snprintf(bssid, sizeof(bssid), "%s", f[0]);
        freq = atoi(f[1]); level = atoi(f[2]);
        snprintf(flags, sizeof(flags), "%s", f[3]);
        if (k == 5) snprintf(ssid, sizeof(ssid), "%s", f[4]);
        int j;
        for (j = 0; j < n; j++) if (!strcmp(nets[j].ssid, ssid)) break;  /* one row per SSID */
        if (j < n) {
            nets[j].count++;
            if (level > nets[j].level) { nets[j].level = level; nets[j].freq = freq; }
            continue;
        }
        if (n >= max) continue;
        memset(&nets[n], 0, sizeof(net_t));
        snprintf(nets[n].ssid, sizeof(nets[n].ssid), "%s", ssid);
        snprintf(nets[n].flags, sizeof(nets[n].flags), "%s", flags);
        nets[n].level = level; nets[n].freq = freq; nets[n].count = 1;
        n++;
    }
    qsort(nets, n, sizeof(net_t), cmp_net);
    return n;
}

/* Merge a fresh scan into the list shown on screen: update / add what was seen
 * now, drop what has not been seen for SCAN_KEEP seconds. A scan that returns
 * only part of the networks (common with WEXT) then does not empty the list. */
static int merge_scan(net_t *list, int n, const net_t *fresh, int nf, double now)
{
    for (int i = 0; i < nf; i++) {
        int j;
        for (j = 0; j < n; j++) if (!strcmp(list[j].ssid, fresh[i].ssid)) break;
        if (j == n) {
            if (n >= MAX_NETS) continue;
            n++;
        }
        list[j] = fresh[i];
        list[j].seen = now;
    }
    int k = 0;
    for (int j = 0; j < n; j++)
        if (now - list[j].seen <= SCAN_KEEP) list[k++] = list[j];
    qsort(list, k, sizeof(net_t), cmp_net);
    return k;
}

static void save_scan(const net_t *nets, int n, const char *name)
{
    FILE *f = fopen(SCAN_FILE, "w");
    if (!f) return;
    time_t t = time(NULL);
    fprintf(f, "# WiFi scan, camera %s, %s", name, ctime(&t));
    fprintf(f, "# signal_dBm  quality        channel  APs  security                  ssid\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "%5d        %-14s %4d     %3d  %-25.25s %s\n", nets[i].level, quality_text(quality(nets[i].level)),
                freq_to_ch(nets[i].freq), nets[i].count, nets[i].flags, nets[i].ssid[0] ? nets[i].ssid : "<hidden>");
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* ------------------------------------------------------------- drawing */

static void header(fb_t *fb, const char *title, const char *right)
{
    fb_rect(fb, 0, 0, fb->w, 18, C_BLUE);
    fb_text(fb, 4, 1, title, C_WHITE, 1);
    if (right) fb_text(fb, fb->w - fb_text_width(right, 1) - 4, 1, right, C_WHITE, 1);
}

/* CPU load since the previous call, from /proc/stat (busy / total jiffies).
 * The first call has no reference and returns -1. */
static int cpu_percent(void)
{
    static unsigned long long last_busy, last_total;
    unsigned long long v[8] = { 0 };
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return -1;
    unsigned long long idle = v[3] + v[4], total = 0;          /* idle + iowait */
    for (int i = 0; i < 8; i++) total += v[i];
    unsigned long long busy = total - idle;
    int pct = -1;
    if (last_total && total > last_total)
        pct = (int)((busy - last_busy) * 100 / (total - last_total));
    last_busy = busy; last_total = total;
    return pct;
}

/* Temperature of the AXP209 power-management chip in tenths of °C, or -1000.
 * Its internal ADC (registers 0x5E/0x5F, 12 bit, 0.1 °C per step, -144.7 °C
 * offset) is read through the axp driver's axp20_regs attribute: writing a
 * value below 0x100 only selects the register pair, reading returns
 * "REG[0x5e]=0x7e,REG[0x5f]=0x4". The SoC's own sensor is not usable. */
static int pmic_temp_dc(void)
{
    const char *path = "/sys/bus/i2c/devices/0-0034/axp20_regs";
    FILE *f = fopen(path, "w");
    if (!f) return -1000;
    fputs("5e", f);
    fclose(f);
    f = fopen(path, "r");
    if (!f) return -1000;
    unsigned hi = 0, lo = 0, a, b;
    int n = fscanf(f, "REG[0x%x]=0x%x,REG[0x%x]=0x%x", &a, &hi, &b, &lo);
    fclose(f);
    if (n != 4 || a != 0x5e) return -1000;
    int raw = (int)((hi << 4) | (lo & 0xf));
    return raw - 1447;
}

/* Memory in use (without buffers / page cache), in percent of MemTotal. */
static int ram_percent(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char key[32]; unsigned long val, total = 0, free_ = 0, buf = 0, cached = 0;
    while (fscanf(f, "%31s %lu kB", key, &val) == 2) {
        if (!strcmp(key, "MemTotal:")) total = val;
        else if (!strcmp(key, "MemFree:")) free_ = val;
        else if (!strcmp(key, "Buffers:")) buf = val;
        else if (!strcmp(key, "Cached:")) cached = val;
    }
    fclose(f);
    if (!total) return -1;
    unsigned long used = total - free_ - buf - cached;
    return (int)(used * 100 / total);
}

static int read_int_file(const char *p, int def)
{
    FILE *f = fopen(p, "r");
    if (!f) return def;
    int v = def;
    if (fscanf(f, "%d", &v) != 1) v = def;
    fclose(f);
    return v;
}

/* BATTERY_LONG_LIFE: switch off before the cell is deeply discharged (the charger
 * chip itself only cuts off at 2.9 V). Low = on battery and at most LOW_PCT or below
 * LOW_UV; it has to stay low for LOW_SECS so a short load peak does not count. */
#ifndef LOW_PCT                     /* overridable for testing: -DLOW_PCT=95 */
#define LOW_PCT  10
#endif
#define LOW_UV   3450000
#define LOW_SECS 30
static int battery_low(void)
{
    if (read_int_file("/sys/class/power_supply/battery/present", 1) == 0) return 0;
    if (read_int_file("/sys/class/power_supply/usb/online", 0) || read_int_file("/sys/class/power_supply/ac/online", 0))
        return 0;
    int pct = read_int_file("/sys/class/power_supply/battery/capacity", -1);
    int uv = read_int_file("/sys/class/power_supply/battery/voltage_now", 0);
    return (pct >= 0 && pct <= LOW_PCT) || (uv > 0 && uv < LOW_UV);
}

/* Battery voltage, icon and percent at the right edge of the header:
 *   "4.11V [###  ] 99%▲"   charging
 *   "4.05V [####|] 86%LIM" long-life limit reached: on external power, charging stopped
 * With BATTERY_LONG_LIFE=1 a white tick in the icon marks where charging stops.
 * Colours: cyan on external power; on battery green / yellow / red by level. */
static void draw_battery(fb_t *fb, int long_life)
{
    if (read_int_file("/sys/class/power_supply/battery/present", 1) == 0) {
        /* no battery: running on external power only -> plug icon + "DC" */
        uint32_t c = RGB(80, 220, 255);
        const char *txt = "DC";
        int x = fb->w - 4 - fb_text_width(txt, 1);
        fb_text(fb, x, 1, txt, c, 1);
        int px = x - 22, py = 3;
        fb_rect(fb, px, py + 2, 3, 2, c);            /* prongs */
        fb_rect(fb, px, py + 8, 3, 2, c);
        fb_rect(fb, px + 3, py, 8, 12, c);           /* plug body */
        fb_rect(fb, px + 11, py + 5, 7, 2, c);       /* cable */
        return;
    }
    int pct = read_int_file("/sys/class/power_supply/battery/capacity", -1);
    if (pct < 0) return;
    if (pct > 100) pct = 100;
    char st[24] = "";
    FILE *f = fopen("/sys/class/power_supply/battery/status", "r");
    if (f) { if (!fgets(st, sizeof(st), f)) st[0] = 0; fclose(f); }
    int charging = !strncmp(st, "Charging", 8);
    int ext = read_int_file("/sys/class/power_supply/usb/online", 0) || read_int_file("/sys/class/power_supply/ac/online", 0);
    uint32_t c = (charging || ext) ? RGB(80, 220, 255) : pct > 50 ? C_GREEN : pct > 20 ? C_YELLOW : C_RED;
    int uv = read_int_file("/sys/class/power_supply/battery/voltage_now", 0);
    /* long-life mode on external power and not charging: charging stopped at 4.10 V and the
     * charger only restarts it once the cell has dropped well below that (it then sits at
     * ~4.05 V, running from external power) - that is the limit, not a fault */
    int at_limit = long_life && ext && !charging;

    char txt[16];
    snprintf(txt, sizeof(txt), "%d%%%s", pct,
             at_limit ? "LIM" : charging ? "\xe2\x96\xb2" : "");                /* ▲ = charging */
    int x = fb->w - 4 - fb_text_width(txt, 1);
    fb_text(fb, x, 1, txt, at_limit ? C_YELLOW : c, 1);
    /* icon: 20x10 body + 2x4 tip, left of the text */
    int bx = x - 27, by = 4;
    fb_rect(fb, bx, by, 20, 10, c);
    fb_rect(fb, bx + 1, by + 1, 18, 8, C_BLUE);
    fb_rect(fb, bx + 20, by + 3, 2, 4, c);
    int fill = (pct * 16 + 50) / 100;
    if (fill > 0) fb_rect(fb, bx + 2, by + 2, fill, 6, c);
    if (long_life) fb_rect(fb, bx + 2 + 14, by, 1, 10, C_WHITE);   /* where long-life charging stops (~90 %) */
    if (uv > 0) {                                                  /* voltage left of the icon */
        char v[12];
        snprintf(v, sizeof(v), "%d.%02dV", uv / 1000000, (uv / 10000) % 100);
        fb_text(fb, bx - 4 - fb_text_width(v, 1), 1, v, C_WHITE, 1);
    }
}

/* "up 42m", "up 5h12m", "up 3d04h" from /proc/uptime */
static void uptime_text(char *out, size_t n)
{
    double up = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    long m = (long)up / 60, h = m / 60, d = h / 24;
    if (d) snprintf(out, n, "up %ldd%02ldh", d, h % 24);
    else if (h) snprintf(out, n, "up %ldh%02ldm", h, m % 60);
    else snprintf(out, n, "up %ldm", m);
}

static void footer(fb_t *fb, const char *t)
{
    char up[24];
    fb_rect(fb, 0, fb->h - 17, fb->w, 17, C_DGRAY);
    fb_text(fb, 4, fb->h - 16, t, C_GRAY, 1);
    uptime_text(up, sizeof(up));
    fb_text(fb, fb->w - fb_text_width(up, 1) - 3, fb->h - 16, up, C_GRAY, 1);
}

static void draw_status(fb_t *fb, const config_t *cfg, wpactrl_t *w)
{
    char st[2048] = "", state[32], ssid[40], ip[32], line[64], cs[256] = "";
    const char *want = config_get(cfg, "WIFI_SSID", "");
    if (w->fd < 0 || wpactrl_cmd(w, "STATUS", st, sizeof(st), 1000) < 0) st[0] = 0;
    kv(st, "wpa_state", state, sizeof(state));
    kv(st, "ssid", ssid, sizeof(ssid));
    iface_ip("wlan0", ip, sizeof(ip));

    int level = 0;
    char sp[256];
    if (w->fd >= 0 && wpactrl_cmd(w, "SIGNAL_POLL", sp, sizeof(sp), 1000) > 0) {
        char r[16]; kv(sp, "RSSI", r, sizeof(r));
        if (r[0]) level = atoi(r);
    }
    if (!level || level < -110 || level > 0) level = proc_wireless_level();
    int connected = !strcmp(state, "COMPLETED");
    if (!connected) level = 0;

    fb_clear(fb, C_BLACK);
    header(fb, config_get(cfg, "CAMERA_NAME", "LAMAX"), NULL);
    draw_battery(fb, config_int(cfg, "BATTERY_LONG_LIFE", 1));

    int y = 20;
    if (!want[0]) {
        fb_text(fb, 4, y, "No WIFI_SSID set in", C_ORANGE, 1); y += 16;
        fb_text(fb, 4, y, "config.txt!", C_ORANGE, 1); y += 18;
    } else if (connected) {
        snprintf(line, sizeof(line), "WiFi: %s", ssid);
        fb_text(fb, 4, y, line, C_WHITE, 1); y += 18;
    } else {
        snprintf(line, sizeof(line), "%s", want);
        fb_text(fb, 4, y, line, C_WHITE, 1); y += 16;
        snprintf(line, sizeof(line), "Connecting... %s", state[0] ? state : "");
        fb_text(fb, 4, y, line, C_ORANGE, 1); y += 18;
    }

    /* big live signal strength */
    int q = quality(level);
    if (connected) {
        snprintf(line, sizeof(line), "%d dBm", level);
        fb_text(fb, 4, y, line, quality_color(q), 2);
        bars(fb, fb->w - 52, y, q, 2);
        y += 34;
        fb_text(fb, 4, y, quality_text(q), quality_color(q), 1);
        y += 18;
    } else {
        fb_text(fb, 4, y, "-- dBm", C_GRAY, 2);
        y += 52;
    }

    snprintf(line, sizeof(line), "IP %s %s", ip[0] ? ip : "-",
             strcasecmp(config_get(cfg, "IP_MODE", "dhcp"), "static") ? "DHCP" : "static");
    fb_text(fb, 4, y, line, ip[0] ? C_WHITE : C_GRAY, 1); y += 16;

    FILE *f = fopen(CAMD_STAT, "r");
    if (f) { if (!fgets(cs, sizeof(cs), f)) cs[0] = 0; fclose(f); }
    if (cs[0]) {
        char port[8], path[40], clients[8], kbps[16], res[16];
        const char *p;
        #define FIELD(name, dst) do { dst[0] = 0; if ((p = strstr(cs, name "="))) sscanf(p + strlen(name) + 1, "%15s", dst); } while (0)
        FIELD("port", port); FIELD("path", path); FIELD("clients", clients); FIELD("kbps", kbps); FIELD("res", res);
        const char *x = strchr(res, 'x');             /* "1920x1080" -> "1080p" */
        int lines = x ? atoi(x + 1) : 0;
        if (lines == 1088) lines = 1080;
        if (lines > 0) snprintf(line, sizeof(line), "RTSP :%s/%s %dp", port, path, lines);
        else snprintf(line, sizeof(line), "RTSP :%s/%s", port, path);
        fb_text(fb, 4, y, line, C_WHITE, 1); y += 16;
        snprintf(line, sizeof(line), "%s client(s) %.1f Mb/s", clients, atof(kbps) / 1000);
        fb_text(fb, 4, y, line, atoi(clients) ? C_GREEN : C_GRAY, 1); y += 16;
    } else {
        fb_text(fb, 4, y, "Stream: not running", C_RED, 1); y += 16;
    }
    int cpu = cpu_percent(), ram = ram_percent(), t = pmic_temp_dc();
    if (cpu >= 0 && ram >= 0 && y <= fb->h - 17 - 16) {
        int n = snprintf(line, sizeof(line), "CPU %d%% RAM %d%%", cpu, ram);
        if (t > -1000 && n > 0) snprintf(line + n, sizeof(line) - n, " %d°C", (t + 5) / 10);
        uint32_t col = cpu > 90 || ram > 90 ? C_ORANGE : C_GRAY;
        if (t >= 700) col = C_ORANGE;                 /* 70 °C inside the camera: getting hot */
        if (t >= 800) col = C_RED;
        fb_text(fb, 4, y, line, col, 1); y += 16;
    }
    footer(fb, "Button: WiFi scan");
}

static void draw_scan(fb_t *fb, const config_t *cfg, const net_t *nets, int n, int scanning)
{
    const char *want = config_get(cfg, "WIFI_SSID", "");
    char t[48];
    fb_clear(fb, C_BLACK);
    snprintf(t, sizeof(t), "%d", n < 0 ? 0 : n);
    header(fb, scanning ? "Scanning WiFi..." : "WiFi networks", n >= 0 ? t : NULL);
    if (n < 0) {
        fb_text(fb, 4, 40, "WiFi not available", C_RED, 1);
    } else if (n == 0) {
        fb_text(fb, 4, 40, scanning ? "Scanning..." : "No networks found", C_GRAY, 1);
    }
    int rows = (fb->h - 18 - 17) / 16;
    for (int i = 0; i < n && i < rows; i++) {
        int y = 19 + i * 16, q = quality(nets[i].level);
        int mine = want[0] && !strcmp(nets[i].ssid, want);
        if (mine) fb_rect(fb, 0, y, fb->w, 16, RGB(20, 50, 90));
        char line[64];
        snprintf(line, sizeof(line), "%4d", nets[i].level);
        int x = fb_text(fb, 0, y, line, quality_color(q), 1);
        bars(fb, x + 3, y, q, 1);
        snprintf(line, sizeof(line), "%2d %.13s", freq_to_ch(nets[i].freq), nets[i].ssid[0] ? nets[i].ssid : "<hidden>");
        fb_text(fb, x + 30, y, line, mine ? C_WHITE : C_GRAY, 1);
    }
    footer(fb, "Button: back");
}

static void draw_message(fb_t *fb, const char *a, const char *b)
{
    fb_clear(fb, C_BLACK);
    fb_text(fb, (fb->w - fb_text_width(a, 1)) / 2, fb->h / 2 - 18, a, C_WHITE, 1);
    if (b) fb_text(fb, (fb->w - fb_text_width(b, 1)) / 2, fb->h / 2 + 2, b, C_GRAY, 1);
    fb_present(fb);
}

/* ------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--key")) {   /* test helper: inject a key press */
        int fd = open("/dev/input/event0", O_WRONLY);
        struct input_event ev[4]; memset(ev, 0, sizeof(ev));
        ev[0].type = EV_KEY; ev[0].code = atoi(argv[2]); ev[0].value = 1;
        ev[1].type = EV_SYN;
        ev[2] = ev[0]; ev[2].value = 0;
        ev[3].type = EV_SYN;
        return write(fd, ev, sizeof(ev)) == sizeof(ev) ? 0 : 1;
    }
    const char *cfgpath = argc > 1 ? argv[1] : "/mnt/extsd/cam/config.txt";
    config_t *cfg = config_load(cfgpath);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    int timeout = config_int(cfg, "SCREEN_TIMEOUT", 60);
    int long_life = config_int(cfg, "BATTERY_LONG_LIFE", 1);

    fb_t fb;
    if (fb_open(&fb, "/dev/graphics/fb0") < 0) return 1;
    fb_blank(&fb, 0);
    fb_hide_boot_logo();
    fb_clear(&fb, C_BLACK);
    fb_present(&fb);

    int in[MAX_INPUTS], nin = 0;
    for (int i = 0; i < MAX_INPUTS; i++) {
        char p[32]; snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) in[nin++] = fd;
    }

    wpactrl_t w = { .fd = -1 };
    net_t nets[MAX_NETS];
    int nnets = 0, page = PAGE_STATUS, scanning = 0;
    double last_activity = now_s(), last_draw = 0, scan_started = 0, last_scan = 0, power_down = 0;
    double last_bat = 0, low_since = 0;

    while (!g_stop) {
        double t = now_s();
        if (w.fd < 0) wpactrl_open(&w, WPA_CTRL);

        /* ---- input ---- */
        struct pollfd pf[MAX_INPUTS];
        for (int i = 0; i < nin; i++) { pf[i].fd = in[i]; pf[i].events = POLLIN; pf[i].revents = 0; }
        poll(pf, nin, 200);
        for (int i = 0; i < nin; i++) {
            struct input_event ev;
            while (read(in[i], &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type != EV_KEY) continue;
                LOGI("key code=%d value=%d", ev.code, ev.value);
                if (ev.code == KEY_POWER) {
                    if (ev.value == 1) power_down = t;
                    else if (ev.value == 0 && power_down) {
                        double held = t - power_down; power_down = 0;
                        if (held < 3) {
                            if (fb.blanked) { fb_blank(&fb, 0); last_activity = t; last_draw = 0; }
                            else fb_blank(&fb, 1);
                        }
                    }
                    continue;
                }
                if (ev.value != 1) continue;
                int was_blank = fb.blanked;
                last_activity = t; last_draw = 0;
                if (was_blank) {               /* dark screen: OK/any button shows the status page */
                    page = PAGE_STATUS;
                    fb_blank(&fb, 0);
                    continue;
                }
                if (page == PAGE_STATUS) { page = PAGE_SCAN; last_scan = 0; }   /* keep the last list while scanning */
                else { page = PAGE_STATUS; scanning = 0; }
            }
        }
        if (long_life && t - last_bat >= 1) {              /* early shutdown on a low battery */
            last_bat = t;
            if (!battery_low()) low_since = 0;
            else if (!low_since) {
                low_since = t;
                LOGI("battery low (%d%%, %d mV): powering off in %d s unless power returns",
                     read_int_file("/sys/class/power_supply/battery/capacity", -1),
                     read_int_file("/sys/class/power_supply/battery/voltage_now", 0) / 1000, LOW_SECS);
            } else if (t - low_since >= LOW_SECS) {
                fb_blank(&fb, 0);
                draw_message(&fb, "Battery low", "Powering off...");
                LOGI("battery low: power off");
                sleep(5);
                sync();
                system("/system/bin/reboot -p");
                sleep(10);
            }
        }
        if (power_down && t - power_down >= 3) {           /* hold power 3 s: shut down */
            fb_blank(&fb, 0);
            draw_message(&fb, "Powering off...", NULL);
            LOGI("power off requested");
            sync();
            system("/system/bin/reboot -p");
            sleep(10);
        }

        /* ---- WiFi scan page: rescan every 10 s ---- */
        if (page == PAGE_SCAN && w.fd >= 0) {
            char r[64];
            if (!scanning && (last_scan == 0 || t - last_scan > SCAN_EVERY)) {
                if (wpactrl_cmd(&w, "SCAN", r, sizeof(r), 2000) > 0) { scanning = 1; scan_started = t; last_draw = 0; }
                last_scan = t;
            }
            if (scanning && t - scan_started > SCAN_WAIT) {
                static net_t fresh[MAX_NETS];
                int n = scan_results(&w, fresh, MAX_NETS);
                if (n >= 0) {
                    nnets = merge_scan(nets, nnets, fresh, n, t);
                    save_scan(nets, nnets, config_get(cfg, "CAMERA_NAME", ""));
                }
                scanning = 0; last_draw = 0; last_scan = t;
            }
        }

        /* ---- screen ---- */
        if (timeout > 0 && !fb.blanked && t - last_activity > timeout && page == PAGE_STATUS) fb_blank(&fb, 1);
        if (page == PAGE_SCAN && t - last_activity > 300) { page = PAGE_STATUS; scanning = 0; }   /* back to status after 5 min */
        if (!fb.blanked && t - last_draw >= 1) {
            if (page == PAGE_STATUS) draw_status(&fb, cfg, &w);
            else draw_scan(&fb, cfg, nets, w.fd < 0 ? -1 : nnets, scanning);
            fb_present(&fb);
            last_draw = t;
        }
        static double last_ping;
        if (w.fd >= 0 && t - last_ping > 5) {   /* reconnect if wpa_supplicant restarted */
            char r[16];
            last_ping = t;
            if (wpactrl_cmd(&w, "PING", r, sizeof(r), 500) < 0) wpactrl_close(&w);
        }
    }
    wpactrl_close(&w);
    fb_close(&fb);
    config_free(cfg);
    return 0;
}
