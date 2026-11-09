# alsa-leadtap

An ALSA PCM plugin that sits between a player (Kodi, mpv, ...) and any slave PCM and does
two things at once:

* the audio goes to the slave **unchanged**, and the plugin reports the slave's `delay`, so
  the player's lip sync stays correct;
* a **copy** of the audio is written to one or more named pipes **a fixed time before it
  becomes audible** on the slave (default 180 ms), regardless of how far ahead the player
  writes.

Purpose: multiroom audio in sync with a picture. A Snapcast server (or anything else that
reads a pipe) can play the copy in another room; because the copy is early by a known
amount, the remote player only needs a fixed buffer instead of a hand-tuned latency.

## How it works

Frames written at time `T` become audible at `T + snd_pcm_delay(slave)`, measured just
before each write (CLOCK_MONOTONIC), so the timeline is re-anchored at every write and
cannot drift against the audio clock. At stream start the plugin also measures how much of
the slave's delay lies beyond its buffer (driver ring, HDMI FIFO) and anchors the first
frame accordingly. The copy is released by a small pacer thread when `play time - lead` is
reached (within +-5 ms: the pacer wakes every 10 ms and releases what is due before its next
wake-up; it sleeps while nothing is queued, before the start and during a pause).

Copy format is fixed: **48 kHz, S16_LE, stereo** (linear resampler from the stream rate;
first two channels of multichannel input). Input formats S16_LE, S32_LE, FLOAT_LE.

Behaviour:

* **IEC 61937** (AC3/DTS passthrough, detected by the Pa/Pb sync words in S16_LE stereo):
  the copy is silence for as long as the bitstream flows.
* The copy never blocks the player. Each pipe is handled on its own: no reader on a pipe,
  or ~25 ms waiting in it (a hard bound, also within one write), means the copy is dropped
  *for that pipe*; the others are not affected. All pipes get the same data. Writes to a pipe are at most `PIPE_BUF`
  bytes, so they are atomic and the stream stays frame-aligned.
* The first `lead` of a stream cannot be early (there is no earlier audio to send).
  Frames more than 20 ms past their release time are dropped rather than sent late; the
  same happens if the player keeps less than `lead` queued. Counters are printed on close
  when `LEADTAP_VERBOSE` is set.
* A jump of the measured play time against the continuous timeline is corrected by
  silence (gap) or by skipping (overlap) in the copy. Jitter below 5 ms, or below one
  period of the slave if that is larger (slaves whose position moves in period steps),
  is ignored.
* `drop`, `prepare` (underrun recovery) discard the queued copy; `pause` holds what has not
  been released yet (copy already handed out before the pause stays out, so a receiver
  plays up to `lead` into a pause; frames written during a pause play after the resume);
  `drain` waits for the slave, or in a non-blocking PCM returns `-EAGAIN` until the slave
  has played out.
* Pause is offered to the application only if the slave can pause.
* `snd_pcm_rewind` and `snd_pcm_forward` have no effect on the audio: alsa-lib moves only the
  application pointer of an external plugin and the audio is already with the slave. The
  calls are accepted (alsa-lib reports the frames as moved), the plugin puts the pointer back
  at the next write, so the free space and the delay stay correct. Applications must not
  rely on rewinding.
* If the slave's delay cannot be read for a write, that write is not copied (its play time is
  unknown); the next write re-anchors the timeline and the hole is filled with silence.
* The buffer and period sizes offered to the application are limited to what the slave
  accepts. The sample rate has to be one the slave supports itself (the plugin does not
  resample; put `plug` in front of it if needed).

## Configuration

```
pcm_type.leadtap {
    lib "/path/to/libasound_module_pcm_leadtap.so"
}

pcm.mydevice {
    type leadtap
    slave.pcm "hw:0"            # name of any PCM (a string, not an inline definition)
    lead 180                    # ms before play time, 0..2000, default 180
    fifo "/run/leadtap.fifo"    # named pipe; the reader creates it and opens it for reading
}
```

`fifo` takes either one path or a list of up to 4 paths, each with its own reader:

```
    fifo [ "/run/leadtap-a.fifo" "/run/leadtap-b.fifo" ]
```

