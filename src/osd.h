/* On-screen text burned into the video: date/time and optionally the camera name. */
#ifndef OSD_H
#define OSD_H
#include <stdint.h>

typedef struct osd osd_t;

/* position: top-left, top-right, bottom-left, bottom-right.
 * name: printed before the time, or NULL / "" for none.
 * Local time follows the TZ environment variable (POSIX form, e.g. CET-1CEST,M3.5.0,M10.5.0/3). */
osd_t *osd_create(int width, int height, const char *position, const char *name);
/* Draw onto an NV12 frame (Y plane, interleaved UV plane, both with this stride). */
void osd_draw(osd_t *o, uint8_t *y, uint8_t *uv, int stride);
void osd_free(osd_t *o);

#endif
