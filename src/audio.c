/* Microphone capture for the RTSP audio track: ALSA (card 0, the internal
 * codec) at 8 kHz mono, encoded to G.711 mu-law, no other processing. */
#include "audio.h"
#include "alsa.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct audio {
    alsa_t pcm;
    int pipe[2];
    clockid_t clock;
    pthread_t thread;
    volatile int stop;
};

/* G.711 mu-law encoder (ITU-T G.711, as in the Sun reference code) */
static uint8_t ulaw(int16_t pcm)
{
    int s = pcm;
    int sign = (s >> 8) & 0x80;
    if (sign) s = -s;
    if (s > 32635) s = 32635;
    s += 0x84;
    int exp = 7;
    for (int mask = 0x4000; !(s & mask) && exp > 0; mask >>= 1) exp--;
    int mant = (s >> (exp + 3)) & 0x0f;
    return (uint8_t)~(sign | (exp << 4) | mant);
}

static int64_t now_us(clockid_t clk)
{
    struct timespec t; clock_gettime(clk, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void *run(void *arg)
{
    audio_t *a = arg;
    int16_t pcm[AUDIO_BLOCK];
    audio_block_t b;
    int64_t base = -1, samples = 0;
    const int64_t block_us = (int64_t)AUDIO_BLOCK * 1000000 / AUDIO_RATE;
    while (!a->stop) {
        if (alsa_read(&a->pcm, pcm, AUDIO_BLOCK) < 0) {
            LOGW("audio: %s", a->pcm.err);
            usleep(200000);
            base = -1;
            continue;
        }
        /* continuous time stamps from the sample count; re-anchored to the
         * clock if they drift more than 200 ms (e.g. after an overrun) */
        int64_t start = now_us(a->clock) - block_us;
        int64_t pts = base < 0 ? start : base + samples * 1000000 / AUDIO_RATE;
        if (base < 0 || llabs(pts - start) > 200000) { base = start; samples = 0; pts = start; }
        samples += AUDIO_BLOCK;
        b.pts_us = pts;
        for (int i = 0; i < AUDIO_BLOCK; i++) b.ulaw[i] = ulaw(pcm[i]);
        if (write(a->pipe[1], &b, sizeof(b)) < 0 && errno != EAGAIN) break;   /* EAGAIN: main loop busy, drop */
    }
    return NULL;
}

audio_t *audio_start(int gain, clockid_t clock)
{
    audio_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->clock = clock;
    if (gain < 0) gain = 0;
    if (gain > 7) gain = 7;
    /* route the built-in microphone (mic1) to the ADC; hardware noise reduction off */
    alsa_mixer_set(0, "Audio main mic", 1);
    alsa_mixer_set(0, "Audio capture route switch", 1);
    alsa_mixer_set(0, "Audio record source", 0);            /* mic1 */
    alsa_mixer_set(0, "Audio noise reduced", 0);
    alsa_mixer_set(0, "MIC1 boost AMP gain control", gain);
    if (alsa_open(&a->pcm, 0, 0, 1, AUDIO_RATE, 1, AUDIO_BLOCK, 4) < 0) {
        LOGE("audio: microphone: %s", a->pcm.err);
        free(a);
        return NULL;
    }
    if (pipe(a->pipe) < 0) { alsa_close(&a->pcm); free(a); return NULL; }
    fcntl(a->pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(a->pipe[1], F_SETFL, O_NONBLOCK);
    if (pthread_create(&a->thread, NULL, run, a) != 0) {
        close(a->pipe[0]); close(a->pipe[1]); alsa_close(&a->pcm); free(a);
        return NULL;
    }
    LOGI("audio: microphone on, G.711 mu-law %d Hz, gain %d", AUDIO_RATE, gain);
    return a;
}

int audio_fd(audio_t *a) { return a ? a->pipe[0] : -1; }

int audio_read(audio_t *a, audio_block_t *b)
{
    return a && read(a->pipe[0], b, sizeof(*b)) == (ssize_t)sizeof(*b);
}

void audio_stop(audio_t *a)
{
    if (!a) return;
    a->stop = 1;
    pthread_join(a->thread, NULL);       /* returns within one block (40 ms) */
    alsa_close(&a->pcm);
    close(a->pipe[0]); close(a->pipe[1]);
    alsa_mixer_set(0, "Audio main mic", 0);
    free(a);
}
