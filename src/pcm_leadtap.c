// SPDX-License-Identifier: LGPL-2.1-or-later
/*
  alsa-leadtap: ALSA external PCM plugin (ioplug) that passes audio to a slave PCM
  unchanged and, in addition, hands out a copy of it a fixed time BEFORE it becomes
  audible on the slave.

  asound.conf:
    pcm_type.leadtap {
      lib "/path/to/libasound_module_pcm_leadtap.so"
    }
    pcm.mydev {
      type leadtap
      slave.pcm "hw:0"           # any PCM name (string)
      lead 180                   # milliseconds, 0..2000 (default 180)
      fifo "/tmp/leadtap.fifo"   # named pipe, created by the reader
      # fifo [ "/tmp/a.fifo" "/tmp/b.fifo" ]   # or several (up to 4), same data to each
    }

  Application side: the slave's hardware parameters, its delay, pause and drain.
  Copy side: 48 kHz, S16_LE, stereo (first two input channels), linearly resampled
  from the stream rate, written to the pipes by a pacer thread. The play time of
  frames written at time T is T + snd_pcm_delay(slave) taken just before the write;
  it is measured again at every write, so the copy follows the slave clock.

  Copy rules:
   - IEC 61937 bursts (Pa/Pb = 0xF872/0x4E1F in S16_LE stereo) switch the copy to
     silence while the bitstream flows (and for a short hold time after the last burst).
   - The copy never blocks: no reader -> dropped; a pipe never holds more than ~25 ms (hard bound).
     Every pipe is handled on its own, one without a reader or with a stuck one does
     not affect the others.
   - Frames that are already more than 20 ms past their release time (a stream
     start with less queued audio than "lead", or a stalled application) are
     dropped, not sent late, so the copy never drifts out of sync.
   - drop / prepare / underrun recovery discard the queued copy, pause holds it. A batch the
     pacer has already taken out of the queue is checked again right before it is written
     (flush generation, pause, deadline), so it cannot reach a pipe after such a transition.
   - snd_pcm_rewind / snd_pcm_forward: alsa-lib moves only the application pointer for an
     ioplug, audio already given to the slave cannot be taken back. The call is accepted but
     has no effect on the audio; the plugin keeps the pointer consistent (see fix_appl).
   - A failed slave delay query: the frames of that write are not copied (their play time is
     unknown); the next write re-anchors and the hole is filled with silence.
*/

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#define OUT_RATE        48000
#define MAX_PIPES       4
#define UNSET           INT64_MIN
#define TOL_MIN_NS      5000000LL           /* below: the copy stays continuous */
#define LATE_MAX_NS     20000000LL          /* overdue by more: drop */
#define BATCH_NS        10000000LL          /* pacer wakes at most this often */
#define BACKLOG_MAX     (OUT_RATE * 25 / 1000 * 4) /* bytes waiting in a pipe */
#define IEC_HOLD_FRAMES 8192                /* ~170 ms: AC3 repeats every 1536 frames */
#define IEC_PA          0xF872
#define IEC_PB          0x4E1F
#define RELBUF          2048
#define DEFAULT_LEAD_MS 180
#define MAX_LEAD_MS     2000
#define PIPE_CLOSED     (-1)
#define PIPE_REJECTED   (-2)                /* not a named pipe: never retried */

enum { K_S16, K_S32, K_F32 };

typedef struct
{
  char *path;
  int fd;
  int64_t next_open_ns;
  uint64_t out, dropped; /* frames, pacer thread only */
} pipe_t;

typedef struct leadtap
{
  snd_pcm_ioplug_t io;
  snd_pcm_ioplug_callback_t cb;
  snd_pcm_t *slave;
  unsigned lead_ms;
  int verbose;

  /* application side (set in hw_params) */
  size_t frame_bytes;
  uint64_t written;               /* frames the slave accepted since prepare */
  snd_pcm_uframes_t slave_buffer; /* the slave's buffer size in frames */
  snd_pcm_sframes_t last_ptr;     /* last position reported, kept for a slave in SETUP */
  int running;

  /* converter state (transfer thread only) */
  int32_t phase;
  int32_t prev[2];
  unsigned iec_hold;
  int16_t *scratch;
  size_t scratch_frames;

  /* One timeline: copy frame j (j counts from the last flush) is due to be audible at
     base + j / 48000. Frames [rd, wr) are in the ring. Guarded by mu. */
  pthread_mutex_t mu;
  pthread_cond_t cv;
  pthread_t thread;
  int thread_started;
  int quit;
  int paused;
  int64_t pause_ns;
  int16_t *ring;
  size_t ring_n;
  uint64_t rd, wr;
  int64_t base;   /* UNSET until the stream has started */
  int64_t tol_ns; /* re-anchor threshold: a slave pointer may move in period steps */
  uint64_t st_dropped, st_late, st_gaps, st_overlaps;
  /* Bumped (under mu) by every flush: a batch the pacer took out of the ring before a flush
     must not reach a pipe afterwards. */
  uint64_t gen;
  uint64_t st_delay_err, st_appl_fix; /* transfer thread only */

  pipe_t pipe[MAX_PIPES];
  unsigned npipes;
} leadtap_t;

static int64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t frames_ns(uint64_t frames)
{
  /* whole seconds apart: the product would overflow 64 bits after days of audio */
  return (int64_t)(frames / OUT_RATE * 1000000000ULL + frames % OUT_RATE * 1000000000ULL / OUT_RATE);
}

static uint64_t ns_frames(int64_t ns)
{
  if (ns <= 0)
    return 0;
  return (uint64_t)ns / 1000000000ULL * OUT_RATE + (uint64_t)ns % 1000000000ULL * OUT_RATE / 1000000000ULL;
}

