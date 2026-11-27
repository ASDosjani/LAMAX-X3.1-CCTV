/* Minimal RTSP server (RFC 2326) + RTP H.264 packetizer (RFC 6184)
 * + optional G.711 mu-law audio track (RFC 3551, payload type 0). */
#include "rtsp.h"
#include "log.h"
#include "md5.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS   8
#define RXBUF         4096
#define RTP_MTU       1400
#define TCP_QUEUE_MAX (1536 * 1024) /* drop frames for a TCP client if it lags this much */
#define SESSION_TIMEOUT 65          /* seconds without RTSP/RTCP activity */
#define RTP_PORT_BASE 6970          /* server UDP ports: video 6970/6971, audio 6972/6973 */
#define NONCES        16            /* outstanding login challenges */
#define NONCE_TTL     60            /* seconds a challenge may be answered */
#define REALM         "LAMAX camera"

enum { ST_INIT, ST_READY, ST_PLAYING };
enum { TR_NONE, TR_UDP, TR_TCP };
enum { T_VIDEO, T_AUDIO, NTRACKS };

typedef struct {                       /* one RTP stream of a client */
    int setup;
    int ch_rtp, ch_rtcp;               /* TCP interleaved channels */
    struct sockaddr_in udp_rtp, udp_rtcp;
    uint32_t ssrc;
    uint16_t seq;
    uint32_t pkts, octets;
    time_t last_sr;
} track_t;

typedef struct {
    int fd;
    struct sockaddr_in peer;
    char rx[RXBUF];
    int rxlen;
    int state;
    int transport;
    track_t tr[NTRACKS];
    uint32_t session;
    int need_key;
    time_t last_seen;
    uint8_t *txq; size_t txlen, txcap; /* pending TCP output */
    int authed;                        /* this connection has logged in */
} client_t;

struct rtsp_server {
    rtsp_cfg_t cfg;
    char path[64], name[64], user[64], pass[64];
    struct { char v[33]; time_t born; } nonce[NONCES];
    int lfd, rtp_fd[NTRACKS], rtcp_fd[NTRACKS];
    client_t cl[MAX_CLIENTS];
    int ncl;
    uint8_t sps[128], pps[64];
    size_t sps_len, pps_len;
    int want_key;
    uint64_t bytes;
};

/* ------------------------------------------------------------------ utils */

static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static uint32_t rnd32(void)
{
    static int seeded;
    if (!seeded) {
        struct timeval tv; gettimeofday(&tv, NULL);
        srand48(tv.tv_sec ^ tv.tv_usec ^ getpid());
        seeded = 1;
    }
    return (uint32_t)lrand48() ^ ((uint32_t)lrand48() << 16);
}

static void b64(const uint8_t *in, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
        out[o++] = t[v >> 18]; out[o++] = t[(v >> 12) & 63]; out[o++] = t[(v >> 6) & 63]; out[o++] = t[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = in[i] << 16;
        out[o++] = t[v >> 18]; out[o++] = t[(v >> 12) & 63]; out[o++] = '='; out[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = in[i] << 16 | in[i + 1] << 8;
        out[o++] = t[v >> 18]; out[o++] = t[(v >> 12) & 63]; out[o++] = t[(v >> 6) & 63]; out[o++] = '=';
    }
    out[o] = 0;
}

/* iterate NAL units in an Annex-B buffer */
static const uint8_t *next_nal(const uint8_t *p, const uint8_t *end, const uint8_t **nal, size_t *nlen)
{
    while (p + 3 <= end && !(p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p + 3 < end && p[3] == 1)))) p++;
    if (p + 3 > end) return NULL;
    p += (p[2] == 1) ? 3 : 4;
    const uint8_t *s = p;
    while (p + 3 <= end && !(p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p + 3 < end && p[3] == 1)))) p++;
    if (p + 3 > end) p = end;
    *nal = s; *nlen = (size_t)(p - s);
    while (*nlen && s[*nlen - 1] == 0 && p != end) (*nlen)--; /* trailing zero of a 4-byte start code */
    return p;
}

/* ---------------------------------------------------------------- server */

static int udp_bind(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { LOGE("rtsp: bind udp %d: %s", port, strerror(errno)); close(fd); return -1; }
    int sz = 256 * 1024; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
    set_nonblock(fd);
    return fd;
}

