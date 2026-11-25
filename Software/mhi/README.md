# MHI decoder (mhia314.library)

*mhia314.library* is an MHI driver: it lets the Raspberry Pi decode MPEG
audio (MP1, MP2 and MP3) for Amiga players that support MHI, such as
AmigaAMP. The Amiga does no decoding at all, so MP3s play even on a 68000.

MHI is the standard interface for MPEG audio decoders on the Amiga; see
the MHI developer archive on Aminet (driver/audio/mhi_dev), by Paul Qureshi
and Thomas Wenzel. This driver supports every MHI call, including pause,
stop, the end of a stream, volume, panning, cross-mixing, bass/mid/treble
and the 5- and 10-band equalizers.

The decoded sound is played through PiAudio, so PiAudio must be running
on the Amiga. With PiAudio's AHI option it shares the sound hardware with
the player and other programs; see the [PiAudio README](../piaudio/README.md).
Playing it through AHI directly, without PiAudio, is planned.

## Installing

On the Pi, `install-pi.sh` installs the `mhi` service. It uses libmpg123
(`sudo apt install libmpg123-0`, normally already there).

On the Amiga, copy `mhia314.library` from `bin_amiga/Libs/MHI` (made by
`make -f Makefile-amiga`) to LIBS:MHI/. In AmigaAMP's preferences, choose
the MHI engine and then this driver.

## Notes

- One stream can be played at a time, as there is one PiAudio stream.
  PiRadio and the MHI driver take turns.
- Volume, tone and pause take effect after about a fifth of a second (the
  audio already on its way through PiAudio).
- The output is what PiAudio plays: 8-bit stereo at 18 kHz.

## How it works

The player queues buffers of MPEG data. The driver copies them into a ring
buffer in A314 memory, and the `mhi` service on the Pi
([mhi.py](mhi.py), which describes the messages) reads them from there as
its decoder needs them. A buffer is given back to the player when the Pi
has taken all of it, so the player is paced by the music, as with a
hardware decoder. The decoder is libmpg123; the decoded audio goes into
PiAudio's pipe, whose pace (the Amiga's audio hardware) sets the speed.