/* ---------------------------------------------------------------- queue */

static void flush_locked(leadtap_t *lt)
{
  lt->rd = lt->wr = 0;
  lt->base = UNSET;
  lt->gen++;
}

static void q_flush(leadtap_t *lt)
{
  pthread_mutex_lock(&lt->mu);
  flush_locked(lt);
  pthread_mutex_unlock(&lt->mu);
}

/* nf == 0 frames of silence when fr is NULL. The caller has checked the space. */
static void ring_put(leadtap_t *lt, const int16_t *fr, size_t nf)
{
  while (nf)
  {
    size_t idx = (size_t)(lt->wr % lt->ring_n), k = lt->ring_n - idx;
    if (k > nf)
      k = nf;
    if (fr)
    {
      memcpy(lt->ring + idx * 2, fr, k * 4);
      fr += k * 2;
    }
    else
      memset(lt->ring + idx * 2, 0, k * 4);
    lt->wr += k;
    nf -= k;
  }
}

/*
  Append nf frames whose first frame is due to be audible at t_play (UNSET before
  start: lt_start anchors the timeline then). Keeps the copy sample-continuous while
  the measured time agrees with the timeline within tol_ns; otherwise a gap is filled
  with silence and an overlap is skipped in the new frames. This is the re-anchoring:
  the timeline never runs free for more than a few milliseconds. A gap that does not
  fit in the ring restarts the timeline at t_play.
*/
static void q_append(leadtap_t *lt, int64_t t_play, const int16_t *fr, size_t nf)
{
  size_t gap = 0, skip = 0;
  int anchored = 0;

  pthread_mutex_lock(&lt->mu);
  int was_empty = lt->rd >= lt->wr;
  if (t_play != UNSET && lt->base == UNSET)
  {
    lt->base = t_play - frames_ns(lt->wr);
    anchored = 1;
  }
  else if (t_play != UNSET)
  {
    int64_t e = t_play - (lt->base + frames_ns(lt->wr));
    if (e > lt->tol_ns)
    {
      gap = (size_t)ns_frames(e);
      lt->st_gaps++;
    }
    else if (e < -lt->tol_ns)
    {
      skip = (size_t)ns_frames(-e);
      lt->st_overlaps++;
    }
  }
  if (skip >= nf)
    skip = nf;
  lt->st_dropped += skip;
  fr += 2 * skip;
  nf -= skip;

  if (gap + nf > lt->ring_n - (size_t)(lt->wr - lt->rd) && gap)
  {
    flush_locked(lt);
    lt->base = t_play;
    anchored = was_empty = 1;
    gap = 0;
  }
  if (nf > lt->ring_n - (size_t)(lt->wr - lt->rd))
    lt->st_dropped += nf;
  else if (nf)
  {
    ring_put(lt, NULL, gap);
    ring_put(lt, fr, nf);
    if (lt->base != UNSET && (anchored || was_empty))
      pthread_cond_signal(&lt->cv);
  }
  pthread_mutex_unlock(&lt->mu);
}

/* Take the frames that are due (release time = play time - lead). Holds mu. */
static size_t q_release(leadtap_t *lt, int16_t *out, size_t cap, int64_t now)
{
  int64_t el = now + (int64_t)lt->lead_ms * 1000000LL - lt->base;
  if (el < 0)
    return 0;
  uint64_t due = ns_frames(el) + 1;
  if (due > lt->wr)
    due = lt->wr;
  if (due <= lt->rd)
    return 0;
  uint64_t late = el > LATE_MAX_NS ? ns_frames(el - LATE_MAX_NS) + 1 : 0;
  if (late > due)
    late = due;
  if (late > lt->rd)
  {
    lt->st_late += late - lt->rd;
    lt->rd = late;
  }
  size_t got = 0;
  while (lt->rd < due && got < cap)
  {
    size_t idx = (size_t)(lt->rd % lt->ring_n), k = lt->ring_n - idx;
    if (k > due - lt->rd)
      k = (size_t)(due - lt->rd);
    if (k > cap - got)
      k = cap - got;
    memcpy(out + got * 2, lt->ring + idx * 2, k * 4);
    got += k;
    lt->rd += k;
  }
  return got;
}

/* ---------------------------------------------------------------- pipes */