rtsp_server_t *rtsp_create(const rtsp_cfg_t *cfg)
{
    rtsp_server_t *s = calloc(1, sizeof(*s));
    s->cfg = *cfg;
    snprintf(s->path, sizeof(s->path), "%s", cfg->path && *cfg->path ? cfg->path : "stream");
    snprintf(s->name, sizeof(s->name), "%s", cfg->name && *cfg->name ? cfg->name : "LAMAX camera");
    snprintf(s->user, sizeof(s->user), "%s", cfg->user ? cfg->user : "");
    snprintf(s->pass, sizeof(s->pass), "%s", cfg->password ? cfg->password : "");
    if (s->cfg.max_clients <= 0 || s->cfg.max_clients > MAX_CLIENTS) s->cfg.max_clients = 4;

    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(cfg->port ? cfg->port : 554); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s->lfd, 8) < 0) {
        LOGE("rtsp: listen on %d: %s", ntohs(a.sin_port), strerror(errno));
        close(s->lfd); free(s); return NULL;
    }
    set_nonblock(s->lfd);
    for (int t = 0; t < NTRACKS; t++) {
        s->rtp_fd[t] = s->rtcp_fd[t] = -1;
        if (t == T_AUDIO && !cfg->audio) continue;
        s->rtp_fd[t] = udp_bind(RTP_PORT_BASE + 2 * t);
        s->rtcp_fd[t] = udp_bind(RTP_PORT_BASE + 2 * t + 1);
    }
    LOGI("rtsp: listening on port %d, path /%s%s%s", ntohs(a.sin_port), s->path,
         s->user[0] ? " (Digest login)" : " (no login)", cfg->audio ? ", with audio" : "");
    return s;
}

void rtsp_set_headers(rtsp_server_t *s, const uint8_t *buf, size_t len)
{
    const uint8_t *p = buf, *end = buf + len, *nal; size_t nl;
    while ((p = next_nal(p, end, &nal, &nl))) {
        int t = nal[0] & 0x1f;
        if (t == 7 && nl <= sizeof(s->sps)) { memcpy(s->sps, nal, nl); s->sps_len = nl; }
        if (t == 8 && nl <= sizeof(s->pps)) { memcpy(s->pps, nal, nl); s->pps_len = nl; }
    }
}

static void client_close(rtsp_server_t *s, int i)
{
    client_t *c = &s->cl[i];
    LOGI("rtsp: client %s:%d disconnected", inet_ntoa(c->peer.sin_addr), ntohs(c->peer.sin_port));
    close(c->fd);
    free(c->txq);
    s->cl[i] = s->cl[--s->ncl];
    memset(&s->cl[s->ncl], 0, sizeof(client_t));
}

/* queue + flush for TCP output; returns -1 if the client must be dropped */
static int tcp_flush(client_t *c)
{
    while (c->txlen) {
        ssize_t w = send(c->fd, c->txq, c->txlen, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        memmove(c->txq, c->txq + w, c->txlen - w);
        c->txlen -= w;
    }
    return 0;
}

static int tcp_queue(client_t *c, const void *d, size_t n)
{
    if (c->txlen + n > c->txcap) {
        size_t nc = c->txcap ? c->txcap * 2 : 64 * 1024;
        while (nc < c->txlen + n) nc *= 2;
        uint8_t *q = realloc(c->txq, nc);
        if (!q) return -1;
        c->txq = q; c->txcap = nc;
    }
    memcpy(c->txq + c->txlen, d, n);
    c->txlen += n;
    return 0;
}

static void send_text(client_t *c, const char *txt)
{
    tcp_queue(c, txt, strlen(txt));
    tcp_flush(c);
}

/* ----------------------------------------------------------- RTSP parsing */

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

/* ------------------------------------------------------------ Digest login */

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

/* new challenge: 128 random bits from the kernel, replaces the oldest slot */
static const char *new_nonce(rtsp_server_t *s)
{
    uint8_t r[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, r, sizeof(r)) != (ssize_t)sizeof(r))
        for (int i = 0; i < 16; i++) r[i] = (uint8_t)rnd32();
    if (fd >= 0) close(fd);
    int k = 0;
    for (int i = 1; i < NONCES; i++) if (s->nonce[i].born < s->nonce[k].born) k = i;
    for (int i = 0; i < 16; i++) sprintf(s->nonce[k].v + 2 * i, "%02x", r[i]);
    s->nonce[k].born = time(NULL);
    return s->nonce[k].v;
}

