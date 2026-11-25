# PiAudio

PiAudio is a service that integrates with ALSA, the sound sub-system on the Raspberry Pi, and lets sound samples be played via Paula on the Amiga.

The *piaudio* program should run on the Amiga. It allocates two sounds channels, one left and one right. Using the following command the piaudio program can run in the background:
```
run piaudio >NIL:
```

On Kickstart 2.04 or later with [AHI](https://aminet.net/package/driver/audio/ahiusr_4.18) installed, piaudio can play through AHI instead:
```
run piaudio AHI >NIL:
```
Then piaudio doesn't take two Paula channels for itself, and its sound is mixed with other programs that play through AHI. It uses AHI's default unit (unit 0), as set in the AHI preferences. Programs that use audio.device or the audio hardware directly still compete with AHI for the Paula channels.

The file *.asoundrc* should be stored in `/home/pi` on the RPi. Most programs that play audio on the RPi can then be used. One such program is mpg123, which is started as:
```
mpg123 -a amiga song.mp3
```
