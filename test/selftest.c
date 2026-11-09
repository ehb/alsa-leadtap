// SPDX-License-Identifier: LGPL-2.1-or-later
/*
  alsa-leadtap selftest. No audio hardware: the slave is test/fake_slave.c, a device
  that consumes in real time and reports a true delay. Its options (environment, read
  when the slave is opened) model what real slaves do: FAKE_HIDDEN_FRAMES (audio delayed
  beyond the ALSA buffer), FAKE_GRAN_FRAMES (position moves in steps, like BATCH drivers),
  FAKE_MAX_BUFFER_BYTES (a slave with a small maximum buffer).

    selftest <libasound_module_pcm_leadtap.so> <libasound_module_pcm_fakeslave.so>

  Every check prints PASS/FAIL; "ALL OK" only if every one passed. A watchdog ends the run
  with FAIL if a test hangs.
   - Copy timing: the copy of marked audio reaches the pipe `lead` before the computed play
     time. 44.1/48 kHz, S16/S32/FLOAT input, little and much audio queued at start, paced
     writes, a buffer smaller than the stream (position wraps), pause, writes during pause,
     a slave with hidden delay, two pipes (also with one unread), a reader that leaves and
     comes back, hw_params twice.
   - Continuity: with a period-granular slave position the copy of a pure tone has no
     discontinuity (no stutter from re-anchoring).
   - IEC 61937 input gives a silent copy; drain returns after the audio has played, also
     in non-blocking mode.
   - drop empties the queue; a new stream after prepare, after an xrun and after close
     while running works; the state is SETUP (not XRUN) after drop and drain.
   - A slave with a small maximum buffer: the application is held inside it, no hang.
   - The delay and position the application sees follow the slave.
  A pause holds the copy that is not yet released; what was already handed out before the
  pause (up to `lead` of play time) stays out. So a marker is shifted by the pause only if
  its release time (play time - lead) lies after the start of the pause.

  Tolerances (documented, not tuned): a marker may arrive 7 ms earlier or 14 ms later
  than lead before the play time. The pacer releases what is due within the next 5 ms, so
  the copy is up to 5 ms early; scheduler and pipe read latency make it later.
*/

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#define LEAD_S      0.180
#define OUT_RATE    48000
#define DEV_MIN_MS  (-7.0)
#define DEV_MAX_MS  14.0
#define DELAY_TOL_S 0.003
#define WATCHDOG_S  45

static int fails, passes;
static char fifo_path[512], fifo2_path[512];
static const char *cur_test = "";

static void check(int ok, const char *fmt, ...)
{
  va_list ap;
  printf("%s ", ok ? "PASS" : "FAIL");
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  fflush(stdout);
  if (ok)
    passes++;
  else
    fails++;
}

static void on_alarm(int sig)
{
  static const char a[] = "FAIL watchdog: test hung: ";
  (void)sig;
  if (write(1, a, sizeof(a) - 1) < 0 || write(1, cur_test, strlen(cur_test)) < 0 || write(1, "\n", 1) < 0)
    _exit(1);
  _exit(1);
}

static void begin(const char *name)
{
  cur_test = name;
  alarm(WATCHDOG_S);
}

static double now_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void sleep_until(double t)
{
  double d = t - now_s();
  if (d > 0)
    usleep((useconds_t)(d * 1e6));
}

static void setenv_l(const char *name, long v)
{
  char val[32];
  snprintf(val, sizeof(val), "%ld", v);
  setenv(name, val, 1);
}

/* ----------------------------------------------------------- FIFO reader */

typedef struct
{
  double t;
  uint64_t end; /* frames received up to and including this read */
} ev_t;

typedef struct
{
  int fd;
  pthread_t th;
  volatile int stop;
  int stopped;
  int16_t *s; /* interleaved stereo */
  size_t cap;
  uint64_t n; /* frames received (atomic) */
  ev_t *ev;
  size_t nev, capev;
} reader_t;

static void *reader_main(void *arg)
{
  reader_t *r = arg;
  uint8_t tmp[65536];
  size_t carry = 0;

  while (!r->stop)
  {
    struct pollfd p = {r->fd, POLLIN, 0};
    poll(&p, 1, 1);
    if (!(p.revents & POLLIN))
    {
      if (p.revents & POLLHUP)
        usleep(1000); /* no writer yet */
      continue;
    }
    ssize_t k = read(r->fd, tmp + carry, sizeof(tmp) - carry);
    if (k <= 0)
      continue;
    double t = now_s();
    size_t bytes = carry + (size_t)k;
    size_t fr = bytes / 4;
    if (r->n + fr <= r->cap && r->nev < r->capev)
    {
      memcpy(r->s + r->n * 2, tmp, fr * 4);
      r->ev[r->nev].t = t;
      r->ev[r->nev].end = r->n + fr;
      r->nev++;
      __atomic_store_n(&r->n, r->n + fr, __ATOMIC_RELEASE);
    }
    carry = bytes - fr * 4;
    memmove(tmp, tmp + fr * 4, carry);
  }
  return NULL;
}

static reader_t *reader_start(const char *path)
{
  reader_t *r = calloc(1, sizeof(*r));
  r->cap = OUT_RATE * 8;
  r->s = calloc(r->cap, 4);
  r->capev = 200000;
  r->ev = calloc(r->capev, sizeof(ev_t));
  r->fd = open(path, O_RDONLY | O_NONBLOCK);
  if (r->fd < 0)
  {
    perror("open fifo");
    exit(2);
  }
  pthread_create(&r->th, NULL, reader_main, r);
  return r;
}