/* 1 = logged in (or no login configured). A nonce is accepted once, within NONCE_TTL:
 * the login uses it up, so a recorded Authorization line cannot be replayed. */
static int check_auth(rtsp_server_t *s, client_t *c, const char *req, const char *method)
{
    if (!s->user[0] || c->authed) return 1;
    char a[512];
    if (!hdr(req, "Authorization", a, sizeof(a)) || strncasecmp(a, "Digest ", 7)) return 0;
    char user[64], realm[64], nonce[64], uri[256], resp[40];
    if (!param(a + 7, "username", user, sizeof(user)) || !param(a + 7, "realm", realm, sizeof(realm)) ||
        !param(a + 7, "nonce", nonce, sizeof(nonce)) || !param(a + 7, "uri", uri, sizeof(uri)) ||
        !param(a + 7, "response", resp, sizeof(resp))) return 0;
    int k = -1;
    for (int i = 0; i < NONCES; i++) if (s->nonce[i].v[0] && !strcmp(s->nonce[i].v, nonce)) k = i;
    if (k < 0 || time(NULL) - s->nonce[k].born > NONCE_TTL) return 0;
    char tmp[512], ha1[33], ha2[33], exp[33];
    snprintf(tmp, sizeof(tmp), "%s:%s:%s", s->user, REALM, s->pass); md5_hex(tmp, ha1);
    snprintf(tmp, sizeof(tmp), "%s:%s", method, uri); md5_hex(tmp, ha2);
    snprintf(tmp, sizeof(tmp), "%s:%s:%s", ha1, nonce, ha2); md5_hex(tmp, exp);
    int bad = strcmp(user, s->user) != 0 || strcmp(realm, REALM) != 0;
    for (int i = 0; i < 32; i++) bad |= tolower((unsigned char)resp[i]) != exp[i];   /* constant time */
    if (bad) {
        LOGW("rtsp: wrong login from %s", inet_ntoa(c->peer.sin_addr));
        sleep(1);                                       /* slows down password guessing */
        return 0;
    }
    s->nonce[k].v[0] = 0;                               /* used up */
    return c->authed = 1;
}

static void local_ip(int fd, char *out, size_t n)
{
    struct sockaddr_in a; socklen_t l = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &l) == 0) snprintf(out, n, "%s", inet_ntoa(a.sin_addr));
    else snprintf(out, n, "0.0.0.0");
}

