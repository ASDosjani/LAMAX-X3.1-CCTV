/* Minimal ALSA PCM + mixer access with the kernel ioctls (after tinyalsa,
 * BSD licence, simplified). */
#include "alsa.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

static void set_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
    struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof(*m));
    m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void set_int(struct snd_pcm_hw_params *p, int n, unsigned v)
{
    struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v;
    i->integer = 1;
}

static void init_params(struct snd_pcm_hw_params *p)
{
    memset(p, 0, sizeof(*p));
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
        struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
        m->bits[0] = m->bits[1] = ~0u;
    }
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
        i->min = 0; i->max = ~0u;
    }
    p->rmask = ~0u; p->cmask = 0; p->info = ~0u;
}

static int fail(alsa_t *a, const char *what)
{
    snprintf(a->err, sizeof(a->err), "%s: %s", what, strerror(errno));
    return -1;
}

int alsa_open(alsa_t *a, int card, int device, int capture, unsigned rate, unsigned channels,
              unsigned period, unsigned periods)
{
    memset(a, 0, sizeof(*a));
    a->capture = capture; a->rate = rate; a->channels = channels; a->period = period; a->periods = periods;
    char fn[64];
    snprintf(fn, sizeof(fn), "/dev/snd/pcmC%dD%d%c", card, device, capture ? 'c' : 'p');
    a->fd = open(fn, O_RDWR);
    if (a->fd < 0) return fail(a, fn);

    struct snd_pcm_hw_params p;
    init_params(&p);
    set_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    set_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    set_mask(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    set_int(&p, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    set_int(&p, SNDRV_PCM_HW_PARAM_FRAME_BITS, 16 * channels);
    set_int(&p, SNDRV_PCM_HW_PARAM_CHANNELS, channels);
    set_int(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);
    set_int(&p, SNDRV_PCM_HW_PARAM_PERIODS, periods);
    set_int(&p, SNDRV_PCM_HW_PARAM_RATE, rate);
    if (ioctl(a->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0) { fail(a, "hw_params"); close(a->fd); a->fd = -1; return -1; }

    struct snd_pcm_sw_params s;
    memset(&s, 0, sizeof(s));
    s.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
    s.period_step = 1;
    s.avail_min = period;
    s.start_threshold = capture ? 1 : period * (periods - 1);
    s.stop_threshold = period * periods;
    s.xfer_align = period / 2;
    s.silence_size = 0;
    s.silence_threshold = 0;
    s.boundary = period * periods;
    while (s.boundary * 2 <= 0x7fffffffu / 2) s.boundary *= 2;
    if (ioctl(a->fd, SNDRV_PCM_IOCTL_SW_PARAMS, &s) < 0) { fail(a, "sw_params"); close(a->fd); a->fd = -1; return -1; }
    if (ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { fail(a, "prepare"); close(a->fd); a->fd = -1; return -1; }
    return 0;
}

int alsa_read(alsa_t *a, int16_t *buf, unsigned frames)
{
    if (!a->running) {
        if (ioctl(a->fd, SNDRV_PCM_IOCTL_START) < 0) return fail(a, "start");
        a->running = 1;
    }
    for (int tries = 0; tries < 2; tries++) {
        struct snd_xferi x = { .result = 0, .buf = buf, .frames = frames };
        if (ioctl(a->fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) == 0) return 0;
        if (errno != EPIPE) return fail(a, "read");
        /* overrun: prepare and start again */
        ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE);
        if (ioctl(a->fd, SNDRV_PCM_IOCTL_START) < 0) return fail(a, "restart");
    }
    return fail(a, "read after overrun");
}

int alsa_write(alsa_t *a, const int16_t *buf, unsigned frames)
{
    for (int tries = 0; tries < 2; tries++) {
        struct snd_xferi x = { .result = 0, .buf = (void *)buf, .frames = frames };
        if (ioctl(a->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) return 0;
        if (errno != EPIPE) return fail(a, "write");
        ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE);        /* underrun */
    }
    return fail(a, "write after underrun");
}

void alsa_close(alsa_t *a)
{
    if (a->fd >= 0) {
        /* playback: let the buffered sound finish; capture: just stop */
        ioctl(a->fd, a->capture ? SNDRV_PCM_IOCTL_DROP : SNDRV_PCM_IOCTL_DRAIN);
        close(a->fd);
    }
    a->fd = -1;
}

int alsa_mixer_set(int card, const char *name, int value)
{
    char fn[32];
    snprintf(fn, sizeof(fn), "/dev/snd/controlC%d", card);
    int fd = open(fn, O_RDWR);
    if (fd < 0) return -1;
    struct snd_ctl_elem_id id;
    memset(&id, 0, sizeof(id));
    id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    snprintf((char *)id.name, sizeof(id.name), "%s", name);
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof(info));
    info.id = id;
    int r = -1;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0) {
        struct snd_ctl_elem_value v;
        memset(&v, 0, sizeof(v));
        v.id = info.id;
        for (unsigned i = 0; i < info.count && i < 128; i++) {
            if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) v.value.enumerated.item[i] = (unsigned)value;
            else v.value.integer.value[i] = value;
        }
        r = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) == 0 ? 0 : -1;
    }
    close(fd);
    return r;
}
