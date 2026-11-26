# MHI decoder (mhia314.library)

*mhia314.library* is an MHI driver: it lets the Raspberry Pi decode MPEG
audio (MP1, MP2 and MP3) for Amiga players that support MHI, such as
AmigaAMP. The Amiga does no decoding at all, so MP3s play even on a 68000.

MHI is the standard interface for MPEG audio decoders on the Amiga; see
the MHI developer archive on Aminet (driver/audio/mhi_dev), by Paul Qureshi
and Thomas Wenzel. This driver supports every MHI call, including pause,
stop, the end of a stream, volume, panning, cross-mixing, bass/mid/treble
and the 5- and 10-band equalizers.

The decoded sound is played through AHI, on whatever AHI device is set up:
a sound card, or the Amiga's own Paula. Without AHI (on Kickstart 1.3, for
example) it is played through PiAudio, which must then be running on the
Amiga; see the [PiAudio README](../piaudio/README.md).

The output can be chosen with the environment variable `A314/MHI`, e.g.
```
SetEnv SAVE A314/MHI "OUTPUT=AHI UNIT=0 RATE=44100 BITS=16"
```
(`SAVE` keeps it in ENVARC: for the next start; `MakeDir ENVARC:A314` first
if that drawer isn't there.)

| Keyword | Values | Default |
|---|---|---|
| OUTPUT | `AUTO` (AHI if it is installed, else PiAudio), `AHI` or `PIAUDIO` | AUTO |
| UNIT | the ahi.device unit, 0 to 3 (set up in the AHI preferences) | 0 |
| RATE | the rate the Pi decodes to for AHI, 8000 to 48000 | 44100 |
| BITS | 16 or 8, for AHI | 16 |

On a 68000, AHI's mixing for Paula takes much of the CPU: use
`OUTPUT=PIAUDIO`, or a lower RATE and `BITS=8`.

## Installing

On the Pi, `install-pi.sh` installs the `mhi` service. It uses libmpg123
(`sudo apt install libmpg123-0`, normally already there).

On the Amiga, copy `mhia314.library` from `bin_amiga/Libs/MHI` (made by
`make -f Makefile-amiga`) to LIBS:MHI/. In AmigaAMP's preferences, choose
the MHI engine and then this driver.

## Notes

- One stream can be played at a time. Through PiAudio, PiRadio and the
  MHI driver take turns.
- Volume, tone and pause take effect after about a fifth of a second (the
  audio already on its way).
- Through PiAudio, the output is what PiAudio plays: 8-bit stereo at
  18 kHz.

## How it works

The player queues buffers of MPEG data. The driver copies them into a ring
buffer in A314 memory, and the `mhi` service on the Pi
([mhi.py](mhi.py), which describes the messages) reads them from there as
its decoder needs them. A buffer is given back to the player when the Pi
has taken all of it, so the player is paced by the music, as with a
hardware decoder. The decoder is libmpg123.

For AHI, the driver gives the Pi four slots in A314 memory. The Pi writes
a block of decoded audio into a free slot, and the driver's worker process
plays it through ahi.device (two blocks at a time, the second linked to the
first, so that there is no gap) and gives the slot back once it has been
played, which paces the stream. Through PiAudio, the decoded audio goes
into PiAudio's pipe, whose pace (the Amiga's audio hardware) sets the
speed.