static void handle_request(rtsp_server_t *s, int ci, char *req)
{
    client_t *c = &s->cl[ci];
    char method[32] = "", url[256] = "", cseq[32] = "0", out[2048], extra[1024] = "";
    sscanf(req, "%31s %255s", method, url);
    hdr(req, "CSeq", cseq, sizeof(cseq));
    c->last_seen = time(NULL);

    if (strcmp(method, "OPTIONS") && !check_auth(s, c, req, method)) {
        snprintf(out, sizeof(out),
                 "RTSP/1.0 401 Unauthorized\r\nCSeq: %s\r\n"
                 "WWW-Authenticate: Digest realm=\"" REALM "\", nonce=\"%s\"\r\n\r\n", cseq, new_nonce(s));
        send_text(c, out);
        return;
    }

    if (!strcmp(method, "OPTIONS")) {
        snprintf(out, sizeof(out), "RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
                 "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n\r\n", cseq);
    } else if (!strcmp(method, "DESCRIBE")) {
        char ip[32], sps64[200], pps64[100], sdp[1024];
        local_ip(c->fd, ip, sizeof(ip));
        b64(s->sps, s->sps_len, sps64);
        b64(s->pps, s->pps_len, pps64);
        int n = snprintf(sdp, sizeof(sdp),
            "v=0\r\no=- %u 1 IN IP4 %s\r\ns=%s\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\na=range:npt=0-\r\n"
            "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;profile-level-id=%02X%02X%02X;sprop-parameter-sets=%s,%s\r\n"
            "a=framerate:%d\r\na=control:track1\r\n",
            rnd32() & 0xffff, ip, s->name,
            s->sps_len > 3 ? s->sps[1] : 0x4d, s->sps_len > 3 ? s->sps[2] : 0, s->sps_len > 3 ? s->sps[3] : 0x28,
            sps64, pps64, s->cfg.fps);
        if (s->cfg.audio)
            n += snprintf(sdp + n, sizeof(sdp) - n,
                          "m=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000/1\r\na=control:track2\r\n");
        char base[300];
        snprintf(base, sizeof(base), "%s%s", url, url[strlen(url) - 1] == '/' ? "" : "/");
        snprintf(out, sizeof(out), "RTSP/1.0 200 OK\r\nCSeq: %s\r\nContent-Base: %s\r\n"
                 "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s", cseq, base, n, sdp);
    } else if (!strcmp(method, "SETUP")) {
        char tr[256] = "";
        hdr(req, "Transport", tr, sizeof(tr));
        /* which track: ".../track2" = audio, anything else = video */
        int t = strstr(url, "track2") ? T_AUDIO : T_VIDEO;
        if (t == T_AUDIO && !s->cfg.audio) {
            snprintf(out, sizeof(out), "RTSP/1.0 404 Not Found\r\nCSeq: %s\r\n\r\n", cseq);
            send_text(c, out); return;
        }
        track_t *k = &c->tr[t];
        if (!c->session) c->session = rnd32() & 0x7fffffff;
        k->ssrc = rnd32();
        k->seq = (uint16_t)rnd32();
        k->pkts = k->octets = 0;
        if (strstr(tr, "RTP/AVP/TCP") || strstr(tr, "interleaved")) {
            int a = 2 * t, b = 2 * t + 1;
            const char *il = strstr(tr, "interleaved=");
            if (il) sscanf(il + 12, "%d-%d", &a, &b);
            c->transport = TR_TCP; k->ch_rtp = a; k->ch_rtcp = b;
            snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%08X\r\n", a, b, k->ssrc);
        } else {
            int p1 = 0, p2 = 0;
            const char *cp = strstr(tr, "client_port=");
            if (cp) sscanf(cp + 12, "%d-%d", &p1, &p2);
            if (!p1 || s->rtp_fd[t] < 0) {
                snprintf(out, sizeof(out), "RTSP/1.0 461 Unsupported Transport\r\nCSeq: %s\r\n\r\n", cseq);
                send_text(c, out); return;
            }
            if (!p2) p2 = p1 + 1;
            c->transport = TR_UDP;
            k->udp_rtp = c->peer; k->udp_rtp.sin_port = htons(p1);
            k->udp_rtcp = c->peer; k->udp_rtcp.sin_port = htons(p2);
            snprintf(extra, sizeof(extra), "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d;ssrc=%08X\r\n",
                     p1, p2, RTP_PORT_BASE + 2 * t, RTP_PORT_BASE + 2 * t + 1, k->ssrc);
        }
        k->setup = 1;
        c->state = ST_READY;
        snprintf(out, sizeof(out), "RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sSession: %08X;timeout=60\r\n\r\n",
                 cseq, extra, c->session);
    } else if (!strcmp(method, "PLAY")) {
        if (c->state == ST_INIT) {
            snprintf(out, sizeof(out), "RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: %s\r\n\r\n", cseq);
        } else {
            c->state = ST_PLAYING; c->need_key = 1; s->want_key = 1;
            char base[300], info[700] = "";
            snprintf(base, sizeof(base), "%s", url);
            size_t bl = strlen(base);
            if (bl && base[bl - 1] == '/') base[--bl] = 0;
            for (int t = 0; t < NTRACKS; t++) {
                if (!c->tr[t].setup) continue;
                c->tr[t].last_sr = 0;
                size_t il = strlen(info);
                snprintf(info + il, sizeof(info) - il, "%surl=%s/track%d;seq=%u", il ? "," : "", base, t + 1, c->tr[t].seq);
            }
            snprintf(out, sizeof(out), "RTSP/1.0 200 OK\r\nCSeq: %s\r\nSession: %08X\r\nRange: npt=0.000-\r\n"
                     "RTP-Info: %s\r\n\r\n", cseq, c->session, info);
            LOGI("rtsp: %s:%d playing over %s%s", inet_ntoa(c->peer.sin_addr), ntohs(c->peer.sin_port),
                 c->transport == TR_TCP ? "TCP" : "UDP", c->tr[T_AUDIO].setup ? " with audio" : "");
        }
    } else if (!strcmp(method, "PAUSE")) {
        if (c->state == ST_PLAYING) c->state = ST_READY;
        snprintf(out, sizeof(out), "RTSP/1.0 200 OK\r\nCSeq: %s\r\nSession: %08X\r\n\r\n", cseq, c->session);
    } else if (!strcmp(method, "TEARDOWN")) {
        snprintf(out, sizeof(out), "RTSP/1.0 200 OK\r\nCSeq: %s\r\nSession: %08X\r\n\r\n", cseq, c->session);
        send_text(c, out);
        c->state = ST_INIT; c->transport = TR_NONE;
        memset(c->tr, 0, sizeof(c->tr));
        return;
    } else if (!strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER")) {
        snprintf(out, sizeof(out), "RTSP/1.0 200 OK\r\nCSeq: %s\r\nSession: %08X\r\n\r\n", cseq, c->session);
    } else {
        snprintf(out, sizeof(out), "RTSP/1.0 501 Not Implemented\r\nCSeq: %s\r\n\r\n", cseq);
    }
    send_text(c, out);
}

