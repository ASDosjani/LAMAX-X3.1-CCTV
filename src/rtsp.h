#ifndef RTSP_H
#define RTSP_H
#include <poll.h>
#include <stddef.h>
#include <stdint.h>

/* Small single-threaded RTSP/RTP server for one H.264 stream.
 * Supports RTP over UDP (unicast) and RTP over the RTSP TCP connection
 * (interleaved), Basic and Digest authentication, RTCP sender reports. */

typedef struct rtsp_server rtsp_server_t;

typedef struct {
    int port;               /* RTSP TCP port, default 554 */
    const char *path;       /* stream path, e.g. "stream" (any path is accepted) */
    const char *user;       /* empty/NULL = no authentication */
    const char *password;
    const char *name;       /* session name in SDP */
    int fps;
    int max_clients;
} rtsp_cfg_t;

rtsp_server_t *rtsp_create(const rtsp_cfg_t *cfg);
/* SPS/PPS Annex-B header from the encoder (needed for SDP) */
void rtsp_set_headers(rtsp_server_t *s, const uint8_t *annexb, size_t len);
/* Fill pollfds; returns count used. */
int  rtsp_pollfds(rtsp_server_t *s, struct pollfd *p, int max);
/* Handle activity reported by poll() for the fds filled by rtsp_pollfds. */
void rtsp_process(rtsp_server_t *s, struct pollfd *p, int n);
/* Number of clients currently in PLAY state. */
int  rtsp_playing(rtsp_server_t *s);
/* Non-zero once if a client started playing and needs a key frame. */
int  rtsp_want_keyframe(rtsp_server_t *s);
/* Send one encoded access unit (Annex-B) to all playing clients. */
void rtsp_send_frame(rtsp_server_t *s, const uint8_t *au, size_t len, int key, int64_t pts_us);
/* Total bytes sent since the last call (for statistics). */
uint64_t rtsp_take_bytes(rtsp_server_t *s);
void rtsp_destroy(rtsp_server_t *s);

#endif
