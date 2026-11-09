// SPDX-License-Identifier: LGPL-2.1-or-later
/*
  Test slave for the selftest: an ioplug "device" that consumes audio in real time
  (CLOCK_MONOTONIC) from the moment it is started, discards it, and reports a true
  delay. Supports pause. Not for production use.

    pcm_type.fakeslave { lib "/path/to/libasound_module_pcm_fakeslave.so" }
    pcm.fake { type fakeslave }
*/

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

typedef struct
{
  snd_pcm_ioplug_t io;
  int tfd;
  int running, paused;
  int64_t t0, pause_ns, paused_total;
  uint64_t written;
  long hidden; /* FAKE_HIDDEN_FRAMES: delay beyond the ALSA buffer, like a driver's own DMA ring */
  long gran;   /* FAKE_GRAN_FRAMES: position moves in steps of this many frames (BATCH drivers) */
  long refuse_ms;   /* FAKE_REFUSE_MS: writes are refused for this long after open, space is still reported */
  long derr_from, derr_to; /* FAKE_DELAY_ERR_MS=from:to: delay fails with -EIO this long after start */
  int64_t open_ns;
} fake_t;

static int64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Frames the device has played so far (uncapped) */
static uint64_t played(fake_t *f)
{
  if (!f->running)
    return 0;
  int64_t ref = f->paused ? f->pause_ns : now_ns();
  int64_t el = ref - f->t0 - f->paused_total;
  return el > 0 ? (uint64_t)el * f->io.rate / 1000000000ULL : 0;
}

static uint64_t consumed(fake_t *f)
{
  uint64_t p = played(f);
  if (f->gran > 0)
    p -= p % (uint64_t)f->gran;
  return p < f->written ? p : f->written;
}

static int fk_start(snd_pcm_ioplug_t *io)
{
  fake_t *f = io->private_data;
  f->t0 = now_ns();
  f->paused = 0;
  f->paused_total = 0;
  f->running = 1;
  return 0;
}

static int fk_stop(snd_pcm_ioplug_t *io)
{
  fake_t *f = io->private_data;
  f->running = 0;
  return 0;
}

static snd_pcm_sframes_t fk_pointer(snd_pcm_ioplug_t *io)
{
  fake_t *f = io->private_data;
  if (io->state == SND_PCM_STATE_SETUP) /* a real driver refuses the position query here */
    return -EBADFD;
  if (f->running && !f->paused && played(f) > f->written)
    return -EPIPE;
  return (snd_pcm_sframes_t)(consumed(f) % io->buffer_size);
}

static int fk_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
  fake_t *f = io->private_data;
  if (f->running && !f->paused && played(f) > f->written)
    return -EPIPE;
  if (f->derr_to > 0 && f->running)
  {
    int64_t el = (now_ns() - f->t0 - f->paused_total) / 1000000;
    if (el >= f->derr_from && el < f->derr_to)
      return -EIO;
  }
  *delayp = (snd_pcm_sframes_t)(f->written - consumed(f)) + (f->running ? f->hidden : 0);
  return 0;
}

static snd_pcm_sframes_t fk_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
                                     snd_pcm_uframes_t size)
{
  fake_t *f = io->private_data;
  (void)areas;
  (void)offset;
  if (f->refuse_ms > 0 && now_ns() - f->open_ns < f->refuse_ms * 1000000LL)
    return -EAGAIN;
  f->written += size;
  return (snd_pcm_sframes_t)size;
}

static int fk_prepare(snd_pcm_ioplug_t *io)
{
  fake_t *f = io->private_data;
  f->running = 0;
  f->paused = 0;
  f->written = 0;
  return 0;
}

static int fk_pause(snd_pcm_ioplug_t *io, int enable)
{
  fake_t *f = io->private_data;
  if (enable && !f->paused)
  {
    f->paused = 1;
    f->pause_ns = now_ns();
  }
  else if (!enable && f->paused)
  {
    f->paused_total += now_ns() - f->pause_ns;
    f->paused = 0;
  }
  return 0;
}