/* consume complete requests (and interleaved RTCP from the client) */
static int client_read(rtsp_server_t *s, int ci)
{
    client_t *c = &s->cl[ci];
    ssize_t r = recv(c->fd, c->rx + c->rxlen, RXBUF - 1 - c->rxlen, MSG_DONTWAIT);
    if (r == 0) return -1;
    if (r < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    c->rxlen += r;
    c->last_seen = time(NULL);
    for (;;) {
        if (c->rxlen >= 4 && c->rx[0] == '$') {            /* interleaved binary (RTCP RR) */
            int len = ((uint8_t)c->rx[2] << 8) | (uint8_t)c->rx[3];
            if (c->rxlen < 4 + len) break;
            memmove(c->rx, c->rx + 4 + len, c->rxlen - 4 - len);
            c->rxlen -= 4 + len;
            continue;
        }
        c->rx[c->rxlen] = 0;
        char *end = strstr(c->rx, "\r\n\r\n");
        if (!end) {
            if (c->rxlen >= RXBUF - 1) return -1;          /* garbage / too long */
            break;
        }
        int hl = (int)(end - c->rx) + 4;
        char cl[16]; int body = 0;
        end[2] = 0;
        if (hdr(c->rx, "Content-Length", cl, sizeof(cl))) body = atoi(cl);
        end[2] = '\r';
        if (c->rxlen < hl + body) break;
        char save = c->rx[hl]; c->rx[hl] = 0;
        handle_request(s, ci, c->rx);
        c->rx[hl] = save;
        memmove(c->rx, c->rx + hl + body, c->rxlen - hl - body);
        c->rxlen -= hl + body;
    }
    return 0;
}

int rtsp_pollfds(rtsp_server_t *s, struct pollfd *p, int max)
{
    int n = 0;
    if (n < max) { p[n].fd = s->lfd; p[n].events = POLLIN; p[n].revents = 0; n++; }
    for (int t = 0; t < NTRACKS; t++)
        if (n < max && s->rtcp_fd[t] >= 0) { p[n].fd = s->rtcp_fd[t]; p[n].events = POLLIN; p[n].revents = 0; n++; }
    for (int i = 0; i < s->ncl && n < max; i++) {
        p[n].fd = s->cl[i].fd;
        p[n].events = POLLIN | (s->cl[i].txlen ? POLLOUT : 0);
        p[n].revents = 0;
        n++;
    }
    return n;
}

void rtsp_process(rtsp_server_t *s, struct pollfd *p, int n)
{
    time_t now = time(NULL);
    for (int k = 0; k < n; k++) {
        if (!p[k].revents) continue;
        if (p[k].fd == s->lfd) {
            struct sockaddr_in a; socklen_t al = sizeof(a);
            int fd = accept(s->lfd, (struct sockaddr *)&a, &al);
            if (fd < 0) continue;
            if (s->ncl >= s->cfg.max_clients) {
                LOGW("rtsp: too many clients, rejecting %s", inet_ntoa(a.sin_addr));
                const char *busy = "RTSP/1.0 453 Not Enough Bandwidth\r\nCSeq: 0\r\n\r\n";
                send(fd, busy, strlen(busy), MSG_DONTWAIT | MSG_NOSIGNAL);
                close(fd);
                continue;
            }
            set_nonblock(fd);
            int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            int sz = 512 * 1024; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
            client_t *c = &s->cl[s->ncl++];
            memset(c, 0, sizeof(*c));
            c->fd = fd; c->peer = a; c->last_seen = now;
            LOGI("rtsp: client %s:%d connected", inet_ntoa(a.sin_addr), ntohs(a.sin_port));
        } else if (p[k].fd == s->rtcp_fd[T_VIDEO] || p[k].fd == s->rtcp_fd[T_AUDIO]) {
            struct sockaddr_in a; socklen_t al = sizeof(a); char b[1500];
            while (recvfrom(p[k].fd, b, sizeof(b), MSG_DONTWAIT, (struct sockaddr *)&a, &al) > 0)
                for (int i = 0; i < s->ncl; i++)
                    if (s->cl[i].transport == TR_UDP && s->cl[i].peer.sin_addr.s_addr == a.sin_addr.s_addr)
                        s->cl[i].last_seen = now;
        }
    }
    /* clients (iterate backwards: client_close swaps in the last entry) */
    for (int i = s->ncl - 1; i >= 0; i--) {
        client_t *c = &s->cl[i];
        short ev = 0;
        for (int k = 0; k < n; k++) if (p[k].fd == c->fd) { ev = p[k].revents; break; }
        int drop = 0;
        if (ev & (POLLERR | POLLHUP | POLLNVAL)) drop = 1;
        if (!drop && (ev & POLLIN) && client_read(s, i) < 0) drop = 1;
        if (!drop && c->txlen && tcp_flush(c) < 0) drop = 1;
        if (!drop && now - c->last_seen > SESSION_TIMEOUT) { LOGW("rtsp: session timeout"); drop = 1; }
        if (drop) client_close(s, i);
    }
}

int rtsp_playing(rtsp_server_t *s)
{
    int n = 0;
    for (int i = 0; i < s->ncl; i++) n += s->cl[i].state == ST_PLAYING;
    return n;
}

int rtsp_want_keyframe(rtsp_server_t *s)
{
    int w = s->want_key;
    s->want_key = 0;
    return w;
}

uint64_t rtsp_take_bytes(rtsp_server_t *s)
{
    uint64_t b = s->bytes;
    s->bytes = 0;
    return b;
}

/* ------------------------------------------------------------ RTP output */

static void emit(rtsp_server_t *s, client_t *c, int t, const uint8_t *pkt, size_t len, int rtcp)
{
    track_t *k = &c->tr[t];
    if (c->transport == TR_TCP) {
        uint8_t h[4] = { '$', (uint8_t)(rtcp ? k->ch_rtcp : k->ch_rtp), (uint8_t)(len >> 8), (uint8_t)len };
        tcp_queue(c, h, 4);
        tcp_queue(c, pkt, len);
    } else {
        sendto(rtcp ? s->rtcp_fd[t] : s->rtp_fd[t], pkt, len, MSG_DONTWAIT,
               (struct sockaddr *)(rtcp ? &k->udp_rtcp : &k->udp_rtp), sizeof(struct sockaddr_in));
    }
    s->bytes += len;
}

/* RTCP sender report: maps the RTP time stamp to wall clock time, which lets the
 * player line up audio and video (both time stamps come from the capture clock) */
static void send_sr(rtsp_server_t *s, client_t *c, int t, uint32_t rtpts)
{
    track_t *k = &c->tr[t];
    struct timeval tv; gettimeofday(&tv, NULL);
    uint32_t ntp_sec = (uint32_t)tv.tv_sec + 2208988800u;
    uint32_t ntp_frac = (uint32_t)((double)tv.tv_usec * 4294.967296);
    uint8_t p[28];
    p[0] = 0x80; p[1] = 200; p[2] = 0; p[3] = 6;
    uint32_t v[6] = { k->ssrc, ntp_sec, ntp_frac, rtpts, k->pkts, k->octets };
    for (int i = 0; i < 6; i++) { p[4 + i * 4] = v[i] >> 24; p[5 + i * 4] = v[i] >> 16; p[6 + i * 4] = v[i] >> 8; p[7 + i * 4] = v[i]; }
    emit(s, c, t, p, sizeof(p), 1);
}

static void rtp_packet(rtsp_server_t *s, client_t *c, int t, int pt, uint32_t ts, int marker,
                       const uint8_t *h2, size_t h2len, const uint8_t *pl, size_t plen)
{
    track_t *k = &c->tr[t];
    uint8_t pkt[12 + 2 + RTP_MTU];
    pkt[0] = 0x80;
    pkt[1] = (marker ? 0x80 : 0) | pt;
    pkt[2] = k->seq >> 8; pkt[3] = k->seq & 0xff; k->seq++;
    pkt[4] = ts >> 24; pkt[5] = ts >> 16; pkt[6] = ts >> 8; pkt[7] = ts;
    pkt[8] = k->ssrc >> 24; pkt[9] = k->ssrc >> 16; pkt[10] = k->ssrc >> 8; pkt[11] = k->ssrc;
    memcpy(pkt + 12, h2, h2len);
    memcpy(pkt + 12 + h2len, pl, plen);
    emit(s, c, t, pkt, 12 + h2len + plen, 0);
    k->pkts++; k->octets += h2len + plen;
}

static void send_nal(rtsp_server_t *s, client_t *c, uint32_t ts, const uint8_t *nal, size_t len, int last)
{
    if (len <= RTP_MTU) { rtp_packet(s, c, T_VIDEO, 96, ts, last, NULL, 0, nal, len); return; }
    uint8_t fu[2];
    fu[0] = (nal[0] & 0xe0) | 28;               /* FU-A indicator */
    const uint8_t *p = nal + 1;
    size_t left = len - 1;
    int first = 1;
    while (left) {
        size_t chunk = left > RTP_MTU - 2 ? RTP_MTU - 2 : left;
        fu[1] = (nal[0] & 0x1f) | (first ? 0x80 : 0) | (chunk == left ? 0x40 : 0);
        rtp_packet(s, c, T_VIDEO, 96, ts, last && chunk == left, fu, 2, p, chunk);
        p += chunk; left -= chunk; first = 0;
    }
}

void rtsp_send_frame(rtsp_server_t *s, const uint8_t *au, size_t len, int key, int64_t pts_us)
{
    uint32_t ts = (uint32_t)((uint64_t)pts_us * 9 / 100);
    time_t now = time(NULL);
    for (int i = 0; i < s->ncl; i++) {
        client_t *c = &s->cl[i];
        if (c->state != ST_PLAYING || !c->tr[T_VIDEO].setup) continue;
        if (c->need_key && !key) continue;
        if (c->transport == TR_TCP && c->txlen > TCP_QUEUE_MAX) {  /* client too slow: drop until next IDR */
            c->need_key = 1; s->want_key = 1;
            continue;
        }
        c->need_key = 0;
        if (now - c->tr[T_VIDEO].last_sr >= 5) { send_sr(s, c, T_VIDEO, ts); c->tr[T_VIDEO].last_sr = now; }
        const uint8_t *p = au, *end = au + len, *nal; size_t nl;
        const uint8_t *pend = NULL; size_t plen = 0;
        while ((p = next_nal(p, end, &nal, &nl))) {       /* send previous; mark the last one */
            if (pend) send_nal(s, c, ts, pend, plen, 0);
            pend = nal; plen = nl;
        }
        if (pend) send_nal(s, c, ts, pend, plen, 1);
        if (c->transport == TR_TCP) tcp_flush(c);
    }
}

void rtsp_send_audio(rtsp_server_t *s, const uint8_t *ulaw, size_t n, int64_t pts_us)
{
    uint32_t ts = (uint32_t)((uint64_t)pts_us * 8 / 1000);        /* 8 kHz RTP clock */
    time_t now = time(NULL);
    for (int i = 0; i < s->ncl; i++) {
        client_t *c = &s->cl[i];
        if (c->state != ST_PLAYING || !c->tr[T_AUDIO].setup) continue;
        if (c->transport == TR_TCP && c->txlen > TCP_QUEUE_MAX) continue;   /* lagging client */
        if (now - c->tr[T_AUDIO].last_sr >= 5) { send_sr(s, c, T_AUDIO, ts); c->tr[T_AUDIO].last_sr = now; }
        rtp_packet(s, c, T_AUDIO, 0, ts, 0, NULL, 0, ulaw, n);
        if (c->transport == TR_TCP) tcp_flush(c);
    }
}

void rtsp_destroy(rtsp_server_t *s)
{
    if (!s) return;
    while (s->ncl) client_close(s, s->ncl - 1);
    close(s->lfd);
    for (int t = 0; t < NTRACKS; t++) {
        if (s->rtp_fd[t] >= 0) close(s->rtp_fd[t]);
        if (s->rtcp_fd[t] >= 0) close(s->rtcp_fd[t]);
    }
    free(s);
}
