/* camcfg: wireless configuration for LamaxTool. A small HTTP server on the
 * camera, protected by HTTP Digest authentication (RFC 7616, MD5, qop=auth;
 * user "admin", password ADMIN_PASSWORD from config.txt). Not started when
 * ADMIN_PASSWORD is empty.
 *
 *   GET  /api/status     text: name, IP, WiFi, stream, battery, uptime
 *   GET  /api/config     config.txt as it is on the SD card
 *   PUT  /api/config     replace config.txt (takes effect after a reboot)
 *   POST /api/reboot     restart the camera
 *   GET  /api/wifi       wpa_supplicant scan results (bssid freq dBm flags ssid)
 *   POST /api/wifi/scan  start a new scan (results ~1-3 s later)
 *   GET  /api/talk       intercom: a one-time talk token (sealed), valid 30 s
 *   TALK /api/talk       (header X-Talk-Token, no Digest) then a stream of 16 kHz mono
 *                        s16le sound, ChaCha20-encrypted with HMAC(master, "talk"+token),
 *                        played on the speaker until the connection closes
 *
 * usage: camcfg <config.txt>
 * Every nonce is used once per request counter (nc), lives 5 minutes, and a
 * new one is issued with each 401, so a recorded request cannot be replayed.
 * A failed login costs 1 s. The config.txt contents (with the WiFi, RTSP and admin
 * passwords) travel encrypted in both directions (seal.h: ChaCha20 + HMAC-SHA256,
 * key from the admin password); status and WiFi scan results are plain. */
#include "alsa.h"
#include "config.h"
#include "log.h"
#include "md5.h"
#include "seal.h"
#include "wpactrl.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define REALM     "LAMAX camera"
#define USER      "admin"
#define MAX_HDR   8192
#define MAX_BODY  65536
#define NONCES    16
#define NONCE_TTL 300
#define WPA_CTRL  "/dev/cam/wpa/wlan0"
#define CAMD_STAT "/dev/cam/camd.status"

static const char *g_cfg_path;
static char g_pass[128];
static uint8_t g_master[32];            /* envelope key, derived from the admin password */

static struct { char v[33]; time_t born; unsigned nc; } nonces[NONCES];

/* intercom */
#define TALK_RATE   16000
#define TALK_PERIOD 320                 /* frames = 20 ms */
#define TALK_CHUNK  (TALK_PERIOD * 2)   /* bytes per chunk = 10 ChaCha20 blocks */
#define TALK_TTL    30
static uint8_t g_talk_token[16];
static time_t g_talk_born;              /* 0 = no token outstanding */
static pid_t g_talk_pid;                /* child playing, 0 = none */
static int g_listen = -1;               /* closed in the talk child */

static uint32_t rnd32(void)
{
    uint32_t r;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0 && read(fd, &r, sizeof(r)) == sizeof(r)) { close(fd); return r; }
    if (fd >= 0) close(fd);
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint32_t)lrand48() ^ (uint32_t)tv.tv_usec;
}

static time_t mono(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

static const char *new_nonce(void)
{
    int oldest = 0;
    for (int i = 1; i < NONCES; i++) if (nonces[i].born < nonces[oldest].born) oldest = i;
    snprintf(nonces[oldest].v, sizeof(nonces[oldest].v), "%08x%08x%08x%08x", rnd32(), rnd32(), rnd32(), rnd32());
    nonces[oldest].born = mono();
    nonces[oldest].nc = 0;
    return nonces[oldest].v;
}

/* ------------------------------------------------------------------ HTTP helpers */

static int send_all(int fd, const void *d, size_t n)
{
    const char *p = d;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static void reply(int fd, int code, const char *status, const char *extra, const char *body, size_t blen)
{
    char h[512];
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                     code, status, (unsigned)blen, extra ? extra : "");
    send_all(fd, h, (size_t)n);
    if (blen) send_all(fd, body, blen);
}

static void reply_text(int fd, int code, const char *status, const char *text)
{
    reply(fd, code, status, NULL, text, strlen(text));
}

static const char *hdr(const char *req, const char *name, char *out, size_t outsz)
{
    size_t nl = strlen(name);
    for (const char *p = req; (p = strchr(p, '\n')); ) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t l = strcspn(v, "\r\n");
            if (l >= outsz) l = outsz - 1;
            memcpy(out, v, l); out[l] = 0;
            return out;
        }
    }
    return NULL;
}