/* Pacer thread only. Never blocks; drops what the reader does not take. */
static void pipe_write(pipe_t *p, const char *buf, size_t bytes, int64_t now)
{
  size_t off = 0;
  int waiting = 0;

  if (p->fd == PIPE_CLOSED && now >= p->next_open_ns)
  {
    /* O_RDWR, not O_WRONLY: snapserver closes and reopens the pipe while it has no writer,
       so a write-only open (ENXIO without a reader) keeps missing it. Holding both ends
       keeps the pipe alive (and a write can never raise SIGPIPE); the backlog check below
       limits what waits for a late reader. */
    struct stat st;
    p->fd = open(p->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (p->fd >= 0 && (fstat(p->fd, &st) < 0 || !S_ISFIFO(st.st_mode)))
    {
      SNDERR("leadtap: %s is not a named pipe", p->path);
      close(p->fd);
      p->fd = PIPE_REJECTED;
    }
    else if (p->fd < 0)
    {
      p->fd = PIPE_CLOSED;
      p->next_open_ns = now + 500000000LL; /* pipe does not exist yet */
    }
  }
  /* let the reader catch up instead of delaying for good */
  if (p->fd < 0)
  {
    p->dropped += bytes / 4;
    return;
  }
  /* hard bound: never leave more than BACKLOG_MAX bytes waiting, also within one batch */
  if (ioctl(p->fd, FIONREAD, &waiting) == 0)
  {
    size_t room = waiting >= BACKLOG_MAX ? 0 : (size_t)(BACKLOG_MAX - waiting) & ~(size_t)3;
    if (room < bytes)
    {
      p->dropped += (bytes - room) / 4;
      bytes = room;
    }
  }
  while (off < bytes)
  {
    /* up to PIPE_BUF a nonblocking write is all or nothing, so the stream stays aligned */
    size_t k = bytes - off < PIPE_BUF ? bytes - off : PIPE_BUF;
    ssize_t w = write(p->fd, buf + off, k);
    if (w > 0)
    {
      off += (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR)
      continue;
    if (w < 0 && errno != EAGAIN)
    {
      close(p->fd);
      p->fd = PIPE_CLOSED;
      p->next_open_ns = now + 500000000LL;
    }
    break;
  }
  p->out += off / 4;
  p->dropped += (bytes - off) / 4;
}

/*
  Hand a batch (taken out of the ring at generation gen, first frame j0) to the pipes. The
  mutex is not held across a write, so everything that can have changed since the batch
  was taken is checked right before the first write:
   - a flush (drop, prepare, stop, drain, close) happened: the batch is stale, discard it;
   - a pause began: the batch is put back (nothing was written yet) and goes out after the
     resume on the shifted timeline;
   - the thread was held up so long that the head of the batch is overdue by more than
     LATE_MAX_NS: those frames are dropped like any late frames.
  Between the pipes only the flush check is repeated: a pause that begins in the middle
  of a batch does not split it, every pipe gets the same data.
*/
static void deliver(leadtap_t *lt, const int16_t *out, size_t got, uint64_t gen, uint64_t j0)
{
  size_t skip = 0;

  pthread_mutex_lock(&lt->mu);
  if (lt->gen != gen || lt->base == UNSET)
  {
    pthread_mutex_unlock(&lt->mu);
    return;
  }
  if (lt->paused)
  {
    if (lt->rd == j0 + got)
      lt->rd = j0;
    pthread_mutex_unlock(&lt->mu);
    return;
  }
  int64_t over = now_ns() + (int64_t)lt->lead_ms * 1000000LL - (lt->base + frames_ns(j0)) - LATE_MAX_NS;
  if (over > 0)
  {
    skip = (size_t)ns_frames(over) + 1;
    if (skip > got)
      skip = got;
    lt->st_late += skip;
  }
  pthread_mutex_unlock(&lt->mu);

  for (unsigned i = 0; skip < got && i < lt->npipes; i++)
  {
    if (i)
    {
      pthread_mutex_lock(&lt->mu);
      int stale = lt->gen != gen;
      pthread_mutex_unlock(&lt->mu);
      if (stale)
        return;
    }
    pipe_write(&lt->pipe[i], (const char *)(out + skip * 2), (got - skip) * 4, now_ns());
  }
}

static void *pacer(void *arg)
{
  leadtap_t *lt = arg;
  int16_t out[RELBUF * 2];

  pthread_mutex_lock(&lt->mu);
  while (!lt->quit)
  {
    int64_t now = now_ns(), wake = 0; /* 0: nothing scheduled, wait for a signal */
    if (!lt->paused && lt->base != UNSET && lt->rd < lt->wr)
    {
      /* release what is due within the next half batch: the copy is at most
         BATCH_NS/2 early or late */
      size_t got = q_release(lt, out, RELBUF, now + BATCH_NS / 2);
      if (got)
      {
        uint64_t gen = lt->gen, j0 = lt->rd - got;
        pthread_mutex_unlock(&lt->mu);
        deliver(lt, out, got, gen, j0);
        pthread_mutex_lock(&lt->mu);
        if (got == RELBUF)
          continue; /* more may be due */
      }
      if (lt->rd < lt->wr && !lt->paused && lt->base != UNSET)
      {
        wake = lt->base + frames_ns(lt->rd) - (int64_t)lt->lead_ms * 1000000LL - BATCH_NS / 2;
        if (wake < now + BATCH_NS)
          wake = now + BATCH_NS;
      }
    }
    if (wake)
    {
      struct timespec ts = {.tv_sec = wake / 1000000000LL, .tv_nsec = wake % 1000000000LL};
      pthread_cond_timedwait(&lt->cv, &lt->mu, &ts);
    }
    else
      pthread_cond_wait(&lt->cv, &lt->mu);
  }
  pthread_mutex_unlock(&lt->mu);
  return NULL;
}

/* ------------------------------------------------------------ converter */

static inline __attribute__((always_inline)) int32_t to_s16(const uint8_t *p, int kind)
{
  if (kind == K_S32)
  {
    int32_t v;
    memcpy(&v, p, 4);
    return v >> 16;
  }
  if (kind == K_F32)
  {
    float f;
    memcpy(&f, p, 4);
    if (f != f)
      return 0;
    if (f > 1.0f)
      f = 1.0f;
    if (f < -1.0f)
      f = -1.0f;
    return (int32_t)(f * 32767.0f);
  }
  int16_t v;
  memcpy(&v, p, 2);
  return v;
}

/*
  Convert nf input frames (first two channels) to 48 kHz S16 stereo in lt->scratch.
  Linear interpolation with the output phase counted in 1/48000 steps:
  no drift, one output per input
  frame at 48 kHz. Inlined once per sample format so the format test is not in the loop.
*/
static inline __attribute__((always_inline)) size_t convert(leadtap_t *lt, const uint8_t *src, size_t nf, size_t cap,
                                                            int kind)
{
  const unsigned rate = lt->io.rate, ch = lt->io.channels;
  const size_t sample_bytes = lt->frame_bytes / ch;
  size_t no = 0;

  for (size_t i = 0; i < nf; i++)
  {
    const uint8_t *f = src + i * lt->frame_bytes;
    int32_t in[2];
    in[0] = to_s16(f, kind);
    in[1] = ch > 1 ? to_s16(f + sample_bytes, kind) : in[0];
    if (kind == K_S16 && ch >= 2 && (uint16_t)in[0] == IEC_PA && (uint16_t)in[1] == IEC_PB)
      lt->iec_hold = IEC_HOLD_FRAMES;
    if (lt->iec_hold)
    {
      lt->iec_hold--;
      in[0] = in[1] = 0;
    }
    while (lt->phase <= OUT_RATE && no < cap)
    {
      int64_t w = ((int64_t)(lt->phase < 0 ? 0 : lt->phase) << 16) / OUT_RATE;
      for (int c = 0; c < 2; c++)
        lt->scratch[2 * no + c] = (int16_t)(lt->prev[c] + (((int64_t)in[c] - lt->prev[c]) * w >> 16));
      no++;
      lt->phase += (int32_t)rate;
    }
    lt->phase -= OUT_RATE;
    lt->prev[0] = in[0];
    lt->prev[1] = in[1];
  }
  return no;
}

static void feed_copy(leadtap_t *lt, const uint8_t *src, size_t nf, int64_t t_play)
{
  size_t cap = nf * OUT_RATE / lt->io.rate + 4, no;

  if (cap > lt->scratch_frames)
    return; /* cannot happen: the scratch is sized for a full slave buffer */
  switch (lt->io.format)
  {
    case SND_PCM_FORMAT_S32_LE:
      no = convert(lt, src, nf, cap, K_S32);
      break;
    case SND_PCM_FORMAT_FLOAT_LE:
      no = convert(lt, src, nf, cap, K_F32);
      break;
    default:
      no = convert(lt, src, nf, cap, K_S16);
  }
  q_append(lt, t_play, lt->scratch, no);
}

static void conv_reset(leadtap_t *lt)
{
  lt->phase = OUT_RATE; /* first output = first input */
  lt->prev[0] = lt->prev[1] = 0;
  lt->iec_hold = 0;
}

/* ------------------------------------------------------------ callbacks */

static snd_pcm_sframes_t slave_delay(leadtap_t *lt)
{
  snd_pcm_sframes_t dl = 0;
  int err = snd_pcm_delay(lt->slave, &dl);
  if (err < 0)
    return err;
  return dl < 0 ? 0 : dl;
}

static int lt_start(snd_pcm_ioplug_t *io)
{
  leadtap_t *lt = io->private_data;
  int err = snd_pcm_start(lt->slave);
  if (err < 0)
    return err;
  int64_t now = now_ns();
  /* A slave may delay audio beyond what is queued in its buffer (driver ring, HDMI
     FIFO, dmix): frame 0 is audible only after that hidden part. */
  snd_pcm_sframes_t dl = slave_delay(lt);
  int64_t hidden = dl > 0 && (uint64_t)dl > lt->written ? (int64_t)((uint64_t)dl - lt->written) : 0;
  lt->running = 1;
  /* No delay, no anchor: the timeline would start at a wrong time. The next write anchors it
     (a copy of what was queued before the start is then not sent). */
  pthread_mutex_lock(&lt->mu);
  if (dl < 0)
    lt->st_delay_err++;
  else if (lt->wr && lt->base == UNSET)
  {
    lt->base = now + hidden * 1000000000LL / (int64_t)io->rate; /* audio queued before start */
    pthread_cond_signal(&lt->cv);
  }
  pthread_mutex_unlock(&lt->mu);
  return 0;
}

static int lt_stop(snd_pcm_ioplug_t *io)
{
  leadtap_t *lt = io->private_data;
  lt->running = 0;
  snd_pcm_drop(lt->slave);
  q_flush(lt);
  return 0;
}

static snd_pcm_sframes_t lt_pointer(snd_pcm_ioplug_t *io)
{
  leadtap_t *lt = io->private_data;
  snd_pcm_state_t st = snd_pcm_state(lt->slave);
  if (st == SND_PCM_STATE_XRUN)
    return -EPIPE;
  /* After drop or drain the slave is in SETUP and refuses avail; alsa-lib still asks
     for the position and would turn an error into an xrun. */
  if (st == SND_PCM_STATE_SETUP || st == SND_PCM_STATE_OPEN)
    return lt->last_ptr;
  /* From the fill level, not the delay: a driver may delay audio beyond its ALSA buffer
     (own DMA ring). Counting that here would make the application keep that much less
     queued. The larger figure is reported by lt_delay only. */
  snd_pcm_sframes_t av = snd_pcm_avail(lt->slave);
  if (av < 0)
    return av;
  snd_pcm_uframes_t queued = (snd_pcm_uframes_t)av < lt->slave_buffer ? lt->slave_buffer - (snd_pcm_uframes_t)av : 0;
  uint64_t consumed = lt->written > queued ? lt->written - queued : 0;
  lt->last_ptr = (snd_pcm_sframes_t)(consumed % io->buffer_size);
  return lt->last_ptr;
}

static int lt_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
  snd_pcm_sframes_t dl = slave_delay(io->private_data);
  if (dl < 0)
    return (int)dl;
  *delayp = dl;
  return 0;
}

/* Blocking mode: wait for the slave. A timeout is not an error (the caller loops); a failed
   wait is: the slave is gone or has run dry. */
static int wait_slave(snd_pcm_ioplug_t *io, leadtap_t *lt)
{
  int r = snd_pcm_wait(lt->slave, 100);
  if (r == -EPIPE)
    snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
  return r < 0 ? r : 0;
}

/*
  alsa-lib's generic ioplug implements snd_pcm_rewind / snd_pcm_forward (and rewindable /
  forwardable) as pure moves of the application pointer: there is no callback, so the slave
  and the copy queue never hear of it. The pointer would stay off by the moved distance,
  and with it the free space the application is told about. Frames given to the slave
  cannot be taken back, so the move is undone here: the pointer must equal the number of
  frames the slave has accepted. Only the transfer path may do this (it runs under the PCM
  lock, with the pointer not yet advanced for the transfer in progress). A distance of more
  than one buffer cannot come from rewind/forward and is left alone.
*/
static void fix_appl(snd_pcm_ioplug_t *io, leadtap_t *lt)
{
  snd_pcm_uframes_t bs = io->buffer_size, b = bs;

  if (!bs)
    return;
  while (b <= ((snd_pcm_uframes_t)LONG_MAX - bs) / 2) /* alsa-lib's boundary */
    b *= 2;
  snd_pcm_uframes_t want = (snd_pcm_uframes_t)(lt->written % b), have = io->appl_ptr % b;
  snd_pcm_uframes_t fwd = (have + b - want) % b;
  if (fwd && (fwd <= bs || b - fwd <= bs))
  {
    io->appl_ptr = want;
    lt->st_appl_fix++;
  }
}

static snd_pcm_sframes_t lt_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
                                     snd_pcm_uframes_t size)
{
  leadtap_t *lt = io->private_data;
  const uint8_t *src = (const uint8_t *)areas[0].addr + (areas[0].first + offset * areas[0].step) / 8;
  snd_pcm_uframes_t done = 0;

  fix_appl(io, lt);
  while (done < size)
  {
    snd_pcm_sframes_t av = snd_pcm_avail(lt->slave);
    if (av == -EPIPE || snd_pcm_state(lt->slave) == SND_PCM_STATE_XRUN)
    {
      snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
      return done ? (snd_pcm_sframes_t)done : -EPIPE;
    }
    if (av < 0)
      return done ? (snd_pcm_sframes_t)done : av;
    if (av == 0)
    {
      if (done)
        break;
      if (io->nonblock)
        return -EAGAIN;
      int r = wait_slave(io, lt);
      if (r < 0)
        return r;
      continue;
    }
    snd_pcm_uframes_t n = size - done < (snd_pcm_uframes_t)av ? size - done : (snd_pcm_uframes_t)av;

    /* Frames written now play after everything queued in the slave (and, while paused,
       only after the resume: the pause shifts the timeline by its length). Before start
       nothing plays: lt_start anchors the queue then. */
    int64_t t_play = UNSET;
    int copy = 1;
    if (lt->running)
    {
      snd_pcm_sframes_t dl = slave_delay(lt);
      pthread_mutex_lock(&lt->mu);
      int64_t ref = lt->paused ? lt->pause_ns : now_ns();
      pthread_mutex_unlock(&lt->mu);
      if (dl < 0)
      {
        /* The play time of these frames is unknown: do not guess. They are not copied; the
           next write that has a delay re-anchors and fills the hole with silence. */
        copy = 0;
        lt->st_delay_err++;
      }
      else
        t_play = ref + (int64_t)((uint64_t)dl * 1000000000ULL / io->rate);
    }
    snd_pcm_sframes_t w = snd_pcm_writei(lt->slave, src + done * lt->frame_bytes, n);
    if (w == -EAGAIN || w == 0)
    {
      /* The slave reported space but took nothing: hand progress or -EAGAIN to the caller;
         a blocking caller waits (and pauses briefly: the descriptor may keep saying ready). */
      if (done)
        break;
      if (io->nonblock)
        return -EAGAIN;
      int r = wait_slave(io, lt);
      if (r < 0)
        return r;
      usleep(1000);
      continue;
    }
    if (w == -EPIPE)
    {
      snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
      return done ? (snd_pcm_sframes_t)done : -EPIPE;
    }
    if (w < 0)
      return done ? (snd_pcm_sframes_t)done : w;
    if (w > 0)
    {
      if (copy)
        feed_copy(lt, src + done * lt->frame_bytes, (size_t)w, t_play);
      lt->written += (uint64_t)w;
      done += (snd_pcm_uframes_t)w;
    }
  }
  return (snd_pcm_sframes_t)done;
}

