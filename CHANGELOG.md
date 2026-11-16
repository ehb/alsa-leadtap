# Changelog

## 0.1.2

- The plugin now offers exactly the sample rates the slave supports. A slave with fixed rates
  (a sound card, the S/PDIF device of service.rpi-gpio-spdif) used to be offered as a
  continuous range, so an application asking for the nearest rate got a rate the slave then
  refused. In Kodi this hit games: a SNES core plays at 32040 Hz, the device failed to open and
  Kodi fell back silently to another output. Now Kodi gets 32000 Hz and resamples itself. A
  slave that takes any rate in a range (e.g. behind `plug`) keeps offering the range.

## 0.1.1

- The add-on's default sound card (slave) is now `hdmi_bmc`, the optical S/PDIF device
  provided by the add-on service.rpi-gpio-spdif (an ALSA plugin, no kernel driver), instead
  of the card `iec958:CARD=SPDIFout,DEV=0` of the old rpi-gpio-spdif kernel driver. With
  HDMI or another sound card the setting must be changed, as before.

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