static const char *param(const char *s, const char *key, char *out, size_t outsz)
{
    size_t kl = strlen(key);
    for (const char *p = s; *p; p++) {
        if ((p == s || p[-1] == ' ' || p[-1] == ',') && !strncasecmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t l;
            if (*v == '"') { v++; l = strcspn(v, "\""); } else l = strcspn(v, ", ");
            if (l >= outsz) l = outsz - 1;
            memcpy(out, v, l); out[l] = 0;
            return out;
        }
    }
    return NULL;
}

/* 1 = authorised; 0 = no / bad credentials; -1 = stale nonce (ask again, no penalty) */
static int check_auth(const char *req, const char *method, const char *target)
{
    char a[1024];
    if (!hdr(req, "Authorization", a, sizeof(a)) || strncasecmp(a, "Digest ", 7)) return 0;
    char user[64], realm[64], nonce[64], uri[512], resp[40], qop[16], nc[16], cnonce[64];
    const char *d = a + 7;
    if (!param(d, "username", user, sizeof(user)) || !param(d, "realm", realm, sizeof(realm)) ||
        !param(d, "nonce", nonce, sizeof(nonce)) || !param(d, "uri", uri, sizeof(uri)) ||
        !param(d, "response", resp, sizeof(resp)) || !param(d, "qop", qop, sizeof(qop)) ||
        !param(d, "nc", nc, sizeof(nc)) || !param(d, "cnonce", cnonce, sizeof(cnonce)))
        return 0;
    if (strcmp(user, USER) || strcmp(realm, REALM) || strcmp(qop, "auth") || strcmp(uri, target)) return 0;
    int k = -1;
    for (int i = 0; i < NONCES; i++) if (nonces[i].v[0] && !strcmp(nonces[i].v, nonce)) k = i;
    if (k < 0 || mono() - nonces[k].born > NONCE_TTL) return -1;
    char tmp[1024], ha1[33], ha2[33], exp[33];
    snprintf(tmp, sizeof(tmp), "%s:%s:%s", USER, REALM, g_pass); md5_hex(tmp, ha1);
    snprintf(tmp, sizeof(tmp), "%s:%s", method, uri); md5_hex(tmp, ha2);
    snprintf(tmp, sizeof(tmp), "%s:%s:%s:%s:%s:%s", ha1, nonce, nc, cnonce, qop, ha2); md5_hex(tmp, exp);
    if (strcasecmp(exp, resp)) return 0;
    unsigned n = (unsigned)strtoul(nc, NULL, 16);
    if (n <= nonces[k].nc) return -1;            /* replayed request counter */
    nonces[k].nc = n;
    return 1;
}

/* ------------------------------------------------------------------ data */

static size_t append_file(char *out, size_t pos, size_t cap, const char *label, const char *path)
{
    char buf[256] = "";
    FILE *f = fopen(path, "r");
    if (f) { if (!fgets(buf, sizeof(buf), f)) buf[0] = 0; fclose(f); }
    buf[strcspn(buf, "\r\n")] = 0;
    if (pos < cap) pos += (size_t)snprintf(out + pos, cap - pos, "%s=%s\n", label, buf);
    return pos < cap ? pos : cap;
}

static void wlan_ip(char *out, size_t n)
{
    out[0] = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0)
        snprintf(out, n, "%s", inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
    close(fd);
}

static int wpa(const char *cmd, char *reply_buf, size_t len)
{
    wpactrl_t w;
    if (wpactrl_open(&w, WPA_CTRL) < 0) return -1;
    int r = wpactrl_cmd(&w, cmd, reply_buf, len, 3000);
    wpactrl_close(&w);
    return r;
}