static int lt_prepare(snd_pcm_ioplug_t *io)
{
  leadtap_t *lt = io->private_data;
  int err = snd_pcm_prepare(lt->slave);
  if (err < 0)
    return err;
  lt->written = 0;
  lt->last_ptr = 0;
  lt->running = 0;
  pthread_mutex_lock(&lt->mu);
  lt->paused = 0;
  flush_locked(lt);
  pthread_mutex_unlock(&lt->mu);
  conv_reset(lt);
  return 0;
}

static int lt_drain(snd_pcm_ioplug_t *io)
{
  leadtap_t *lt = io->private_data;
  int err = 0;

  if (snd_pcm_state(lt->slave) == SND_PCM_STATE_PREPARED && lt->written)
  {
    err = lt_start(io);
    if (err < 0)
      return err;
  }
  if (snd_pcm_state(lt->slave) != SND_PCM_STATE_SETUP)
  {
    /* Non-blocking: the slave answers -EAGAIN while it plays out; alsa-lib keeps the
       stream DRAINING and calls again. Blocking: wait for the slave here. */
    if (!io->nonblock)
      snd_pcm_nonblock(lt->slave, 0);
    err = snd_pcm_drain(lt->slave);
    if (!io->nonblock)
      snd_pcm_nonblock(lt->slave, 1);
    if (err == -EAGAIN)
      return err;
  }
  lt->running = 0;
  q_flush(lt);
  return err;
}

