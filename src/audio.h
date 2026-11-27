/* Microphone -> G.711 mu-law, 8 kHz mono, 40 ms blocks, in a thread.
 * Blocks arrive through a pipe so the single-threaded RTSP loop can poll it. */
#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>
#include <time.h>

#define AUDIO_RATE  8000
#define AUDIO_BLOCK 320                 /* samples = bytes per block (40 ms) */

typedef struct {
    int64_t pts_us;                     /* first sample, in the capture clock given to audio_start */
    uint8_t ulaw[AUDIO_BLOCK];
} audio_block_t;

typedef struct audio audio_t;

/* gain: microphone boost 0..7. clock: the clock the video time stamps use.
 * Returns NULL if the microphone cannot be opened. */
audio_t *audio_start(int gain, clockid_t clock);
/* fd to poll for POLLIN; then audio_read() returns one block (1) or nothing (0) */
int  audio_fd(audio_t *a);
int  audio_read(audio_t *a, audio_block_t *b);
void audio_stop(audio_t *a);

#endif
