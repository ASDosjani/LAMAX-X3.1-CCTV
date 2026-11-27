/* Minimal ALSA PCM access with the kernel ioctls (no libtinyalsa: the camera's
 * copy crashes in pcm_read). 16-bit signed little-endian, interleaved. */
#ifndef ALSA_H
#define ALSA_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    int capture;
    unsigned channels, rate, period, periods;
    int running;
    char err[128];
} alsa_t;

/* 0 = ok, -1 = error (text in a->err) */
int  alsa_open(alsa_t *a, int card, int device, int capture, unsigned rate, unsigned channels,
               unsigned period_frames, unsigned periods);
/* read exactly `frames` frames (capture); restarts after an overrun. 0 = ok */
int  alsa_read(alsa_t *a, int16_t *buf, unsigned frames);
/* write `frames` frames (playback); restarts after an underrun. 0 = ok */
int  alsa_write(alsa_t *a, const int16_t *buf, unsigned frames);
void alsa_close(alsa_t *a);

/* set a mixer control by name (integer / boolean / enum index), 0 = ok */
int  alsa_mixer_set(int card, const char *name, int value);

#endif
