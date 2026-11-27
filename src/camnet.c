/* camnet: small helpers for autorun.sh (the camera has no ip/route/grep/sed).
 *   camnet static <if> <ip> <netmask> [gateway]   set address + default route
 *   camnet show <if>                              print IPv4 address
 *   camnet get <config> <KEY> [default]           print a config value
 *   camnet wpaconf <config> <out>                 write wpa_supplicant.conf from config
 *   camnet state <ctrl-socket>                    print wpa_state (e.g. COMPLETED)
 *   camnet ntp <server> [server...]               set the clock (and the RTC) by SNTP */
#include "config.h"
#include "wpactrl.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/wireless.h>

static int set_addr(int s, const char *ifn, unsigned long req, const char *ip)
{
    struct ifreq r; memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", ifn);
    struct sockaddr_in *a = (struct sockaddr_in *)&r.ifr_addr;
    a->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &a->sin_addr) != 1) { fprintf(stderr, "bad address %s\n", ip); return -1; }
    if (ioctl(s, req, &r) < 0) { fprintf(stderr, "ioctl %s: %s\n", ip, strerror(errno)); return -1; }
    return 0;
}

static int show(int s, const char *ifn)
{
    struct ifreq r; memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", ifn);
    if (ioctl(s, SIOCGIFADDR, &r) < 0) { printf("%s: no address\n", ifn); return 1; }
    printf("%s: %s", ifn, inet_ntoa(((struct sockaddr_in *)&r.ifr_addr)->sin_addr));
    if (ioctl(s, SIOCGIFNETMASK, &r) == 0) printf(" netmask %s", inet_ntoa(((struct sockaddr_in *)&r.ifr_addr)->sin_addr));
    printf("\n");
    return 0;
}

static int cmd_static(int argc, char **argv)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    const char *ifn = argv[2];
    if (set_addr(s, ifn, SIOCSIFADDR, argv[3]) < 0) return 1;
    if (set_addr(s, ifn, SIOCSIFNETMASK, argv[4]) < 0) return 1;
    struct ifreq r; memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", ifn);
    ioctl(s, SIOCGIFFLAGS, &r);
    r.ifr_flags |= IFF_UP | IFF_RUNNING;
    ioctl(s, SIOCSIFFLAGS, &r);
    if (argc > 5 && argv[5][0]) {
        struct rtentry rt; memset(&rt, 0, sizeof(rt));
        struct sockaddr_in *gw = (struct sockaddr_in *)&rt.rt_gateway, *dst = (struct sockaddr_in *)&rt.rt_dst,
                           *mask = (struct sockaddr_in *)&rt.rt_genmask;
        gw->sin_family = dst->sin_family = mask->sin_family = AF_INET;
        inet_pton(AF_INET, argv[5], &gw->sin_addr);
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        rt.rt_dev = (char *)ifn;
        ioctl(s, SIOCDELRT, &rt);                      /* replace an old default route */
        if (ioctl(s, SIOCADDRT, &rt) < 0) { fprintf(stderr, "default route via %s: %s\n", argv[5], strerror(errno)); return 1; }
    }
    show(s, ifn);
    close(s);
    return 0;
}

static void hex(FILE *f, const char *s)
{
    for (; *s; s++) fprintf(f, "%02x", (unsigned char)*s);
}

static int cmd_wpaconf(const char *cfgpath, const char *out)
{
    config_t *c = config_load(cfgpath);
    const char *ssid = config_get(c, "WIFI_SSID", "");
    const char *pass = config_get(c, "WIFI_PASSWORD", "");
    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return 1; }
    chmod(out, 0600);
    fprintf(f, "ctrl_interface=/dev/cam/wpa\nap_scan=1\nupdate_config=0\n");
    if (ssid[0]) {
        fprintf(f, "network={\n\tssid=");
        hex(f, ssid);                                   /* hex form: any characters are safe */
        fprintf(f, "\n\tscan_ssid=1\n");
        if (!pass[0]) fprintf(f, "\tkey_mgmt=NONE\n");
        else if (strlen(pass) < 8 || strlen(pass) > 63) {
            fprintf(stderr, "WIFI_PASSWORD must be 8..63 characters\n");
            fprintf(f, "\tkey_mgmt=WPA-PSK\n\tpsk=\"%s\"\n", pass);
        } else fprintf(f, "\tkey_mgmt=WPA-PSK\n\tproto=RSN WPA\n\tpsk=\"%s\"\n", pass);
        fprintf(f, "}\n");
    }
    fclose(f);
    config_free(c);
    return 0;
}

/* ---- SNTP (RFC 4330): the camera has no date/ntpd/busybox ---------------- */
#define NTP_UNIX_OFFSET 2208988800u      /* 1900-01-01 -> 1970-01-01 */

static double now_real(void)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static double ntp_ts(const uint8_t *p)
{
    uint32_t s = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    uint32_t f = (uint32_t)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7];
    return (double)(s - NTP_UNIX_OFFSET) + f / 4294967296.0;
}

