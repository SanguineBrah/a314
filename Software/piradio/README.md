# PiRadio

PiRadio turns an RTL-SDR dongle plugged into the Raspberry Pi into a radio
that you tune from the Amiga. The Amiga program shows a radio set on its own
screen, with a tuning dial, a frequency display, a level meter, band keys,
and knobs for volume, squelch and RF gain. With the WINDOW ToolType it opens
in a window on the Workbench instead. The sound is played on the Amiga by
[PiAudio](../piaudio/README.md).

## Requirements

On the Raspberry Pi:

- An RTL-SDR dongle and the `rtl-sdr` tools: `sudo apt install rtl-sdr`
- The piradio and piaudio services, which `install-pi.sh` installs.

On the Amiga:

- Kickstart/Workbench 1.3 or later.
- PiAudio, running at the same time as PiRadio.

## Running

Copy *PiRadio* and *PiRadio.info* to SYS:Utilities (`make -f Makefile-amiga`
puts them in `bin_amiga/Utilities`) and double-click the icon.
PiAudio has to be running as well; it can also be started from its icon
(see the [PiAudio README](../piaudio/README.md)). From the shell:

```
run >NIL: PiAudio
PiRadio
```

PiRadio opens straight away and connects to the Pi in the background,
retrying until the service answers. The radio starts on the band and
frequency used last time. Press POWER (or close the window), or press Esc
or Q, to quit; the Pi then stops `rtl_fm`. TO WB (or W) puts the radio's
screen behind the Workbench, and it carries on playing; bring it back with
the screen's depth gadget or Left-Amiga+M.

PiRadio needs about 210 KB of chip memory for its screen and pictures. To
have it in a window on the Workbench instead, switch on the ToolType
`(WINDOW)` in its icon: select the icon, choose Information from the Icons
menu, and take away the brackets. From the shell: `PiRadio WINDOW`.

While PiRadio is running, the piradio service has the piaudio pipe open, so
other programs on the Pi should not play sound to the Amiga at the same
time.

## Controls

- Click or drag on the dial to tune, or turn the TUNING knob: drag it
  round, or click on its left or right side for one step. The marks under
  the scale are the active channels found by the last sweep.
- The band keys select a band (in the window: the Band cycle gadget).
- Drag the VOL, SQL and GAIN knobs up or down (in the window: the sliders).
- SEEK goes to the next active channel down or up. SCAN steps through the
  active channels until it is pressed again. With the squelch off, it stays
  on each channel for `scan_dwell_seconds`. With the squelch on, it stays
  while there is a signal, and moves on after `scan_hang_seconds` of quiet.
- REGION steps to the next region's band list (see Configuration). The
  region is shown in the message strip (in the window: the Region cycle
  gadget, which also chooses a region directly).
- HELP shows the keyboard shortcuts on the dial.
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
| Help or ?           | Show or hide the keyboard shortcuts            |
| W                   | Put the radio's screen behind the Workbench    |
| Esc or Q            | Quit (Esc first cancels a frequency entry)     |

## Configuration

PiRadio comes with band lists for several regions: UK and Europe, North
America, Australia and NZ, and Japan. Click REGION to step through them.
The region is remembered, and so is the band and frequency you were using
in each region. Check what you are allowed to listen to where you live.

The regions and other settings are in `/etc/opt/a314/piradio.conf`, a JSON
file that `install-pi.sh` creates from [piradio.conf](piradio.conf), so
other regions or bands can be added there. `"regions"` maps each region's
name to its list of bands; a plain `"bands"` list instead is used as a
single region.

Each band has `short` (button label, up to 6 characters), `name`, `lo` and
`hi` (band edges in Hz), `step` (channel spacing in Hz) and `mode` (`wbfm`,
`fm`, `am`, `usb`, `lsb` or `raw`). Optional fields are `squelch` (default
squelch level), `rate` (the demodulation sample rate, rounded up to a
multiple of 18000 Hz; the default is 180000 for `wbfm` and 18000 otherwise)
and `args` (extra `rtl_fm` arguments, e.g. `["-E", "direct2"]`). Up to 10
regions, each with up to 10 bands, are used.

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

Messages from the service are shown in the message strip under the dial,
or at the bottom of the window, for
example when no dongle is found or the dongle is in use by another program.
The service retries every few seconds, so plugging the dongle in is enough.
The service logs to the a314d journal: `journalctl -u a314d`.

The service and the Amiga program exchange text lines over the a314 stream
`piradio`. The commands are listed at the top of `piradio.py`.

The Amiga program is in four parts: `piradio.c` (the connection to the Pi
and the actions), `panel.c` (the radio on its own screen), `window.c` (the
radio in a window) and `piradio_art.c`, the pictures for the screen.
`piradio_art.c` and `piradio_art.h` are made by `python3 make_art.py`, which
draws them; give it the name of a PNG file to get a preview as well.
