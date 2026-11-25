/* camled: the camera's LEDs.
 *   0..4  /sys/class/leds/led_0..led_4 (on this camera only led_0 = blue is fitted)
 *   5     the top yellow LED on AXP209 GPIO1 (/sys/devices/platform/axp20-gpio,
 *         the way the stock app drives it when WiFi is switched on)
 *   The red LED is the PMIC's hardware charge indicator and cannot be controlled.
 *
 *   LED_MODE=off     all LEDs off
 *   LED_MODE=stock   leave the LEDs alone
 *   LED_MODE=status  two roles:
 *     LED_STREAM (e.g. "0,2" = blue + yellow) shows the camera state:
 *        streaming (a client is watching)    short soft pulse every 2 s (yellow: short flash)
 *        ready (WiFi + IP, nobody watching)   tiny flash every 4 s
 *        no WiFi / no IP yet                  fast blink, 4 Hz
 *        error (camd not running)             double flash every second
 *     LED_POWER (e.g. "1" = red) shows the power supply:
 *        external power, battery charging     slow blink (1 s on / 1 s off)
 *        external power, battery full         steady on
 *        on battery                           off
 *        on battery, below 15 %               short flash every 2 s
 *     all other LEDs are switched off.
 *
 * Breathing uses software PWM (100 Hz), so it also works on plain on/off GPIO LEDs.
 * usage: camled [config.txt] | camled --test <n>   (light only led_<n> for 5 s) */
#include "config.h"
#include "log.h"
#include "wpactrl.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NLED 6
#define AXP_GPIO "/sys/devices/platform/axp20-gpio"
#define CAMD_STAT "/dev/cam/camd.status"
#define WPA_CTRL  "/dev/cam/wpa/wlan0"

enum { ST_ERROR, ST_CONNECTING, ST_READY, ST_STREAMING };
static const char *st_name[] = { "error", "connecting", "ready", "streaming" };

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { g_stop = 1; }

static int led_fd[NLED], led_max[NLED], led_cur[NLED];

static void sysfs_write(const char *path, const char *v)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, v, strlen(v)); close(fd); }
}

static void led_open(void)
{
    /* LED 5: AXP GPIO1 as output */
    sysfs_write(AXP_GPIO "/gpio", "1");
    sysfs_write(AXP_GPIO "/io", "1");
    led_fd[5] = open(AXP_GPIO "/value", O_WRONLY);
    led_max[5] = 1;
    led_cur[5] = -1;
    for (int i = 0; i < 5; i++) {
        char p[64], b[16] = "";
        snprintf(p, sizeof(p), "/sys/class/leds/led_%d/trigger", i);
        int t = open(p, O_WRONLY);
        if (t >= 0) { write(t, "none", 4); close(t); }           /* no kernel trigger */
        snprintf(p, sizeof(p), "/sys/class/leds/led_%d/max_brightness", i);
        int m = open(p, O_RDONLY);
        led_max[i] = 255;
        if (m >= 0) { if (read(m, b, sizeof(b) - 1) > 0) led_max[i] = atoi(b); close(m); }
        if (led_max[i] <= 0) led_max[i] = 1;
        snprintf(p, sizeof(p), "/sys/class/leds/led_%d/brightness", i);
        led_fd[i] = open(p, O_WRONLY);
        led_cur[i] = -1;
    }
}

/* On this camera both fitted LEDs light up when their control value is 0:
 * led_0 (blue) is wired opposite to its active_low setting in script.bin,
 * and the yellow LED on AXP GPIO1 is active-low. LED_INVERT can override. */
static int led_inv[NLED] = { 1, 0, 0, 0, 0, 1 };

static void led_set(int i, int on)
{
    if (led_fd[i] < 0 || led_cur[i] == on) return;
    char b[8];
    int lvl = led_inv[i] ? !on : on;
    int n = snprintf(b, sizeof(b), "%d", lvl ? led_max[i] : 0);
    lseek(led_fd[i], 0, SEEK_SET);
    write(led_fd[i], b, n);
    led_cur[i] = on;
}

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int have_ip(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq r; memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, sizeof(r.ifr_name), "wlan0");
    int ok = ioctl(s, SIOCGIFADDR, &r) == 0 &&
             ((struct sockaddr_in *)&r.ifr_addr)->sin_addr.s_addr != 0;
    close(s);
    return ok;
}

static int read_state(wpactrl_t *w)
{
    struct stat st;
    /* camd rewrites its status file every 2 s */
    if (stat(CAMD_STAT, &st) != 0 || time(NULL) - st.st_mtime > 10) return ST_ERROR;
    if (w->fd < 0) wpactrl_open(w, WPA_CTRL);
    char r[2048] = "";
    if (w->fd < 0 || wpactrl_cmd(w, "STATUS", r, sizeof(r), 1000) < 0) { wpactrl_close(w); return ST_CONNECTING; }
    if (!strstr(r, "wpa_state=COMPLETED") || !have_ip()) return ST_CONNECTING;
    FILE *f = fopen(CAMD_STAT, "r");
    int clients = 0;
    if (f) {
        char line[256] = "";
        if (fgets(line, sizeof(line), f)) { char *p = strstr(line, "clients="); if (p) clients = atoi(p + 8); }
        fclose(f);
    }
    return clients > 0 ? ST_STREAMING : ST_READY;
}

enum { PW_BATTERY, PW_BATTERY_LOW, PW_CHARGING, PW_FULL };

static int read_int(const char *p, int def)
{
    FILE *f = fopen(p, "r");
    int v = def;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = def; fclose(f); }
    return v;
}

