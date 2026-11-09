# Changelog

## 0.1.0

First public version.

- ALSA PCM plugin `leadtap`: passes audio unchanged to any slave PCM, reports the slave's
  delay, and writes a copy (48 kHz, S16_LE, stereo) to up to four named pipes a fixed time
  before the audio becomes audible.
- IEC 61937 bitstreams give a silent copy; each pipe is independent and never blocks the
  player.
- LibreELEC / Kodi service add-on `service.alsa-leadtap` (aarch64): the plugin with the
  wled-audiosync binary, settings for sound card, lead and WLED target, ALSA configuration
  as a drop-in file in `/var/lib/alsa/conf.d`.
- Selftest with a simulated slave, no audio hardware needed.