static void get_status(int fd)
{
    static char out[2048];
    size_t pos = 0, cap = sizeof(out);
    config_t *cfg = config_load(g_cfg_path);
    char ip[32]; wlan_ip(ip, sizeof(ip));
    pos += (size_t)snprintf(out + pos, cap - pos, "name=%s\nip=%s\n",
                            cfg ? config_get(cfg, "CAMERA_NAME", "") : "", ip);
    config_free(cfg);
    char st[1024];
    if (wpa("STATUS", st, sizeof(st)) > 0) {
        for (char *l = strtok(st, "\n"); l; l = strtok(NULL, "\n"))
            if (!strncmp(l, "ssid=", 5) || !strncmp(l, "wpa_state=", 10))
                pos += (size_t)snprintf(out + pos, cap - pos, "wifi_%s\n", l);
    }
    pos = append_file(out, pos, cap, "stream", CAMD_STAT);
#define PS "/sys/class/power_supply/"
    pos = append_file(out, pos, cap, "battery_pct", PS "battery/capacity");
    pos = append_file(out, pos, cap, "battery_status", PS "battery/status");
    pos = append_file(out, pos, cap, "battery_uv", PS "battery/voltage_now");
    pos = append_file(out, pos, cap, "usb_power", PS "usb/online");
    pos = append_file(out, pos, cap, "ac_power", PS "ac/online");
#undef PS
    pos = append_file(out, pos, cap, "uptime", "/proc/uptime");
    time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm);
    if (pos < cap) pos += strftime(out + pos, cap - pos, "time_utc=%Y-%m-%d %H:%M:%S\n", &tm);
    reply(fd, 200, "OK", NULL, out, pos < cap ? pos : cap);
}

static void get_config(int fd)
{
    static uint8_t plain[MAX_BODY - SEAL_OVERHEAD], out[MAX_BODY];
    int f = open(g_cfg_path, O_RDONLY);
    if (f < 0) { reply_text(fd, 404, "Not Found", "config.txt not found\n"); return; }
    ssize_t n = read(f, plain, sizeof(plain));
    close(f);
    if (n < 0) { reply_text(fd, 500, "Error", "read failed\n"); return; }
    size_t len = seal(g_master, plain, (size_t)n, out, time(NULL));
    memset(plain, 0, sizeof(plain));
    reply(fd, 200, "OK", "X-Lamax-Sealed: 1\r\n", (const char *)out, len);
}

static void put_config(int fd, char *sealed, size_t sn)
{
    long r = seal_open(g_master, (uint8_t *)sealed, sn, time(NULL));
    if (r == -2) { reply_text(fd, 400, "Bad Request", "message too old or camera clock off\n"); return; }
    if (r < 0) {
        LOGW("config over WiFi: cannot decrypt (wrong key or altered)");
        reply_text(fd, 400, "Bad Request", "cannot decrypt the settings\n");
        return;
    }
    const char *body = sealed + 28;
    size_t n = (size_t)r;
    if (n == 0 || memchr(body, 0, n)) { reply_text(fd, 400, "Bad Request", "empty or binary config\n"); return; }
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.new", g_cfg_path);
    int f = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) { reply_text(fd, 500, "Error", "cannot write the SD card\n"); return; }
    int ok = write(f, body, n) == (ssize_t)n && fsync(f) == 0;
    close(f);
    if (!ok || rename(tmp, g_cfg_path) != 0) {
        unlink(tmp);
        reply_text(fd, 500, "Error", "writing config.txt failed\n");
        return;
    }
    sync();
    LOGI("config.txt replaced over WiFi (%u bytes)", (unsigned)n);
    reply_text(fd, 200, "OK", "saved; restart the camera to apply\n");
}

static void do_reboot(int fd)
{
    reply_text(fd, 200, "OK", "rebooting\n");
    shutdown(fd, SHUT_WR);
    LOGI("reboot requested over WiFi");
    sync();
    if (fork() == 0) {
        sleep(1);
        execl("/system/bin/reboot", "reboot", (char *)NULL);
        _exit(1);
    }
}

static void get_wifi(int fd)
{
    static char buf[16384];
    int n = wpa("SCAN_RESULTS", buf, sizeof(buf));
    if (n < 0) { reply_text(fd, 503, "Unavailable", "wpa_supplicant is not running\n"); return; }
    reply(fd, 200, "OK", NULL, buf, (size_t)n);
}

static void scan_wifi(int fd)
{
    char r[64] = "";
    if (wpa("SCAN", r, sizeof(r)) < 0) { reply_text(fd, 503, "Unavailable", "wpa_supplicant is not running\n"); return; }
    reply_text(fd, 200, "OK", r[0] ? r : "OK\n");
}

/* ------------------------------------------------------------------ intercom */

static int talking(void)
{
    return g_talk_pid > 0 && kill(g_talk_pid, 0) == 0;   /* SIGCHLD ignored: no zombies */
}