The plugin opens each pipe read-write and non-blocking when audio flows (so a reader that
closes and reopens it, like snapserver without a writer, is not missed); it never creates
a pipe and refuses a path that is not a named pipe. A pipe that does not exist yet is
looked for again every 500 ms. For players that restrict device names (e.g. Kodi only
offers pass-through on `iec958*`/`hdmi*` names) put the plugin into the card definition
behind the name the player expects.

The `slave.pcm` device is opened with the application's `SND_PCM_NO_AUTO_*` flags.

## Build

Needs the ALSA headers, not the library (the plugin is loaded into a process that already
has libasound):

```
make plugin                                  # build/libasound_module_pcm_leadtap.so
make plugin CC=aarch64-linux-gnu-gcc ALSA_INC=/path/to/dir-containing-alsa
```

Because the plugin is not linked against libasound, it must not call the
`snd_pcm_hw_params_get_*_min/max` getters or the `*_near` setters: an unversioned reference
binds to the old ALSA_0.9 variants of those, with a different argument list.

`make test` builds a fake slave device and the selftest and runs it; it needs alsa-lib but
no audio hardware. The selftest checks that the copy arrives `lead` before the computed
play time (44.1 and 48 kHz; S16, S32 and float input; little and much audio queued at
start; paced writes; a buffer smaller than the stream; pause, also with writes during the
pause; a slave with delay beyond its buffer; a slave whose position moves in steps; two
pipes, one of them unread; a reader that leaves and comes back; `hw_params` twice), that
the copy of a tone has no discontinuities, that IEC 61937 gives a silent copy, that `drop`
empties the queue and a new stream works after `drop`, an underrun and a close while
running, that blocking and non-blocking `drain` work, that a slave with a small maximum
buffer does not hang the player, and that the delay seen by the application equals the
slave's.

## LibreELEC add-on