/* one query; returns the clock offset (server - local) in *off */
static int sntp_query(const char *server, double *off, char *addr, size_t alen)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    int e = getaddrinfo(server, "123", &hints, &res);
    if (e || !res) { fprintf(stderr, "ntp: %s: cannot resolve (%s)\n", server, gai_strerror(e)); return -1; }
    snprintf(addr, alen, "%s", inet_ntoa(((struct sockaddr_in *)res->ai_addr)->sin_addr));
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int rc = -1;
    for (int attempt = 0; attempt < 3 && rc; attempt++) {
        uint8_t pkt[48]; memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x23;                                      /* LI 0, version 4, mode 3 (client) */
        double t1 = now_real();
        uint32_t sec = (uint32_t)t1 + NTP_UNIX_OFFSET;      /* our transmit time, echoed back */
        pkt[40] = sec >> 24; pkt[41] = sec >> 16; pkt[42] = sec >> 8; pkt[43] = sec;
        if (sendto(s, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen) != sizeof(pkt)) break;
        struct pollfd pf = { .fd = s, .events = POLLIN };
        if (poll(&pf, 1, 2000) <= 0) continue;
        uint8_t r[68];
        ssize_t n = recv(s, r, sizeof(r), 0);
        double t4 = now_real();
        if (n < 48 || (r[0] & 7) != 4 || r[1] == 0 || r[1] > 15) continue;   /* server mode, sane stratum */
        if (memcmp(r + 24, pkt + 40, 4)) continue;                        /* not our request */
        double t2 = ntp_ts(r + 32), t3 = ntp_ts(r + 40);
        *off = ((t2 - t1) + (t3 - t4)) / 2;
        rc = 0;
    }
    if (rc) fprintf(stderr, "ntp: %s (%s): no answer\n", server, addr);
    close(s);
    freeaddrinfo(res);
    return rc;
}

static int cmd_ntp(int argc, char **argv)
{
    /* bionic resolves through netd unless told otherwise; netd is not running */
    setenv("ANDROID_DNS_MODE", "local", 1);
    for (int i = 2; i < argc; i++) {
        if (!argv[i][0]) continue;
        double off; char addr[32] = "";
        if (sntp_query(argv[i], &off, addr, sizeof(addr))) continue;
        double t = now_real() + off;
        struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = (long)((t - (time_t)t) * 1e6) };
        if (settimeofday(&tv, NULL)) { fprintf(stderr, "ntp: settimeofday: %s\n", strerror(errno)); return 1; }
        /* keep the RTC in step so the time is right after a restart even before the network is up */
        int fd = open("/dev/rtc0", O_RDWR);
        if (fd >= 0) {
            struct tm tm; time_t ts = tv.tv_sec; gmtime_r(&ts, &tm);
            struct rtc_time rt = { .tm_sec = tm.tm_sec, .tm_min = tm.tm_min, .tm_hour = tm.tm_hour,
                                   .tm_mday = tm.tm_mday, .tm_mon = tm.tm_mon, .tm_year = tm.tm_year };
            if (ioctl(fd, RTC_SET_TIME, &rt)) fprintf(stderr, "ntp: rtc: %s\n", strerror(errno));
            close(fd);
        }
        struct tm tm; time_t ts = tv.tv_sec; gmtime_r(&ts, &tm);
        char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        printf("time set from %s (%s): %s UTC, offset %+.3f s\n", argv[i], addr, buf, off);
        return 0;
    }
    return 1;
}

/* camnet powersave <iface> on|off: WiFi power management (wireless extensions).
 * The XR819 sleeps between beacons with it on: ~200 ms ping, 30-60 % loss under load,
 * the stream stalls. Off: ~20 ms, no loss (costs some battery). Prints the new state. */
static int cmd_powersave(const char *ifname, const char *mode)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct iwreq w;
    memset(&w, 0, sizeof(w));
    strncpy(w.ifr_name, ifname, IFNAMSIZ - 1);
    w.u.power.disabled = strcmp(mode, "on") != 0;
    if (!w.u.power.disabled) w.u.power.flags = IW_POWER_ON;
    int r = ioctl(fd, SIOCSIWPOWER, &w);
    memset(&w.u, 0, sizeof(w.u));
    if (r == 0) r = ioctl(fd, SIOCGIWPOWER, &w);
    close(fd);
    if (r < 0) { printf("wifi power save: %s\n", strerror(errno)); return 1; }
    printf("wifi power save %s\n", w.u.power.disabled ? "off" : "on");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "powersave")) return cmd_powersave(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "show")) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        return show(s, argv[2]);
    }
    if (argc >= 5 && !strcmp(argv[1], "static")) return cmd_static(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "get")) {
        config_t *c = config_load(argv[2]);
        printf("%s\n", config_get(c, argv[3], argc > 4 ? argv[4] : ""));
        config_free(c);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "ntp")) return cmd_ntp(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "wpaconf")) return cmd_wpaconf(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "state")) {
        wpactrl_t w; char r[2048], st[32] = "";
        if (wpactrl_open(&w, argv[2]) == 0 && wpactrl_cmd(&w, "STATUS", r, sizeof(r), 1000) > 0) {
            char *p = strstr(r, "wpa_state=");
            if (p) sscanf(p + 10, "%31s", st);
        }
        wpactrl_close(&w);
        printf("%s\n", st[0] ? st : "NONE");
        return 0;
    }
    fprintf(stderr, "usage: camnet static|show|get|wpaconf|state|ntp|powersave ...\n");
    return 2;
}