static int read_power(void)
{
    int ext = read_int("/sys/class/power_supply/usb/online", 0) || read_int("/sys/class/power_supply/ac/online", 0);
    char st[24] = "";
    FILE *f = fopen("/sys/class/power_supply/battery/status", "r");
    if (f) { if (!fgets(st, sizeof(st), f)) st[0] = 0; fclose(f); }
    if (ext) return strncmp(st, "Charging", 8) ? PW_FULL : PW_CHARGING;
    return read_int("/sys/class/power_supply/battery/capacity", 100) < 15 ? PW_BATTERY_LOW : PW_BATTERY;
}

static int power_pattern(int pw, double t)
{
    switch (pw) {
    case PW_CHARGING:    return fmod(t, 2.0) < 1.0;
    case PW_FULL:        return 1;
    case PW_BATTERY_LOW: return fmod(t, 2.0) < 0.08;
    default:             return 0;
    }
}

/* brightness 0..1 of the pattern at time t */
static double pattern(int st, double t)
{
    switch (st) {
    case ST_STREAMING: {                      /* mostly dark: a soft 0.9 s pulse every 2 s */
        double p = fmod(t, 2.0);
        if (p >= 0.9) return 0;
        double x = sin(M_PI * p / 0.9);
        return x * x;
    }
    case ST_READY:      return fmod(t, 4.0) < 0.15 ? 1 : 0;             /* short flash every 4 s */
    case ST_CONNECTING: return fmod(t, 0.25) < 0.125 ? 1 : 0;           /* 4 Hz blink */
    default: {                                                          /* double flash */
        double p = fmod(t, 1.0);
        return (p < 0.12 || (p > 0.25 && p < 0.37)) ? 1 : 0;
    }
    }
}

int main(int argc, char **argv)
{
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    led_open();

    if (argc > 2 && !strcmp(argv[1], "--test")) {   /* identify LEDs: only led_<n> on for 5 s */
        int n = atoi(argv[2]);
        for (int i = 0; i < NLED; i++) led_set(i, i == n);
        sleep(5);
        led_set(n, 0);
        return 0;
    }

    config_t *cfg = config_load(argc > 1 ? argv[1] : "/mnt/extsd/cam/config.txt");
    const char *inv = config_get(cfg, "LED_INVERT", NULL);   /* e.g. "0,5"; empty = none */
    if (inv) {
        for (int i = 0; i < NLED; i++) led_inv[i] = 0;
        for (const char *p = inv; *p; p++) if (*p >= '0' && *p < '0' + NLED) led_inv[*p - '0'] = 1;
    }
    /* LED=on|off (LED_MODE=status|off|stock is the older, still accepted form) */
    const char *mode = config_get(cfg, "LED_MODE", "status");
    const char *simple = config_get(cfg, "LED", NULL);
    if (simple) mode = (!strcasecmp(simple, "off") || !strcmp(simple, "0")) ? "off" : "status";
    if (!strcasecmp(mode, "stock")) { LOGI("led: mode stock, not touching LEDs"); return 0; }

    int use[NLED] = { 0 }, pwr[NLED] = { 0 };
    /* LED_STATUS is the old name of LED_STREAM */
    const char *list = config_get(cfg, "LED_STREAM", config_get(cfg, "LED_STATUS", "0,5"));
    const char *plist = config_get(cfg, "LED_POWER", "");
    for (const char *p = list; *p; p++) if (*p >= '0' && *p < '0' + NLED) use[*p - '0'] = 1;
    for (const char *p = plist; *p; p++) if (*p >= '0' && *p < '0' + NLED) pwr[*p - '0'] = 1;
    int status = !strcasecmp(mode, "status");
    for (int i = 0; i < NLED; i++) led_set(i, 0);
    if (!status) { LOGI("led: all LEDs off"); return 0; }
    LOGI("led: status on LED(s) %s, power on LED(s) %s", list, plist[0] ? plist : "-");

    wpactrl_t w = { .fd = -1 };
    int st = ST_CONNECTING, last_st = -1, pw = PW_FULL;
    double t0 = now_s(), last_check = 0;
    const double frame = 0.010;                  /* 100 Hz software PWM */
    while (!g_stop) {
        double t = now_s() - t0;
        if (t - last_check >= 1) {
            st = read_state(&w);
            pw = read_power();
            last_check = t;
            if (st != last_st) { LOGI("led: state %s", st_name[st]); last_st = st; }
        }
        double level = pattern(st, t);
        double phase = fmod(t, frame) / frame;   /* position inside the PWM frame */
        int on = level >= 0.999 ? 1 : level <= 0.001 ? 0 : phase < level;
        int pon = power_pattern(pw, t);
        /* the yellow LED (5) sits behind the PMIC's I2C GPIO, which is too slow
         * for clean PWM: it just switches on for the bright part of a pulse */
        int on_nopwm = level >= 0.5;
        for (int i = 0; i < NLED; i++) {
            if (use[i]) led_set(i, i == 5 ? on_nopwm : on);
            else if (pwr[i]) led_set(i, pon);
        }
        /* sleep to the next interesting edge: frame boundary or duty edge */
        double next = level > 0.001 && level < 0.999 ? (phase < level ? level - phase : 1 - phase) * frame : frame;
        struct timespec ts = { 0, (long)((next < 0.001 ? 0.001 : next) * 1e9) };
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < NLED; i++) led_set(i, 0);
    wpactrl_close(&w);
    config_free(cfg);
    return 0;
}