static int lt_pause(snd_pcm_ioplug_t *io, int enable)
{
  leadtap_t *lt = io->private_data;
  int err = snd_pcm_pause(lt->slave, enable);
  if (err < 0)
    return err;
  int64_t now = now_ns();
  pthread_mutex_lock(&lt->mu);
  if (enable)
  {
    lt->paused = 1;
    lt->pause_ns = now;
  }
  else if (lt->paused)
  {
    if (lt->base != UNSET)
      lt->base += now - lt->pause_ns;
    lt->paused = 0;
    pthread_cond_signal(&lt->cv);
  }
  pthread_mutex_unlock(&lt->mu);
  return 0;
}

static int lt_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
  leadtap_t *lt = io->private_data;
  snd_pcm_hw_params_t *hw;
  snd_pcm_sw_params_t *sw;
  snd_pcm_uframes_t period = io->period_size, buffer = io->buffer_size, slave_period, boundary;
  int err;
  (void)params;

  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_sw_params_alloca(&sw);
  if ((err = snd_pcm_hw_params_any(lt->slave, hw)) < 0 ||
      (err = snd_pcm_hw_params_set_access(lt->slave, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
      (err = snd_pcm_hw_params_set_format(lt->slave, hw, io->format)) < 0 ||
      (err = snd_pcm_hw_params_set_channels(lt->slave, hw, io->channels)) < 0 ||
      (err = snd_pcm_hw_params_set_rate(lt->slave, hw, io->rate, 0)) < 0)
    return err;
  /* A period of another size is harmless. The buffer must not be smaller than the one the
     application negotiated: it is told it may queue that much, and a smaller slave buffer
     would leave it waiting for space that never comes. set_constraints keeps the
     application inside the slave's range; this is the safety net, so take the smallest
     slave buffer that is not below it (the *_near setters are unusable here, see below). */
  snd_pcm_hw_params_set_period_size(lt->slave, hw, period, 0);
  if ((err = snd_pcm_hw_params_set_buffer_size_min(lt->slave, hw, &buffer)) < 0 ||
      (err = snd_pcm_hw_params_set_buffer_size(lt->slave, hw, buffer)) < 0)
  {
    SNDERR("leadtap: slave cannot hold the negotiated buffer of %lu frames", (unsigned long)io->buffer_size);
    return -EINVAL;
  }
  if ((err = snd_pcm_hw_params(lt->slave, hw)) < 0 || (err = snd_pcm_get_params(lt->slave, &lt->slave_buffer, &slave_period)) < 0)
    return err;

  /* The slave must not start by itself: ALSA assumes a standing device in PREPARED
     and lt_start is the one place that starts it (and anchors the copy). */
  if ((err = snd_pcm_sw_params_current(lt->slave, sw)) < 0 || (err = snd_pcm_sw_params_get_boundary(sw, &boundary)) < 0 ||
      (err = snd_pcm_sw_params_set_start_threshold(lt->slave, sw, boundary)) < 0 ||
      (err = snd_pcm_sw_params(lt->slave, sw)) < 0)
    return err;

  lt->frame_bytes = (size_t)snd_pcm_format_physical_width(io->format) / 8 * io->channels;
  size_t copy_frames = (size_t)lt->slave_buffer * OUT_RATE / io->rate;
  size_t ring_n = copy_frames + OUT_RATE / 2 + RELBUF; /* queue plus slack for a hidden delay */
  size_t scratch_n = copy_frames + 8;
  int16_t *ring = calloc(ring_n, 4), *scratch = malloc(scratch_n * 4);
  if (!ring || !scratch)
  {
    free(ring);
    free(scratch);
    return -ENOMEM;
  }
  pthread_mutex_lock(&lt->mu);
  free(lt->ring);
  lt->ring = ring;
  lt->ring_n = ring_n;
  flush_locked(lt);
  /* slave pointers that move in period steps make the measured delay jitter by a period */
  int64_t tol = (int64_t)slave_period * 1000000000LL / (int64_t)io->rate;
  lt->tol_ns = tol > TOL_MIN_NS ? tol : TOL_MIN_NS;
  pthread_mutex_unlock(&lt->mu);
  free(lt->scratch);
  lt->scratch = scratch;
  lt->scratch_frames = scratch_n;
  return 0;
}

static int lt_hw_free(snd_pcm_ioplug_t *io)
{
  leadtap_t *lt = io->private_data;
  return snd_pcm_hw_free(lt->slave);
}

static int lt_poll_count(snd_pcm_ioplug_t *io)
{
  return snd_pcm_poll_descriptors_count(((leadtap_t *)io->private_data)->slave);
}

static int lt_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int space)
{
  return snd_pcm_poll_descriptors(((leadtap_t *)io->private_data)->slave, pfd, space);
}