static int fk_drain(snd_pcm_ioplug_t *io)
{
  fake_t *f = io->private_data;
  if (io->nonblock)
    return f->running && consumed(f) < f->written ? -EAGAIN : 0;
  while (f->running && consumed(f) < f->written)
    usleep(1000);
  return 0;
}

static int fk_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds, unsigned short *revents)
{
  fake_t *f = io->private_data;
  uint64_t ticks;
  (void)nfds;
  if (pfd[0].revents & POLLIN)
  {
    ssize_t n = read(f->tfd, &ticks, sizeof(ticks));
    (void)n;
  }
  /* own counters only: no snd_pcm_* call on this PCM from here */
  uint64_t used = f->written - consumed(f);
  uint64_t avail = used >= io->buffer_size ? 0 : io->buffer_size - used;
  *revents = avail >= io->period_size ? POLLOUT : 0;
  return 0;
}

static int fk_close(snd_pcm_ioplug_t *io)
{
  fake_t *f = io->private_data;
  close(f->tfd);
  free(f);
  return 0;
}

static const snd_pcm_ioplug_callback_t fk_callback = {
    .start = fk_start,
    .stop = fk_stop,
    .pointer = fk_pointer,
    .transfer = fk_transfer,
    .close = fk_close,
    .prepare = fk_prepare,
    .drain = fk_drain,
    .pause = fk_pause,
    .poll_revents = fk_poll_revents,
    .delay = fk_delay,
};

SND_PCM_PLUGIN_DEFINE_FUNC(fakeslave)
{
  static const unsigned access[] = {SND_PCM_ACCESS_RW_INTERLEAVED};
  static const unsigned formats[] = {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_FLOAT_LE};
  static const unsigned rates[] = {44100, 48000};
  int err;
  (void)root;
  (void)conf;

  if (stream != SND_PCM_STREAM_PLAYBACK)
    return -EINVAL;
  fake_t *f = calloc(1, sizeof(*f));
  if (!f)
    return -ENOMEM;
  const char *env;
  f->hidden = (env = getenv("FAKE_HIDDEN_FRAMES")) ? atol(env) : 0;
  f->gran = (env = getenv("FAKE_GRAN_FRAMES")) ? atol(env) : 0;
  f->refuse_ms = (env = getenv("FAKE_REFUSE_MS")) ? atol(env) : 0;
  if ((env = getenv("FAKE_DELAY_ERR_MS")))
    sscanf(env, "%ld:%ld", &f->derr_from, &f->derr_to);
  f->open_ns = now_ns();
  unsigned max_buffer_bytes = (env = getenv("FAKE_MAX_BUFFER_BYTES")) ? (unsigned)atol(env) : 1u << 21;
  f->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (f->tfd < 0)
  {
    free(f);
    return -errno;
  }
  struct itimerspec its = {{0, 2000000}, {0, 2000000}};
  timerfd_settime(f->tfd, 0, &its, NULL);

  f->io.version = SND_PCM_IOPLUG_VERSION;
  f->io.name = "fakeslave";
  f->io.callback = &fk_callback;
  f->io.private_data = f;
  f->io.poll_fd = f->tfd;
  f->io.poll_events = POLLIN;
  f->io.mmap_rw = 0;
  if ((err = snd_pcm_ioplug_create(&f->io, name, stream, mode)) < 0)
  {
    close(f->tfd);
    free(f);
    return err;
  }
  if ((err = snd_pcm_ioplug_set_param_list(&f->io, SND_PCM_IOPLUG_HW_ACCESS, 1, access)) < 0 ||
      (err = snd_pcm_ioplug_set_param_list(&f->io, SND_PCM_IOPLUG_HW_FORMAT, 3, formats)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(&f->io, SND_PCM_IOPLUG_HW_CHANNELS, 1, 8)) < 0 ||
      (err = snd_pcm_ioplug_set_param_list(&f->io, SND_PCM_IOPLUG_HW_RATE, 2, rates)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(&f->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 256, 1 << 18)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(&f->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 1024)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(&f->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 512, max_buffer_bytes)) < 0)
  {
    snd_pcm_ioplug_delete(&f->io);
    return err;
  }
  *pcmp = f->io.pcm;
  return 0;
}

SND_PCM_PLUGIN_SYMBOL(fakeslave);