static void reader_stop(reader_t *r)
{
  if (r->stopped)
    return;
  r->stopped = 1;
  r->stop = 1;
  pthread_join(r->th, NULL);
  close(r->fd);
}

static void reader_free(reader_t *r)
{
  if (!r)
    return;
  reader_stop(r);
  free(r->s);
  free(r->ev);
  free(r);
}

/* Arrival time of output frame j: the pacer releases what is due, so a read ends at the
   due time of its last frame. */
static double arrival(const reader_t *r, uint64_t j)
{
  size_t lo = 0, hi = r->nev;
  while (lo < hi)
  {
    size_t m = (lo + hi) / 2;
    if (r->ev[m].end > j)
      hi = m;
    else
      lo = m + 1;
  }
  if (lo >= r->nev)
    return -1;
  return r->ev[lo].t - (double)(r->ev[lo].end - 1 - j) / OUT_RATE;
}

/* ------------------------------------------------------------------- PCM */

static int apply_hw(snd_pcm_t *p, unsigned rate, snd_pcm_format_t fmt, snd_pcm_uframes_t period, snd_pcm_uframes_t buffer)
{
  snd_pcm_hw_params_t *hw;
  int dir = 0;

  snd_pcm_hw_params_alloca(&hw);
  if (snd_pcm_hw_params_any(p, hw) < 0 || snd_pcm_hw_params_set_access(p, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0 ||
      snd_pcm_hw_params_set_format(p, hw, fmt) < 0 || snd_pcm_hw_params_set_channels(p, hw, 2) < 0 ||
      snd_pcm_hw_params_set_rate(p, hw, rate, 0) < 0 || snd_pcm_hw_params_set_period_size_near(p, hw, &period, &dir) < 0 ||
      snd_pcm_hw_params_set_buffer_size_near(p, hw, &buffer) < 0 || snd_pcm_hw_params(p, hw) < 0)
    return -1;
  return 0;
}

static snd_pcm_t *open_pcm_ex(const char *name, unsigned rate, snd_pcm_format_t fmt, snd_pcm_uframes_t period,
                              snd_pcm_uframes_t buffer, int twice, int mode)
{
  snd_pcm_t *p;
  snd_pcm_sw_params_t *sw;
  snd_pcm_uframes_t boundary;

  snd_pcm_sw_params_alloca(&sw);
  if (snd_pcm_open(&p, name, SND_PCM_STREAM_PLAYBACK, mode) < 0)
    return NULL;
  if ((twice && apply_hw(p, rate, fmt, period / 2, buffer / 2) < 0) || apply_hw(p, rate, fmt, period, buffer) < 0)
  {
    snd_pcm_close(p);
    return NULL;
  }
  snd_pcm_sw_params_current(p, sw);
  snd_pcm_sw_params_get_boundary(sw, &boundary);
  snd_pcm_sw_params_set_start_threshold(p, sw, boundary); /* we start by hand */
  if (snd_pcm_sw_params(p, sw) < 0)
  {
    snd_pcm_close(p);
    return NULL;
  }
  return p;
}

static snd_pcm_t *open_pcm(unsigned rate)
{
  return open_pcm_ex("lt", rate, SND_PCM_FORMAT_S16_LE, 1024, 2 * (snd_pcm_uframes_t)rate, 0, 0);
}

static int write_frames(snd_pcm_t *p, const void *buf, size_t frames, size_t fb)
{
  const uint8_t *s = buf;
  double give_up = now_s() + 20;
  while (frames)
  {
    snd_pcm_sframes_t w = snd_pcm_writei(p, s, frames);
    if (w == -EAGAIN || w == 0)
    {
      if (now_s() > give_up)
        return -1;
      usleep(1000);
      continue;
    }
    if (w < 0)
    {
      printf("writei: %s\n", snd_strerror((int)w));
      return -1;
    }
    s += (size_t)w * fb;
    frames -= (size_t)w;
  }
  return 0;
}

static int write_all(snd_pcm_t *p, const int16_t *s, size_t frames)
{
  return write_frames(p, s, frames, 4);
}

#define MARKERS 5
static const double marker_t[MARKERS] = {0.30, 0.60, 0.90, 1.20, 1.50};

/* 1 kHz tone, amplitude 5000, with 2 ms bursts of 30000 at the marker times */
static int16_t *make_tone(unsigned rate, double secs, int with_markers)
{
  size_t n = (size_t)(secs * rate);
  int16_t *s = malloc(n * 4);
  for (size_t i = 0; i < n; i++)
  {
    int16_t v = (int16_t)(5000.0 * sin(2 * M_PI * 1000.0 * (double)i / rate));
    s[2 * i] = s[2 * i + 1] = v;
  }
  if (with_markers)
    for (int m = 0; m < MARKERS; m++)
    {
      size_t a = (size_t)(marker_t[m] * rate), len = rate / 500;
      for (size_t i = a; i < a + len && i < n; i++)
        s[2 * i] = s[2 * i + 1] = 30000;
    }
  return s;
}

/* The same signal in the sample format under test; *fb receives the frame size. */
static void *to_format(const int16_t *s, size_t n, snd_pcm_format_t fmt, size_t *fb)
{
  *fb = fmt == SND_PCM_FORMAT_S16_LE ? 4 : 8;
  void *out = malloc(n * *fb);
  for (size_t i = 0; i < 2 * n; i++)
  {
    if (fmt == SND_PCM_FORMAT_S32_LE)
      ((int32_t *)out)[i] = (int32_t)s[i] * 65536;
    else if (fmt == SND_PCM_FORMAT_FLOAT_LE)
      ((float *)out)[i] = (float)s[i] / 32768.0f;
    else
      ((int16_t *)out)[i] = s[i];
  }
  return out;
}

/* ---------------------------------------------------------------- tests */

typedef struct
{
  const char *name;
  const char *pcm;             /* "lt" (one pipe), "lt2" (two pipes) */
  unsigned rate;
  snd_pcm_format_t fmt;
  double prefill_s;
  snd_pcm_uframes_t buffer;    /* 0: 2 s */
  int paced;                   /* write like a player: stay ~0.3 s ahead */
  int pause;                   /* 1: pause after all is written, 2: write while paused */
  long hidden, gran;           /* fake slave options */
  int twice;                   /* hw_params twice */
  int markers;                 /* marked tone; 0: plain tone, check continuity */
  int pipe2;                   /* 1: reader on the second pipe too, 2: second pipe unread */
  int detach;                  /* reader leaves at 0.6 s and comes back at 0.9 s */
  const char *derr;            /* slave delay fails from:to ms after start */
} tone_t;

/* Markers in the copy: 30000 on the left channel after tone (<= 5000) */
static void check_markers(const tone_t *c, const reader_t *rd, const char *label, double t_start, double pause_real,
                          double pause_at)
{
  int found = 0, ok = 1;
  double lo = 1e9, hi = -1e9;
  uint64_t last = 0;
  for (uint64_t j = 0; j < rd->n && found < MARKERS; j++)
  {
    if (rd->s[2 * j] > 20000 && (found == 0 || j > last + 2400))
    {
      double expect = t_start + marker_t[found] + (c->pause && marker_t[found] - LEAD_S > pause_at ? pause_real : 0) -
                      LEAD_S + (double)c->hidden / c->rate;
      double dev = (arrival(rd, j) - expect) * 1e3;
      if (dev < lo)
        lo = dev;
      if (dev > hi)
        hi = dev;
      if (dev < DEV_MIN_MS || dev > DEV_MAX_MS)
        ok = 0;
      last = j;
      found++;
    }
  }
  check(found == MARKERS && ok, "%s: %scopy leads play time by %.0f ms (%d of %d markers, deviation %.1f..%.1f ms, limit %.0f..%.0f ms)",
        c->name, label, LEAD_S * 1e3, found, MARKERS, lo, hi, DEV_MIN_MS, DEV_MAX_MS);
}

/* A pure 48 kHz tone repeats every 48 frames; a gap or a skipped piece breaks that. */
static uint64_t discontinuities(const reader_t *r)
{
  uint64_t bad = 0;
  for (uint64_t j = 48; j < r->n; j++)
    if (abs(r->s[2 * j] - r->s[2 * (j - 48)]) > 16)
      bad++;
  return bad;
}

static void run_tone(const tone_t *c)
{
  const double secs = 1.8, pause_len = 0.4, pause_at = c->pause == 2 ? 0.05 : 0.25;
  const unsigned rate = c->rate;
  size_t total = (size_t)(secs * rate), done = 0, pre = (size_t)(c->prefill_s * rate), fb;
  int16_t *sig = make_tone(rate, secs, c->markers);
  void *buf = to_format(sig, total, c->fmt, &fb);
  double max_delay_err = 0, t_start, pause_real = 0;
  snd_pcm_sframes_t dl;
  reader_t *rd2 = NULL;

  begin(c->name);
  if (c->hidden)
    setenv_l("FAKE_HIDDEN_FRAMES", c->hidden);
  if (c->gran)
    setenv_l("FAKE_GRAN_FRAMES", c->gran);
  if (c->derr)
    setenv("FAKE_DELAY_ERR_MS", c->derr, 1);
  reader_t *rd = reader_start(fifo_path);
  if (c->pipe2 == 1)
    rd2 = reader_start(fifo2_path);
  snd_pcm_t *p = open_pcm_ex(c->pcm, rate, c->fmt, 1024, c->buffer ? c->buffer : 2 * (snd_pcm_uframes_t)rate, c->twice, 0);

  if (!p)
  {
    check(0, "%s: open leadtap PCM", c->name);
    goto out;
  }
  if (write_frames(p, buf, pre, fb) < 0)
    fails++;
  done = pre;

  /* before start nothing plays, delay == queued frames exactly */
  int e = snd_pcm_delay(p, &dl);
  check(e == 0 && (size_t)dl == pre, "%s: delay before start equals queued frames (%ld of %zu)", c->name, (long)dl, pre);

  t_start = now_s();
  if (snd_pcm_start(p) < 0)
    check(0, "%s: start", c->name);
  int detach_state = 0;
  size_t first_end = c->pause == 2 ? pre : total;
  while (done < first_end)
  {
    size_t chunk = first_end - done < 1024 ? first_end - done : 1024;
    double el = now_s() - t_start;
    if (c->paced && (double)done - el * rate > 0.3 * rate)
    {
      usleep(2000);
    }
    else
    {
      if (write_frames(p, (const uint8_t *)buf + done * fb, chunk, fb) < 0)
        break;
      done += chunk;
      if (!c->pause && !c->gran)
      {
        double exp_delay = (double)done - (now_s() - t_start) * rate + (double)c->hidden;
        if (snd_pcm_delay(p, &dl) == 0)
        {
          double err = fabs((double)dl - exp_delay) / rate;
          if (err > max_delay_err)
            max_delay_err = err;
        }
      }
    }
    if (c->detach)
    {
      if (detach_state == 0 && el > 0.6)
      {
        reader_stop(rd);
        detach_state = 1;
      }
      if (detach_state == 1 && el > 0.9)
      {
        rd2 = reader_start(fifo_path);
        detach_state = 2;
      }
    }
  }
  if (c->pause == 1)
  {
    sleep_until(t_start + pause_at);
    double a = now_s();
    if (snd_pcm_pause(p, 1) < 0)
      check(0, "%s: pause", c->name);
    usleep((useconds_t)(pause_len * 1e6));
    if (snd_pcm_pause(p, 0) < 0)
      check(0, "%s: resume", c->name);
    pause_real = now_s() - a;
  }
  else if (c->pause == 2)
  {
    /* the player keeps writing while paused: those frames play only after the resume */
    sleep_until(t_start + pause_at);
    double a = now_s();
    if (snd_pcm_pause(p, 1) < 0)
      check(0, "%s: pause", c->name);
    usleep(200000);
    write_frames(p, (const uint8_t *)buf + done * fb, total - done, fb);
    sleep_until(a + pause_len);
    if (snd_pcm_pause(p, 0) < 0)
      check(0, "%s: resume", c->name);
    pause_real = now_s() - a;
  }
  else if (!c->gran)
    check(max_delay_err <= DELAY_TOL_S, "%s: running delay follows the slave (max deviation %.2f ms, limit %.0f ms)", c->name,
          max_delay_err * 1e3, DELAY_TOL_S * 1e3);

  sleep_until(t_start + secs + (c->pause ? pause_real : 0) + 0.15 + (double)c->hidden / rate);
  if (c->pipe2 == 2)
  {
    /* nobody reads the second pipe: it holds a bounded backlog (while the PCM is open) and
       the first pipe is not affected */
    int fd = open(fifo2_path, O_RDONLY | O_NONBLOCK), waiting = -1;
    if (fd >= 0)
    {
      ioctl(fd, FIONREAD, &waiting);
      close(fd);
    }
    check(waiting > 0 && waiting <= 4800, "%s: unread second pipe holds a hard-bounded backlog (limit 4800) (%d bytes)", c->name, waiting);
  }
  snd_pcm_drop(p);
  snd_pcm_close(p);
  usleep(50000);
  reader_stop(rd);
  if (rd2)
    reader_stop(rd2);

  if (c->detach)
  {
    uint64_t n2 = rd2 ? rd2->n : 0;
    check(n2 > 0.55 * OUT_RATE && n2 < 0.85 * OUT_RATE,
          "%s: copy resumes for a reader that comes back (%llu frames in the last 0.72 s of copy, expected ~%d)", c->name,
          (unsigned long long)n2, (int)(0.745 * OUT_RATE));
  }
  else if (c->markers)
    check_markers(c, rd, "", t_start, pause_real, pause_at);
  else
  {
    uint64_t bad = discontinuities(rd);
    check(rd->n > 0.8 * OUT_RATE && bad == 0, "%s: copy of a tone is continuous (%llu frames, %llu discontinuities)", c->name,
          (unsigned long long)rd->n, (unsigned long long)bad);
  }
  if (c->pipe2 == 1)
  {
    check_markers(c, rd2, "second pipe: ", t_start, pause_real, pause_at);
    check(rd->n == rd2->n && memcmp(rd->s, rd2->s, rd->n * 4) == 0, "%s: both pipes carry the same data (%llu and %llu frames)",
          c->name, (unsigned long long)rd->n, (unsigned long long)rd2->n);
  }
out:
  unsetenv("FAKE_HIDDEN_FRAMES");
  unsetenv("FAKE_GRAN_FRAMES");
  unsetenv("FAKE_DELAY_ERR_MS");
  reader_free(rd);
  reader_free(rd2);
  free(sig);
  free(buf);
  alarm(0);
}

/* IEC 61937 bursts -> silent copy */
static void run_iec(void)
{
  const unsigned rate = 48000;
  size_t total = rate; /* 1 s */
  int16_t *s = malloc(total * 4);
  begin("iec");
  for (size_t i = 0; i < total; i++)
  {
    s[2 * i] = (int16_t)((i * 7) % 20000 + 1000);
    s[2 * i + 1] = (int16_t)(-(int)((i * 11) % 20000) - 1000);
    if (i % 1536 == 0)
    {
      s[2 * i] = (int16_t)0xF872;
      s[2 * i + 1] = (int16_t)0x4E1F;
    }
  }
  reader_t *rd = reader_start(fifo_path);
  snd_pcm_t *p = open_pcm(rate);
  if (!p)
  {
    check(0, "iec: open leadtap PCM");
    reader_free(rd);
    free(s);
    return;
  }
  write_all(p, s, total);
  double t0 = now_s();
  snd_pcm_start(p);
  int err = snd_pcm_drain(p);
  double dt = now_s() - t0;
  snd_pcm_avail(p);
  snd_pcm_state_t st = snd_pcm_state(p);
  usleep(100000);
  snd_pcm_close(p);
  reader_stop(rd);
  uint64_t nonzero = 0;
  for (uint64_t j = 0; j < rd->n * 2; j++)
    if (rd->s[j])
      nonzero++;
  check(err == 0 && dt > 0.85 && dt < 1.3, "iec: drain returns after the audio has played (%.2f s, err %d)", dt, err);
  check(st == SND_PCM_STATE_SETUP, "iec: state after drain and a position query is SETUP, not XRUN (%s)", snd_pcm_state_name(st));
  check(rd->n > 30000 && nonzero == 0, "iec: copy is silence (%llu frames received, %llu non-zero samples)",
        (unsigned long long)rd->n, (unsigned long long)nonzero);
  reader_free(rd);
  free(s);
  alarm(0);
}

/* drop empties the queue; a new stream after prepare works */
static void run_drop(void)
{
  const unsigned rate = 48000;
  int16_t *sig = make_tone(rate, 1.0, 0);
  begin("drop");
  reader_t *rd = reader_start(fifo_path);
  snd_pcm_t *p = open_pcm(rate);
  if (!p)
  {
    check(0, "drop: open leadtap PCM");
    reader_free(rd);
    free(sig);
    return;
  }
  write_all(p, sig, (size_t)(0.9 * rate));
  double t_start = now_s();
  snd_pcm_start(p);
  sleep_until(t_start + 0.30);
  double t_drop = now_s();
  snd_pcm_drop(p);
  snd_pcm_avail(p); /* alsa-lib asks the plugin for the position even in SETUP */
  check(snd_pcm_state(p) == SND_PCM_STATE_SETUP, "drop: state after drop and a position query is SETUP, not XRUN (%s)",
        snd_pcm_state_name(snd_pcm_state(p)));
  usleep(25000);
  uint64_t at_drop = __atomic_load_n(&rd->n, __ATOMIC_ACQUIRE);
  usleep(500000);
  uint64_t after = __atomic_load_n(&rd->n, __ATOMIC_ACQUIRE);
  /* frames overdue by more than 20 ms at the start are dropped by design, so the first
     copy frame is due 0.02 s after start: expected = t_drop - t_start + 0.02 */
  double expect = (t_drop - t_start + 0.02) * OUT_RATE;
  check(fabs((double)at_drop - expect) < 0.03 * OUT_RATE, "drop: copy released up to the drop only (%llu frames, expected %.0f +-%d)",
        (unsigned long long)at_drop, expect, (int)(0.03 * OUT_RATE));
  check(after == at_drop, "drop: nothing arrives after the drop (%llu -> %llu frames in 500 ms)", (unsigned long long)at_drop,
        (unsigned long long)after);

  /* second stream on the same PCM */
  uint64_t before2 = after;
  snd_pcm_prepare(p);
  write_all(p, sig, (size_t)(0.4 * rate));
  double t2 = now_s();
  snd_pcm_start(p);
  sleep_until(t2 + 0.4 + 0.15);
  uint64_t got2 = __atomic_load_n(&rd->n, __ATOMIC_ACQUIRE) - before2;
  double expect2 = (0.4 - (LEAD_S - 0.02)) * OUT_RATE; /* the first 160 ms of play time are overdue */
  check(fabs((double)got2 - expect2) < 0.03 * OUT_RATE, "drop: new stream after prepare is copied again (%llu frames, expected %.0f)",
        (unsigned long long)got2, expect2);
  snd_pcm_drop(p);
  snd_pcm_close(p);
  reader_free(rd);
  free(sig);
  alarm(0);
}

/* The slave runs dry: xrun. The application recovers with prepare and the copy goes on. */
static void run_xrun(void)
{
  const unsigned rate = 48000;
  int16_t *sig = make_tone(rate, 1.0, 0);
  begin("xrun");
  reader_t *rd = reader_start(fifo_path);
  snd_pcm_t *p = open_pcm(rate);
  if (!p)
  {
    check(0, "xrun: open leadtap PCM");
    reader_free(rd);
    free(sig);
    return;
  }
  write_all(p, sig, 4800);
  snd_pcm_start(p);
  usleep(400000); /* 0.1 s of audio played out: underrun */
  snd_pcm_sframes_t e = snd_pcm_writei(p, sig, 480);
  check(e == -EPIPE, "xrun: write after the slave ran dry reports -EPIPE (%ld)", (long)e);
  check(snd_pcm_prepare(p) == 0, "xrun: prepare recovers");
  uint64_t before = __atomic_load_n(&rd->n, __ATOMIC_ACQUIRE);
  write_all(p, sig, (size_t)(0.4 * rate));
  double t2 = now_s();
  snd_pcm_start(p);
  sleep_until(t2 + 0.4 + 0.15);
  uint64_t got = __atomic_load_n(&rd->n, __ATOMIC_ACQUIRE) - before;
  double expect = (0.4 - (LEAD_S - 0.02)) * OUT_RATE;
  check(fabs((double)got - expect) < 0.03 * OUT_RATE, "xrun: the copy continues after recovery (%llu frames, expected %.0f)",
        (unsigned long long)got, expect);
  snd_pcm_drop(p);
  snd_pcm_close(p);
  reader_free(rd);
  free(sig);
  alarm(0);
}

/* Closing a running stream must neither hang nor leave the next one broken. */
static void run_close_running(void)
{
  const unsigned rate = 48000;
  int16_t *sig = make_tone(rate, 1.0, 0);
  begin("close while running");
  reader_t *rd = reader_start(fifo_path);
  snd_pcm_t *p = open_pcm(rate);
  if (!p)
  {
    check(0, "close while running: open leadtap PCM");
    reader_free(rd);
    free(sig);
    return;
  }
  write_all(p, sig, (size_t)(0.5 * rate));
  snd_pcm_start(p);
  usleep(250000);
  snd_pcm_close(p);
  check(1, "close while running: returns");
  usleep(50000);
  uint64_t before = __atomic_load_n(&rd->n, __ATOMIC_ACQUIRE);
  p = open_pcm(rate);
  int ok = p != NULL;
  if (p)
  {
    write_all(p, sig, (size_t)(0.5 * rate));
    double t0 = now_s();
    snd_pcm_start(p);
    sleep_until(t0 + 0.5);
    uint64_t got = __atomic_load_n(&rd->n, __ATOMIC_ACQUIRE) - before;
    ok = got > 0.1 * OUT_RATE;
    snd_pcm_drop(p);
    snd_pcm_close(p);
  }
  check(ok, "close while running: the PCM can be opened and copied again");
  reader_free(rd);
  free(sig);
  alarm(0);
}

/* Non-blocking open: drain must not block, the slave's -EAGAIN is handed on. */
static void run_nonblock_drain(void)
{
  const unsigned rate = 48000;
  int16_t *sig = make_tone(rate, 1.0, 0);
  begin("non-blocking drain");
  reader_t *rd = reader_start(fifo_path);
  snd_pcm_t *p = open_pcm_ex("lt", rate, SND_PCM_FORMAT_S16_LE, 1024, 2 * (snd_pcm_uframes_t)rate, 0, SND_PCM_NONBLOCK);
  if (!p)
  {
    check(0, "non-blocking drain: open leadtap PCM");
    reader_free(rd);
    free(sig);
    return;
  }
  write_all(p, sig, (size_t)(0.5 * rate));
  snd_pcm_start(p);
  double t0 = now_s();
  int e = snd_pcm_drain(p);
  double first = now_s() - t0;
  check(e == -EAGAIN && first < 0.1, "non-blocking drain: first call returns -EAGAIN at once (%d after %.0f ms)", e, first * 1e3);
  while (e == -EAGAIN && now_s() - t0 < 3)
  {
    usleep(5000);
    e = snd_pcm_drain(p);
  }
  double dt = now_s() - t0;
  check(e == 0 && dt > 0.4 && dt < 0.8, "non-blocking drain: finishes when the audio has played (%.2f s, err %d)", dt, e);
  snd_pcm_close(p);
  reader_free(rd);
  free(sig);
  alarm(0);
}

/* A driver can delay audio beyond its ALSA buffer (own DMA ring). The application must still
   see the buffer fill level as the position and the larger figure only as delay; otherwise
   it keeps that much less audio queued and the copy runs out of lead. */
static void run_hidden_delay(void)
{
  enum { HIDDEN = 3072 };
  begin("hidden delay");
  setenv_l("FAKE_HIDDEN_FRAMES", HIDDEN);

  snd_pcm_t *p = open_pcm(48000);
  if (!p)
  {
    check(0, "hidden delay: open");
    unsetenv("FAKE_HIDDEN_FRAMES");
    return;
  }
  snd_pcm_uframes_t buffer = 0, period = 0;
  snd_pcm_get_params(p, &buffer, &period);
  int16_t *zeros = calloc(buffer, 4);
  int err = write_all(p, zeros, buffer);
  if (snd_pcm_state(p) == SND_PCM_STATE_PREPARED)
    snd_pcm_start(p);
  sleep_until(now_s() + 0.05);
  snd_pcm_sframes_t avail = snd_pcm_avail(p), dl = 0;
  snd_pcm_delay(p, &dl);
  long queued = (long)buffer - (long)avail;
  check(err == 0 && labs(dl - queued - HIDDEN) < 480,
        "hidden delay: delay %ld = queued %ld + hidden %d (avail %ld of %lu)", (long)dl, queued, HIDDEN, (long)avail,
        (unsigned long)buffer);
  check(labs((long)avail - 2400) < 960, "hidden delay: position follows the buffer, not the delay (avail %ld, expected ~2400)",
        (long)avail);
  snd_pcm_drop(p);
  snd_pcm_close(p);
  free(zeros);
  unsetenv("FAKE_HIDDEN_FRAMES");
  alarm(0);
}

/* A slave whose buffer maximum is far below what the application asks for (dmix, HDMI, USB).
   The application has to be held inside it; otherwise it queues more than the slave takes
   and waits for space that never comes (no hang is the check, the watchdog reports it). */
static void run_small_buffer(void)
{
  const unsigned rate = 48000;
  int16_t *sig = make_tone(rate, 1.0, 0);
  begin("small slave buffer");
  setenv_l("FAKE_MAX_BUFFER_BYTES", 16384); /* 4096 frames of S16 stereo */
  reader_t *rd = reader_start(fifo_path);
  snd_pcm_t *p = open_pcm_ex("lt40", rate, SND_PCM_FORMAT_S16_LE, 1024, 2 * (snd_pcm_uframes_t)rate, 0, 0);
  if (!p)
  {
    check(0, "small slave buffer: open leadtap PCM");
    goto out;
  }
  snd_pcm_uframes_t buffer = 0, period = 0;
  snd_pcm_get_params(p, &buffer, &period);
  check(buffer > 0 && buffer <= 4096, "small slave buffer: application buffer stays inside the slave's maximum (%lu of 4096 frames)",
        (unsigned long)buffer);
  size_t pre = buffer < 4800 ? buffer : 4800, done = pre;
  write_all(p, sig, pre);
  snd_pcm_start(p);
  double t0 = now_s();
  int err = 0;
  while (done < rate && !err)
  {
    size_t chunk = rate - done < 1024 ? rate - done : 1024;
    err = write_all(p, sig + done * 2, chunk);
    done += chunk;
  }
  sleep_until(t0 + 1.0 + 0.1);
  snd_pcm_drop(p);
  snd_pcm_close(p);
  usleep(50000);
  reader_stop(rd);
  check(err == 0 && done >= rate && rd->n > 0.6 * OUT_RATE, "small slave buffer: a second of audio plays and is copied (%llu frames, lead 40 ms)",
        (unsigned long long)rd->n);
out:
  unsetenv("FAKE_MAX_BUFFER_BYTES");
  reader_free(rd);
  free(sig);
  alarm(0);
}

static double cpu_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* The slave reports space but refuses the write (-EAGAIN). A non-blocking
   caller must get -EAGAIN back at once; a blocking caller must wait without burning a CPU. */
static void run_refuse(void)
{
  const unsigned rate = 48000;
  int16_t *sig = make_tone(rate, 0.5, 0);

  const char *title = "slave refuses writes";
  begin(title);
  setenv_l("FAKE_REFUSE_MS", 300);
  reader_t *rd = reader_start(fifo_path);
  snd_pcm_t *p = open_pcm_ex("lt", rate, SND_PCM_FORMAT_S16_LE, 1024, 2 * (snd_pcm_uframes_t)rate, 0, SND_PCM_NONBLOCK);
  if (!p)
  {
    check(0, "%s: open (non-blocking)", title);
    goto out;
  }
  double t0 = now_s();
  snd_pcm_sframes_t w = snd_pcm_writei(p, sig, 1024);
  double dt = now_s() - t0;
  check(w == -EAGAIN && dt < 0.05, "%s: non-blocking write returns -EAGAIN at once (%ld after %.0f ms)", title, (long)w, dt * 1e3);
  while (w < 0 && now_s() - t0 < 3)
  {
    usleep(2000);
    w = snd_pcm_writei(p, sig, 1024);
  }
  check(w == 1024, "%s: the write goes through when the slave takes it again (%ld)", title, (long)w);
  snd_pcm_drop(p);
  snd_pcm_close(p);

  p = open_pcm_ex("lt", rate, SND_PCM_FORMAT_S16_LE, 1024, 2 * (snd_pcm_uframes_t)rate, 0, 0);
  if (!p)
  {
    check(0, "%s: open (blocking)", title);
    goto out;
  }
  double c0 = cpu_s();
  t0 = now_s();
  w = snd_pcm_writei(p, sig, 1024);
  dt = now_s() - t0;
  double cpu = cpu_s() - c0;
  check(w == 1024 && dt > 0.1 && cpu < 0.5 * dt, "%s: blocking write waits (%ld after %.0f ms, %.0f ms CPU)", title, (long)w, dt * 1e3,
        cpu * 1e3);
  snd_pcm_drop(p);
  snd_pcm_close(p);
out:
  unsetenv("FAKE_REFUSE_MS");
  reader_free(rd);
  free(sig);
  alarm(0);
}

/* alsa-lib's generic ioplug moves only the application pointer on snd_pcm_rewind/forward.
   The plugin undoes that; the free space the application sees must stay exact. */
static void run_rewind_forward(void)
{
  const unsigned rate = 48000;
  int16_t *sig = make_tone(rate, 1.0, 0);
  snd_pcm_uframes_t buffer = 0, period = 0;

  begin("rewind/forward");
  reader_t *rd = reader_start(fifo_path);
  snd_pcm_t *p = open_pcm(rate);
  if (!p)
  {
    check(0, "rewind/forward: open leadtap PCM");
    goto out;
  }
  snd_pcm_get_params(p, &buffer, &period);
  write_all(p, sig, 9600);
  snd_pcm_sframes_t rw = snd_pcm_rewind(p, 2400);
  check(rw >= 0, "rewind/forward: snd_pcm_rewind is accepted (%ld)", (long)rw);
  write_all(p, sig, 4800);
  snd_pcm_sframes_t av = snd_pcm_avail(p), dl = 0;
  snd_pcm_delay(p, &dl);
  check(av == (snd_pcm_sframes_t)buffer - 14400 && dl == 14400,
        "rewind/forward: after a rewind the free space and the delay still count the frames the slave holds (avail %ld expected %ld, delay %ld expected 14400)",
        (long)av, (long)buffer - 14400, (long)dl);
  snd_pcm_sframes_t fw = snd_pcm_forward(p, 1200);
  check(fw >= 0, "rewind/forward: snd_pcm_forward is accepted (%ld)", (long)fw);
  write_all(p, sig, 2400);
  av = snd_pcm_avail(p);
  snd_pcm_delay(p, &dl);
  check(av == (snd_pcm_sframes_t)buffer - 16800 && dl == 16800,
        "rewind/forward: same after a forward (avail %ld expected %ld, delay %ld expected 16800)", (long)av,
        (long)buffer - 16800, (long)dl);
  snd_pcm_drop(p);
  snd_pcm_close(p);
out:
  reader_free(rd);
  free(sig);
  alarm(0);
}

static int make_fifo(const char *path)
{
  if (mkfifo(path, 0600) != 0)
  {
    perror("mkfifo");
    return -1;
  }
  return 0;
}

int main(int argc, char **argv)
{
  char dir[] = "/tmp/leadtap-test-XXXXXX";
  char conf[600];

  if (argc != 3)
  {
    fprintf(stderr, "usage: %s leadtap.so fakeslave.so\n", argv[0]);
    return 2;
  }
  if (!mkdtemp(dir))
  {
    perror("mkdtemp");
    return 2;
  }
  snprintf(fifo_path, sizeof(fifo_path), "%s/fifo", dir);
  snprintf(fifo2_path, sizeof(fifo2_path), "%s/fifo2", dir);
  snprintf(conf, sizeof(conf), "%s/asound.conf", dir);
  if (make_fifo(fifo_path) || make_fifo(fifo2_path))
    return 2;
  FILE *f = fopen(conf, "w");
  if (!f)
  {
    perror("fopen");
    return 2;
  }
  fprintf(f,
          "pcm_type.leadtap { lib \"%s\" }\n"
          "pcm_type.fakeslave { lib \"%s\" }\n"
          "pcm.fake { type fakeslave }\n"
          "pcm.lt { type leadtap slave.pcm \"fake\" lead 180 fifo \"%s\" }\n"
          "pcm.lt2 { type leadtap slave.pcm \"fake\" lead 180 fifo [ \"%s\" \"%s\" ] }\n"
          "pcm.lt40 { type leadtap slave.pcm \"fake\" lead 40 fifo \"%s\" }\n",
          argv[1], argv[2], fifo_path, fifo_path, fifo2_path, fifo_path);
  fclose(f);
  setenv("ALSA_CONFIG_PATH", conf, 1);
  if (getenv("LEADTAP_VERBOSE") == NULL)
    setenv("LEADTAP_VERBOSE", "1", 1);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGALRM, on_alarm);

  /* gate: the plugin must open at all */
  snd_pcm_t *probe = open_pcm(48000);
  if (!probe)
  {
    printf("FAIL cannot open the leadtap PCM (config %s)\n", conf);
    return 1;
  }
  snd_pcm_close(probe);

  const snd_pcm_format_t S16 = SND_PCM_FORMAT_S16_LE;
  const tone_t tones[] = {
      {.name = "48k prefill 0.1 s", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.1, .markers = 1},
      {.name = "48k prefill 0.8 s", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.8, .markers = 1},
      {.name = "44.1k prefill 0.1 s", .pcm = "lt", .rate = 44100, .fmt = S16, .prefill_s = 0.1, .markers = 1},
      {.name = "44.1k prefill 0.8 s", .pcm = "lt", .rate = 44100, .fmt = S16, .prefill_s = 0.8, .markers = 1},
      {.name = "44.1k paced writes", .pcm = "lt", .rate = 44100, .fmt = S16, .prefill_s = 0.3, .paced = 1, .markers = 1},
      {.name = "48k S32 input", .pcm = "lt", .rate = 48000, .fmt = SND_PCM_FORMAT_S32_LE, .prefill_s = 0.1, .markers = 1},
      {.name = "48k FLOAT input", .pcm = "lt", .rate = 48000, .fmt = SND_PCM_FORMAT_FLOAT_LE, .prefill_s = 0.1, .markers = 1},
      {.name = "48k buffer 0.5 s (position wraps)", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.3, .buffer = 24000,
       .paced = 1, .markers = 1},
      {.name = "48k hw_params twice", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.1, .twice = 1, .markers = 1},
      {.name = "48k with pause", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.1, .pause = 1, .markers = 1},
      {.name = "48k writes during pause", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.4, .pause = 2, .markers = 1},
      {.name = "48k hidden delay 100 ms, prefill 0.8 s", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.8,
       .hidden = 4800, .markers = 1},
      {.name = "48k hidden delay 100 ms, paced", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.3, .paced = 1,
       .hidden = 4800, .markers = 1},
      {.name = "48k slave position in 20 ms steps", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.3, .paced = 1,
       .gran = 960},
      {.name = "48k two pipes", .pcm = "lt2", .rate = 48000, .fmt = S16, .prefill_s = 0.1, .pipe2 = 1, .markers = 1},
      {.name = "48k two pipes, second unread", .pcm = "lt2", .rate = 48000, .fmt = S16, .prefill_s = 0.1, .pipe2 = 2,
       .markers = 1},
      {.name = "48k reader leaves and returns", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.3, .paced = 1,
       .detach = 1, .markers = 1},
      {.name = "48k slave delay fails for 100 ms", .pcm = "lt", .rate = 48000, .fmt = S16, .prefill_s = 0.3, .paced = 1,
       .derr = "400:500", .markers = 1},
  };
  for (unsigned i = 0; i < sizeof(tones) / sizeof(tones[0]); i++)
    run_tone(&tones[i]);
  run_iec();
  run_drop();
  run_xrun();
  run_close_running();
  run_nonblock_drain();
  run_hidden_delay();
  run_small_buffer();
  run_refuse();
  run_rewind_forward();

  unlink(fifo_path);
  unlink(fifo2_path);
  unlink(conf);
  rmdir(dir);
  if (fails)
  {
    printf("FAILED: %d check(s) failed, %d passed\n", fails, passes);
    return 1;
  }
  printf("ALL OK (%d checks)\n", passes);
  return 0;
}