static void get_talk(int fd)
{
    if (talking()) { reply_text(fd, 409, "Conflict", "someone is already talking\n"); return; }
    int u = open("/dev/urandom", O_RDONLY);
    if (u < 0 || read(u, g_talk_token, sizeof(g_talk_token)) != (ssize_t)sizeof(g_talk_token)) {
        if (u >= 0) close(u);
        reply_text(fd, 500, "Error", "no random numbers\n");
        return;
    }
    close(u);
    g_talk_born = mono();
    uint8_t out[sizeof(g_talk_token) + SEAL_OVERHEAD];
    size_t n = seal(g_master, g_talk_token, sizeof(g_talk_token), out, time(NULL));
    reply(fd, 200, "OK", "X-Lamax-Sealed: 1\r\n", (const char *)out, n);
}

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* child process: decrypt the stream and play it until the connection ends */
static void talk_play(int fd, const uint8_t key[32], const uint8_t *pre, size_t prelen)
{
    static const uint8_t zero_nonce[12];
    struct timeval tv = { 3, 0 };                         /* 3 s without sound: end */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    alsa_mixer_set(0, "Speaker Function", 1);             /* must be written before every open */
    alsa_mixer_set(0, "Audio speaker out", 1);
    alsa_mixer_set(0, "Line Volume", 31);
    alsa_t a;
    /* 40 ms x 4 periods = 160 ms of buffer against WiFi jitter (20 ms x 8 is rejected) */
    if (alsa_open(&a, 0, 0, 0, TALK_RATE, 1, 2 * TALK_PERIOD, 4) < 0) {
        LOGE("talk: speaker: %s", a.err);
        return;
    }
    LOGI("talk: playing");
    uint8_t buf[TALK_CHUNK];
    size_t have = prelen < sizeof(buf) ? prelen : sizeof(buf);
    memcpy(buf, pre, have);
    uint32_t counter = 1;
    size_t total = 0;
    for (;;) {
        while (have < sizeof(buf)) {
            ssize_t r = recv(fd, buf + have, sizeof(buf) - have, 0);
            if (r <= 0) goto done;
            have += (size_t)r;
        }
        chacha20_xor(key, zero_nonce, counter, buf, sizeof(buf));
        counter += TALK_CHUNK / 64;
        if (alsa_write(&a, (int16_t *)buf, TALK_PERIOD) < 0) { LOGW("talk: %s", a.err); break; }
        total += TALK_PERIOD;
        have = 0;
    }
done:
    alsa_close(&a);                                       /* plays out what is buffered */
    alsa_mixer_set(0, "Audio speaker out", 0);
    LOGI("talk: ended after %.1f s", total / (double)TALK_RATE);
}

static void talk_session(int fd, const char *req, const uint8_t *pre, size_t prelen)
{
    char tok[64];
    uint8_t t[16];
    int ok = hdr(req, "X-Talk-Token", tok, sizeof(tok)) && strlen(tok) == 32 && g_talk_born &&
             mono() - g_talk_born <= TALK_TTL;
    for (int i = 0; ok && i < 16; i++) {
        int h = hexval(tok[2 * i]), l = hexval(tok[2 * i + 1]);
        if (h < 0 || l < 0) ok = 0; else t[i] = (uint8_t)(h << 4 | l);
    }
    uint8_t diff = 0;
    for (int i = 0; ok && i < 16; i++) diff |= t[i] ^ g_talk_token[i];
    if (!ok || diff || talking()) {
        LOGW("talk: refused (bad or expired token, or busy)");
        sleep(1);
        reply_text(fd, 403, "Forbidden", "talk token invalid or expired\n");
        return;
    }
    g_talk_born = 0;                                      /* single use */
    uint8_t key[32], m[4 + 16];
    memcpy(m, "talk", 4); memcpy(m + 4, t, 16);
    hmac_sha256(g_master, 32, m, sizeof(m), key);
    const char *ok_resp = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n";
    send_all(fd, ok_resp, strlen(ok_resp));
    pid_t p = fork();
    if (p == 0) { close(g_listen); talk_play(fd, key, pre, prelen); _exit(0); }
    if (p > 0) g_talk_pid = p;
    memset(key, 0, sizeof(key));
}

