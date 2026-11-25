#ifndef WPACTRL_H
#define WPACTRL_H
#include <stddef.h>

/* Minimal client for the wpa_supplicant control socket. */
typedef struct { int fd; char local[108]; } wpactrl_t;

int  wpactrl_open(wpactrl_t *w, const char *ctrl_path);   /* e.g. /dev/cam/wpa/wlan0 */
/* Send a command; reply is NUL-terminated. Returns reply length or -1. */
int  wpactrl_cmd(wpactrl_t *w, const char *cmd, char *reply, size_t len, int timeout_ms);
void wpactrl_close(wpactrl_t *w);

#endif