/* The slave is another PCM: asking it here does not touch our own lock. */
static int lt_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds, unsigned short *revents)
{
  return snd_pcm_poll_descriptors_revents(((leadtap_t *)io->private_data)->slave, pfd, nfds, revents);
}

/* Also the cleanup path of a failed open: copes with no slave and no thread. */
static int lt_close(snd_pcm_ioplug_t *io)
{
  leadtap_t *lt = io->private_data;

  if (lt->thread_started)
  {
    pthread_mutex_lock(&lt->mu);
    lt->quit = 1;
    pthread_cond_signal(&lt->cv);
    pthread_mutex_unlock(&lt->mu);
    pthread_join(lt->thread, NULL);
  }
  if (lt->verbose)
  {
    fprintf(stderr, "leadtap: late %llu, overflow/overlap dropped %llu, gaps %llu overlaps %llu;",
            (unsigned long long)lt->st_late, (unsigned long long)lt->st_dropped, (unsigned long long)lt->st_gaps,
            (unsigned long long)lt->st_overlaps);
    fprintf(stderr, " delay errors %llu, rewind/forward undone %llu;", (unsigned long long)lt->st_delay_err,
            (unsigned long long)lt->st_appl_fix);
    for (unsigned i = 0; i < lt->npipes; i++)
      fprintf(stderr, " pipe %u out %llu dropped %llu;", i, (unsigned long long)lt->pipe[i].out,
              (unsigned long long)lt->pipe[i].dropped);
    fprintf(stderr, "\n");
  }
  for (unsigned i = 0; i < lt->npipes; i++)
  {
    if (lt->pipe[i].fd >= 0)
      close(lt->pipe[i].fd);
    free(lt->pipe[i].path);
  }
  if (lt->slave)
    snd_pcm_close(lt->slave);
  pthread_mutex_destroy(&lt->mu);
  pthread_cond_destroy(&lt->cv);
  free(lt->ring);
  free(lt->scratch);
  free(lt);
  return 0;
}

