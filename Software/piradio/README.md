# PiRadio

PiRadio turns an RTL-SDR dongle plugged into the Raspberry Pi into a radio
that you tune from the Workbench. The Amiga program opens a window with a
tuning dial, band buttons, and sliders for volume, squelch and RF gain. The
sound is played on the Amiga by [PiAudio](../piaudio/README.md).

## Requirements

On the Raspberry Pi:

- An RTL-SDR dongle and the `rtl-sdr` tools: `sudo apt install rtl-sdr`
- The piradio and piaudio services, which `install-pi.sh` installs.

On the Amiga:

- Kickstart/Workbench 1.3 or later.
- PiAudio, running at the same time as PiRadio.

## Running

```
run >NIL: PiAudio
PiRadio
```

PiRadio opens its window straight away and connects to the Pi in the
background, retrying until the service answers. The radio starts on the band
and frequency used last time. Close the window, or press Esc or Q, to quit;
the Pi then stops `rtl_fm`.

While PiRadio is running, the piradio service has the piaudio pipe open, so
other programs on the Pi should not play sound to the Amiga at the same
time.

## Controls

- Click or drag on the dial to tune. The marks under the scale are the
  active channels found by the last sweep.
- The band buttons select a band. The line under a button shows the current
  band.
- SEEK goes to the next active channel down or up. SCAN steps through the
  active channels until it is pressed again. With the squelch off, it stays
  on each channel for `scan_dwell_seconds`. With the squelch on, it stays
  while there is a signal, and moves on after `scan_hang_seconds` of quiet.
- SWEEP looks for active channels again. Seek and scan sweep the band with
  `rtl_power` when there is no recent sweep, and scan sweeps again each
  time it reaches the end of the band. Audio stops for the second or two a
  sweep takes, and only stations transmitting during the sweep are found.
- The lamps show TUNER (lit when receiving, flashing while starting or
  sweeping), SIGNAL (squelch open), AUDIO (the piaudio pipe is open on the
  Pi) and SCAN.

| Key                 | Action                                         |
|---------------------|------------------------------------------------|
| Cursor Left / Right | Tune one channel down/up (Shift: 10, Alt: 100) |
| Cursor Up / Down    | Seek up/down                                   |
| Space (Shift+Space) | Start scanning up (down), or stop              |
| R                   | Sweep the band                                 |
| F1 - F10            | Select band                                    |
| 0-9 . Return        | Type a frequency in MHz, e.g. `121.5` Return   |
| v / V               | Volume down/up                                 |
| s / S               | Squelch down/up                                |
| g / G, a            | RF gain down/up, automatic gain                |
| Esc or Q            | Quit (Esc first cancels a frequency entry)     |

## Configuration

The bands and other settings are in `/etc/opt/a314/piradio.conf`, a JSON
file that `install-pi.sh` creates from [piradio.conf](piradio.conf). The
bands in it are examples: change them to suit your country, and check what
you are allowed to listen to where you live.

Each band has `short` (button label, up to 6 characters), `name`, `lo` and
`hi` (band edges in Hz), `step` (channel spacing in Hz) and `mode` (`wbfm`,
`fm`, `am`, `usb`, `lsb` or `raw`). Optional fields are `squelch` (default
squelch level), `rate` (the demodulation sample rate, rounded up to a
multiple of 18000 Hz; the default is 180000 for `wbfm` and 18000 otherwise)
and `args` (extra `rtl_fm` arguments, e.g. `["-E", "direct2"]`). Up to 10
bands are used.

The other settings are `ppm` (frequency correction), `device_index` (which
dongle), `gains` (the RF gain steps in tenths of a dB, 0 or more), the paths to
`rtl_fm`, `rtl_power` and the piaudio pipe, `sweep_threshold_db` (how far
above the noise a channel must be to count as active), and the two scan
times. Entries that can't be used are logged and skipped.

The service reads the file when it starts, so restart a314d after editing
it (`sudo systemctl restart a314d`). The last band, frequencies, squelch,
gain and volume are kept in `~/.config/piradio/state.json` on the Pi.

If `install-pi.sh` was run before PiRadio existed, add this line to
`/etc/opt/a314/a314d.conf` and restart a314d:

```
piradio		/opt/a314/venv/bin/python3	/opt/a314/piradio.py
```

## Problems

Messages from the service are shown at the bottom of the window, for
example when no dongle is found or the dongle is in use by another program.
The service retries every few seconds, so plugging the dongle in is enough.
The service logs to the a314d journal: `journalctl -u a314d`.

The service and the Amiga program exchange text lines over the a314 stream
`piradio`. The commands are listed at the top of `piradio.py`.