/* ------------------------------------------------------------------ server */

static void handle(int fd)
{
    static char req[MAX_HDR + 1];
    static char body[MAX_BODY];
    size_t got = 0;
    char *end = NULL;
    while (got < MAX_HDR) {
        ssize_t r = recv(fd, req + got, MAX_HDR - got, 0);
        if (r <= 0) return;
        got += (size_t)r; req[got] = 0;
        if ((end = strstr(req, "\r\n\r\n"))) break;
    }
    if (!end) { reply_text(fd, 431, "Request Header Fields Too Large", "headers too large\n"); return; }
    *end = 0;
    char *extra = end + 4;
    size_t have = got - (size_t)(extra - req);

    char method[8], target[256];
    if (sscanf(req, "%7s %255s", method, target) != 2) { reply_text(fd, 400, "Bad Request", "bad request\n"); return; }

    /* intercom stream: the one-time token is the authentication; sound bytes that
     * arrived together with the header are handed over */
    if (!strcmp(method, "TALK") && !strcmp(target, "/api/talk")) {
        talk_session(fd, req, (const uint8_t *)extra, have);
        return;
    }

    char cl[24];
    size_t blen = hdr(req, "Content-Length", cl, sizeof(cl)) ? (size_t)strtoul(cl, NULL, 10) : 0;
    if (blen > MAX_BODY) { reply_text(fd, 413, "Payload Too Large", "too large\n"); return; }

    /* read the body first (only PUT /api/config has one): closing a socket with
     * unread data resets the connection and the client would miss the reply */
    if (blen) {
        if (have > blen) have = blen;
        memcpy(body, extra, have);
        while (have < blen) {
            ssize_t r = recv(fd, body + have, blen - have, 0);
            if (r <= 0) { reply_text(fd, 400, "Bad Request", "body incomplete\n"); return; }
            have += (size_t)r;
        }
    }

    int a = check_auth(req, method, target);
    if (a != 1) {
        char h[256];
        snprintf(h, sizeof(h), "WWW-Authenticate: Digest realm=\"" REALM "\", qop=\"auth\", algorithm=MD5, "
                 "nonce=\"%s\"%s\r\n", new_nonce(), a < 0 ? ", stale=true" : "");
        if (a == 0 && strstr(req, "\nAuthorization:")) {
            LOGW("wrong login from a client");
            sleep(1);                                   /* slows down password guessing */
        }
        reply(fd, 401, "Unauthorized", h, "login required\n", 15);
        return;
    }

    if (!strcmp(method, "GET") && !strcmp(target, "/api/status")) get_status(fd);
    else if (!strcmp(method, "GET") && !strcmp(target, "/api/config")) get_config(fd);
    else if (!strcmp(method, "PUT") && !strcmp(target, "/api/config")) put_config(fd, body, blen);
    else if (!strcmp(method, "POST") && !strcmp(target, "/api/reboot")) do_reboot(fd);
    else if (!strcmp(method, "GET") && !strcmp(target, "/api/wifi")) get_wifi(fd);
    else if (!strcmp(method, "POST") && !strcmp(target, "/api/wifi/scan")) scan_wifi(fd);
    else if (!strcmp(method, "GET") && !strcmp(target, "/api/talk")) get_talk(fd);
    else reply_text(fd, 404, "Not Found", "unknown request\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: camcfg <config.txt>\n"); return 2; }
    g_cfg_path = argv[1];
    config_t *cfg = config_load(g_cfg_path);
    if (!cfg) { LOGE("cannot read %s", g_cfg_path); return 1; }
    snprintf(g_pass, sizeof(g_pass), "%s", config_get(cfg, "ADMIN_PASSWORD", ""));
    int port = config_int(cfg, "ADMIN_PORT", 8080);
    config_free(cfg);
    if (!g_pass[0]) { LOGI("camcfg: ADMIN_PASSWORD is empty, wireless configuration off"); return 0; }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    seal_master(g_pass, g_master);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    LOGI("camcfg: key ready in %ld ms", (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    int ls = g_listen = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(ls, 4) < 0) {
        LOGE("camcfg: port %d: %s", port, strerror(errno));
        return 1;
    }
    LOGI("camcfg: wireless configuration on port %d", port);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; sleep(1); continue; }
        struct timeval tv = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle(fd);
        close(fd);
    }
}