static const snd_pcm_ioplug_callback_t lt_callback = {
    .start = lt_start,
    .stop = lt_stop,
    .pointer = lt_pointer,
    .transfer = lt_transfer,
    .close = lt_close,
    .hw_params = lt_hw_params,
    .hw_free = lt_hw_free,
    .prepare = lt_prepare,
    .drain = lt_drain,
    .poll_descriptors_count = lt_poll_count,
    .poll_descriptors = lt_poll_descriptors,
    .poll_revents = lt_poll_revents,
    .delay = lt_delay,
};

/* ----------------------------------------------------------- constraints */

/*
  Offer what the slave can do. Format, channels and rate come from its unrestricted
  parameter space. Buffer and period limits depend on format and channels, so they are
  taken over every combination offered and converted to bytes (the unit ioplug uses):
  the application must never negotiate a buffer the slave cannot hold.

  The limits are read with the set_*_min/max functions (restrict, then report the limit
  that results). The snd_pcm_hw_params_get_*_min/max getters and the *_near setters must
  not be used in this plugin: it is not linked against libasound, an unversioned reference
  binds to the ALSA_0.9 variants of these (value returned, other argument list), and the
  call silently does nothing useful.
*/
static int set_constraints(leadtap_t *lt)
{
  static const snd_pcm_format_t all_formats[] = {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_FLOAT_LE};
  unsigned formats[3], nf = 0, cmin = 1, cmax = 8, rmin = 1, rmax = 1u << 20;
  unsigned long long pbmin = 0, pbmax = 1u << 20, bbmin = 0, bbmax = 1u << 24;
  snd_pcm_hw_params_t *any, *hw;
  snd_pcm_uframes_t a, b, c, d;
  int err, dir = 0;

  snd_pcm_hw_params_alloca(&any);
  snd_pcm_hw_params_alloca(&hw);
  if ((err = snd_pcm_hw_params_any(lt->slave, any)) < 0 ||
      (err = snd_pcm_hw_params_set_access(lt->slave, any, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
      (err = snd_pcm_hw_params_set_channels_min(lt->slave, any, &cmin)) < 0 ||
      (err = snd_pcm_hw_params_set_channels_max(lt->slave, any, &cmax)) < 0 ||
      (err = snd_pcm_hw_params_set_rate_min(lt->slave, any, &rmin, &dir)) < 0 ||
      (err = snd_pcm_hw_params_set_rate_max(lt->slave, any, &rmax, &dir)) < 0)
    return err;
  for (unsigned i = 0; i < 3; i++)
  {
    int ok = 0;
    for (unsigned ch = cmin; ch <= cmax; ch++)
    {
      a = c = 1;
      b = 1u << 24;
      d = 1u << 20;
      snd_pcm_hw_params_copy(hw, any);
      if (snd_pcm_hw_params_set_format(lt->slave, hw, all_formats[i]) < 0 || snd_pcm_hw_params_set_channels(lt->slave, hw, ch) < 0 ||
          snd_pcm_hw_params_set_buffer_size_min(lt->slave, hw, &a) < 0 || snd_pcm_hw_params_set_buffer_size_max(lt->slave, hw, &b) < 0 ||
          snd_pcm_hw_params_set_period_size_min(lt->slave, hw, &c, &dir) < 0 ||
          snd_pcm_hw_params_set_period_size_max(lt->slave, hw, &d, &dir) < 0)
        continue;
      unsigned long long fb = (unsigned long long)snd_pcm_format_physical_width(all_formats[i]) / 8 * ch;
      ok = 1;
      if (a * fb > bbmin)
        bbmin = a * fb;
      if (b * fb < bbmax)
        bbmax = b * fb;
      if (c * fb > pbmin)
        pbmin = c * fb;
      if (d * fb < pbmax)
        pbmax = d * fb;
    }
    if (ok)
      formats[nf++] = all_formats[i];
  }
  if (!nf || cmin > cmax || bbmin > bbmax || pbmin > pbmax)
  {
    SNDERR("leadtap: slave offers no usable S16_LE/S32_LE/FLOAT_LE configuration");
    return -EINVAL;
  }

  static const unsigned access[] = {SND_PCM_ACCESS_RW_INTERLEAVED};
  if ((err = snd_pcm_ioplug_set_param_list(&lt->io, SND_PCM_IOPLUG_HW_ACCESS, 1, access)) < 0 ||
      (err = snd_pcm_ioplug_set_param_list(&lt->io, SND_PCM_IOPLUG_HW_FORMAT, nf, formats)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(&lt->io, SND_PCM_IOPLUG_HW_CHANNELS, cmin, cmax)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(&lt->io, SND_PCM_IOPLUG_HW_RATE, rmin, rmax)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(&lt->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, (unsigned)pbmin, (unsigned)pbmax)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(&lt->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 1024)) < 0 ||
      (err = snd_pcm_ioplug_set_param_minmax(&lt->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, (unsigned)bbmin, (unsigned)bbmax)) < 0)
    return err;
  return 0;
}

/* ------------------------------------------------------------------ open */

SND_PCM_PLUGIN_DEFINE_FUNC(leadtap)
{
  snd_config_iterator_t i, next;
  const char *slave_name = NULL, *fifos[MAX_PIPES];
  unsigned nfifo = 0;
  long lead = DEFAULT_LEAD_MS;
  int err;

  snd_config_for_each(i, next, conf)
  {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    if (snd_config_get_id(n, &id) < 0)
      continue;
    if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
      continue;
    if (strcmp(id, "slave") == 0)
    {
      if (snd_config_get_string(n, &slave_name) == 0)
        continue;
      snd_config_t *p;
      if (snd_config_search(n, "pcm", &p) == 0 && snd_config_get_string(p, &slave_name) == 0)
        continue;
      SNDERR("leadtap: slave.pcm must be a PCM name (string)");
      return -EINVAL;
    }
    if (strcmp(id, "lead") == 0)
    {
      if (snd_config_get_integer(n, &lead) < 0 || lead < 0 || lead > MAX_LEAD_MS)
      {
        SNDERR("leadtap: lead must be 0..%d (ms)", MAX_LEAD_MS);
        return -EINVAL;
      }
      continue;
    }
    if (strcmp(id, "fifo") == 0)
    {
      /* one path, or a compound of paths */
      if (nfifo == 0 && snd_config_get_string(n, &fifos[0]) == 0)
      {
        nfifo = 1;
        continue;
      }
      snd_config_iterator_t j, jn;
      if (nfifo == 0 && snd_config_get_type(n) == SND_CONFIG_TYPE_COMPOUND)
      {
        snd_config_for_each(j, jn, n)
        {
          if (nfifo == MAX_PIPES || snd_config_get_string(snd_config_iterator_entry(j), &fifos[nfifo]) < 0)
          {
            nfifo = 0;
            break;
          }
          nfifo++;
        }
        if (nfifo)
          continue;
      }
      SNDERR("leadtap: fifo must be a path or a list of 1..%d paths", MAX_PIPES);
      return -EINVAL;
    }
    SNDERR("leadtap: unknown field %s", id);
    return -EINVAL;
  }
  if (!slave_name || !nfifo)
  {
    SNDERR("leadtap: slave.pcm and fifo are required");
    return -EINVAL;
  }
  if (stream != SND_PCM_STREAM_PLAYBACK)
    return -EINVAL;

  leadtap_t *lt = calloc(1, sizeof(*lt));
  if (!lt)
    return -ENOMEM;
  lt->lead_ms = (unsigned)lead;
  lt->verbose = getenv("LEADTAP_VERBOSE") != NULL;
  lt->base = UNSET;
  lt->tol_ns = TOL_MIN_NS;
  lt->npipes = nfifo;
  for (unsigned k = 0; k < nfifo; k++)
  {
    lt->pipe[k].fd = PIPE_CLOSED;
    lt->pipe[k].path = strdup(fifos[k]);
  }
  pthread_mutex_init(&lt->mu, NULL);
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  pthread_cond_init(&lt->cv, &ca);
  pthread_condattr_destroy(&ca);
  conv_reset(lt);

  lt->cb = lt_callback;
  lt->io.version = SND_PCM_IOPLUG_VERSION;
  lt->io.name = "leadtap";
  lt->io.private_data = lt;
  lt->io.mmap_rw = 0;
  lt->io.poll_fd = -1; /* the callbacks forward the slave's descriptors */
  lt->io.callback = &lt->cb;

  for (unsigned k = 0; k < nfifo; k++)
    if (!lt->pipe[k].path)
    {
      err = -ENOMEM;
      goto fail;
    }
  /* The application's NO_AUTO_* choices apply to the slave too. */
  err = snd_pcm_open_lconf(&lt->slave, slave_name, SND_PCM_STREAM_PLAYBACK,
                           SND_PCM_NONBLOCK | (mode & (SND_PCM_NO_AUTO_RESAMPLE | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT)),
                           root);
  if (err < 0)
    goto fail;
  snd_pcm_nonblock(lt->slave, 1);
  {
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    err = snd_pcm_hw_params_any(lt->slave, hw);
    if (err < 0)
      goto fail;
    if (snd_pcm_hw_params_can_pause(hw))
      lt->cb.pause = lt_pause; /* offered only when the slave can pause */
  }

  err = snd_pcm_ioplug_create(&lt->io, name, stream, mode);
  if (err < 0)
    goto fail;
  lt->io.nonblock = (mode & SND_PCM_NONBLOCK) != 0; /* ioplug only learns it from snd_pcm_nonblock() */
  err = set_constraints(lt);
  if (err < 0)
  {
    snd_pcm_ioplug_delete(&lt->io); /* calls lt_close */
    return err;
  }
  if (pthread_create(&lt->thread, NULL, pacer, lt) != 0)
  {
    snd_pcm_ioplug_delete(&lt->io); /* calls lt_close */
    return -EAGAIN;
  }
  lt->thread_started = 1;
  *pcmp = lt->io.pcm;
  return 0;

fail:
  lt_close(&lt->io);
  return err;
}

SND_PCM_PLUGIN_SYMBOL(leadtap);