`libreelec/` packages the plugin together with [wled-audiosync](https://github.com/ehb/wled-audiosync)
(reads the second pipe, sends WLED Audio Sync) as the Kodi service add-on
`service.alsa-leadtap`, for 64-bit Raspberry Pi (aarch64). It is tied to the architecture, not
to a kernel or LibreELEC version.

The zip `service.alsa-leadtap-<version>.zip` is attached to each release on the
[Releases page](https://github.com/ehb/alsa-leadtap/releases). The wled-audiosync binary
inside it (GPL-3.0-or-later) is built from the wled-audiosync tag named in
`ci/wled-audiosync.ref`; that public tag is its corresponding source. To build the zip
yourself:

```sh
python libreelec/build_addon.py --so build/aarch64/libasound_module_pcm_leadtap.so \
    --audiosync path/to/aarch64/wled-audiosync --audiosync-license path/to/wled-audiosync/LICENSE
```

The script refuses files that are not 64-bit AArch64 ELF. The version comes from the newest
`CHANGELOG.md` entry unless `--version x.y.z` is given. Install the zip in Kodi (Settings ->
Add-ons -> Install from zip file; allow unknown sources first). LibreELEC then enables a
systemd unit that starts before Kodi, writes the ALSA configuration and keeps
wled-audiosync running (restarted when it dies). **Restart Kodi once after the first
installation:** a running Kodi does not see the new audio device.

What you get:

* an audio device **`hdmi_leadtap`**, shown in Kodi as "Audio with early copy (leadtap)".
  The name begins with `hdmi` on purpose: Kodi offers passthrough only on `iec958`, `spdif`
  and `hdmi` names. Choose it as the audio output device. For AC3/DTS pick the sound card
  itself as the passthrough device (the plugin device carries no passthrough, and the copy
  is silent for such streams).
* pipes `/tmp/leadtap.fifo` (for a Snapcast server) and, if WLED is on,
  `/tmp/leadtap-sync.fifo` (read by the bundled wled-audiosync). Copy format 48 kHz, S16_LE,
  stereo.

Settings (add-on settings in Kodi):

| Setting | Default | Meaning |
|---|---|---|
| Sound card (ALSA device) | `hdmi_bmc` | name of the slave PCM that plays the sound, see `aplay -L`. The default is the optical S/PDIF output provided by the add-on service.rpi-gpio-spdif; **with HDMI or another sound card this setting must be changed**, for HDMI for example `hdmi:CARD=vc4hdmi0,DEV=0` |
| Lead of the copy (ms) | 180 | 0..2000; wled-audiosync uses the same value |
| Send audio sync to WLED | on | run wled-audiosync |
| Target address | 239.0.0.1 | WLED's multicast group or the IPv4 address of one controller |
| Gain | 1.0 | gain of wled-audiosync |

The default slave `hdmi_bmc` comes from service.rpi-gpio-spdif; its setting `extra_ms` (default 80)
keeps enough audio queued behind the plugin for a lead of 180 ms. Without it the copy would be
dropped now and then, see the
[rpi-gpio-spdif README](https://github.com/ehb/rpi-gpio-spdif#setting-extra_ms).

Invalid values fall back to the default (the journal says so). Changing a setting restarts
the unit, which rewrites the configuration; a device that is playing keeps the old setting
until Kodi opens it again (next playback). **This includes the lead:** the running plugin
keeps the old lead until the next playback starts, while wled-audiosync is restarted with the
new lead at once, so the light is off by the difference until then. Change the lead while
nothing is playing. If the unit fails (plugin missing, a configuration file of the same name
that this add-on did not write, ...) or WLED sync cannot run (wled-audiosync missing, not
executable or rejecting its arguments), Kodi shows the reason as a notification after
start-up; the reason stays in `/run/service.alsa-leadtap.status` until the next start or a
clean stop. Whether a change needs a restart is decided by comparing the effective settings
with the applied ones, so a change made in the first seconds after Kodi starts is not lost.

**ALSA configuration.** The unit writes `/var/lib/alsa/conf.d/60-alsa-leadtap.conf`.
LibreELEC's `alsa.conf` reads every `*.conf` of that directory (a tmpfs) before
`/run/asound.conf` and the user's `/storage/.config/asound.conf`, so your own file can still
override it. Add-ons that need ALSA configuration should use their own file name there (the
rpi-gpio-spdif add-on uses `50-rpi-gpio-spdif.conf`). Stopping or removing the add-on deletes
the file. Do not also keep a hand-written `hdmi_leadtap` or `pcm_type.leadtap` in
`/storage/.config/asound.conf`.

**Snapcast.** The Snapcast server is not part of the add-on; LibreELEC has its own
(`service.snapserver`). Give it the pipe as a stream source:

```
source = pipe:///tmp/leadtap.fifo?name=Kodi&sampleformat=48000:16:2&mode=create
```

(In the snapserver configuration; check where the LibreELEC add-on keeps it. The
add-on's own Kodi/PulseAudio source is not the one to use: it knows nothing of the early
copy.) Set the client buffer to match your network.

**Updates.** Kodi loads the plugin when it first opens the device; after updating the add-on,
restart Kodi once to get the new plugin code.

`libreelec/test_addon.sh` tests the start script with a fake add-on root against native
builds (settings mapping, generated configuration through alsa-lib, supervision of
wled-audiosync, cleanup, the build script's architecture check); it needs Linux with
`aplay` and is run by hand, not in CI.

## Status

Early. Besides the selftest against a simulated slave (see above) it has run on one
machine, a Raspberry Pi 4B with LibreELEC 12.2.1 (alsa-lib 1.2.11) and Kodi 21:

- In front of an S/PDIF sound card whose driver has its own DMA ring beyond the ALSA buffer
  (the kernel driver of rpi-gpio-spdif 0.1.0, as before the switch to the plugin):
  internet radio at 44.1 kHz, pause, station change, rate changes, switching to and from
  AC3 passthrough (which Kodi sends to the card directly, on its separate passthrough
  device). Listened to and found in sync.
- A second room through Snapcast (buffer 130 ms, client on Wi-Fi), measured with a
  microphone against the copy: the other room plays between 12 and 25 ms early over
  several runs, with no drift over two hours of radio.
- In front of an HDMI output with `aplay` (silence, 44.1 and 48 kHz): opens, plays for the
  right time, complete copy, no gaps. The lead against the sound actually leaving HDMI
  was not measured, and a TV adds delay no driver reports.

- In front of the device `hdmi_bmc` of rpi-gpio-spdif 0.2.0 (the default slave, `extra_ms` 80):
  radio and video in Kodi, a video with flash and click stayed lip-synchronous by eye, the second
  room measured 20 to 24 ms early, no dropouts in the copy.

Not tested: USB and `dmix` slaves, Kodi on HDMI, other alsa-lib versions than 1.2.8 (selftest)
and 1.2.11, runs longer than about two hours.

## License

LGPL-2.1-or-later (the usual license for alsa-lib plugins), see `LICENSE`.
