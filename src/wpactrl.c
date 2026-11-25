#include "wpactrl.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int wpactrl_open(wpactrl_t *w, const char *path)
{
    static int counter;
    memset(w, 0, sizeof(*w));
    w->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (w->fd < 0) return -1;
    struct sockaddr_un a; memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(w->local, sizeof(w->local), "/dev/cam/camui_%d_%d", (int)getpid(), counter++);
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", w->local);
    unlink(w->local);
    if (bind(w->fd, (struct sockaddr *)&a, sizeof(a)) < 0) goto fail;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (connect(w->fd, (struct sockaddr *)&a, sizeof(a)) < 0) goto fail;
    return 0;
fail:
    close(w->fd); unlink(w->local); w->fd = -1;
    return -1;
}

int wpactrl_cmd(wpactrl_t *w, const char *cmd, char *reply, size_t len, int timeout_ms)
{
    if (w->fd < 0) return -1;
    /* Drop stale datagrams first: a reply that arrived after an earlier
     * timeout (e.g. a slow SCAN_RESULTS) would otherwise be taken as the
     * answer to this command, and every later answer would be off by one. */
    char junk[512];
    while (recv(w->fd, junk, sizeof(junk), MSG_DONTWAIT) > 0) {}
    if (send(w->fd, cmd, strlen(cmd), 0) < 0) return -1;
    for (;;) {
        struct pollfd p = { .fd = w->fd, .events = POLLIN };
        if (poll(&p, 1, timeout_ms) <= 0) return -1;
        ssize_t r = recv(w->fd, reply, len - 1, 0);
        if (r < 0) return -1;
        reply[r] = 0;
        if (r > 0 && reply[0] == '<') continue;  /* unsolicited event message, skip */
        return (int)r;
    }
}

void wpactrl_close(wpactrl_t *w)
{
    if (w->fd >= 0) { close(w->fd); unlink(w->local); }
    w->fd = -1;
}
