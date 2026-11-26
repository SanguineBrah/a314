# PiAudio

PiAudio is a service that integrates with ALSA, the sound sub-system on the Raspberry Pi, and lets sound samples be played via Paula on the Amiga.

The *piaudio* program should run on the Amiga. It allocates two sounds channels, one left and one right. Using the following command the piaudio program can run in the background:
```
run piaudio >NIL:
```

PiAudio can also be used from the Workbench. *PiAudio.info* is a project icon that starts `C:PiAudio`; copy it to SYS:Tools (it doesn't need a file of its own) and double-click it to start PiAudio. Double-clicking it again asks whether to stop it. To start PiAudio with the Workbench, put the icon in the WBStartup drawer instead (Workbench 2.0 or later).

On Kickstart 2.04 or later with [AHI](https://aminet.net/package/driver/audio/ahiusr_4.18) installed, piaudio can play through AHI instead:
```
run piaudio AHI >NIL:
```
From the Workbench, change the icon's `(AHI)` ToolType to `AHI` (in the icon's Information window). Then piaudio doesn't take two Paula channels for itself, and its sound is mixed with other programs that play through AHI. It uses AHI's default unit (unit 0), as set in the AHI preferences. For PiAudio's sound to be mixed with other programs' sounds, give that unit more than one channel in the AHI preferences; with one channel, another program's sound takes the channel while it plays, and PiAudio is quiet meanwhile. Programs that use audio.device or the audio hardware directly still compete with AHI for the Paula channels.

piaudio uses the sound hardware (its two Paula channels, or the AHI unit) only while the Pi plays something. After two seconds of silence it lets go of it, so that other programs, such as a module player, can play; when the Pi plays something again, piaudio takes the hardware back if it is free. It never takes Paula channels from another program: if one is using them, the Pi's sound is left out until they are free, and a program that starts while the Pi is playing gets the channels and piaudio waits.

The file *.asoundrc* should be stored in `/home/pi` on the RPi. Most programs that play audio on the RPi can then be used. One such program is mpg123, which is started as:
```
mpg123 -a amiga song.mp3
```
