/* On-screen text burned into the video: date/time, optionally the camera name and,
 * while running from the battery, the battery percentage. */
#ifndef OSD_H
#define OSD_H
#include <stdint.h>

typedef struct osd osd_t;

/* position: top-left, top-right, bottom-left, bottom-right.
 * name: printed before the time, or NULL / "" for none.
 * battery: append "BAT 73%" while there is no external power.
 * Local time follows the TZ environment variable (POSIX form, e.g. CET-1CEST,M3.5.0,M10.5.0/3). */
osd_t *osd_create(int width, int height, const char *position, const char *name, int battery);
/* Draw onto an NV12 frame (Y plane, interleaved UV plane, both with this stride). */
void osd_draw(osd_t *o, uint8_t *y, uint8_t *uv, int stride);
void osd_free(osd_t *o);

#endif
