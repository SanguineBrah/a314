# USB memory sticks (USB0: to USB3:)

USB memory sticks plugged into the Raspberry Pi appear on the Amiga as
removable drives, USB0: to USB3:, which behave like floppy drives:

- When a stick is plugged in, its volume appears under its own name, with
  an icon on the Workbench.
- When it is pulled out, the volume goes. If a program still has files or
  drawers on it, AmigaDOS asks for the volume to be put back ("Please
  replace volume ... in any drive"), and carries on when it is.
- An empty drive says that no disk is present.
- A write-protected stick is shown as read only.

Sticks take the drives in the order they are plugged in, and a stick that
is pulled out and put back gets the drive it had. Any file system that the
Pi can mount can be used: FAT, exFAT, NTFS, ext4 and others. File names
are shown in the Amiga's character set (ISO-8859-1): characters it doesn't
have are shown as `?`, and `:` and `/` as `_`. Protection bits and comments
set on the Amiga are kept in a hidden file `.a314` in each directory of the
stick, written only when a file has any.

To make a stick safe to pull out, eject it in **Prefs/A314/Disks**, which
shows what is in each USB drive: the Pi then closes its files, unmounts it
and powers it off. A stick pulled out without ejecting is fine as long as
nothing was being written to it.

If the Pi's software is restarted, the drives are empty for a moment and
then come back by themselves.

## Installing

On the Pi, `install-pi.sh` installs the `a314usb` service, and a polkit rule
(`/etc/polkit-1/rules.d/50-a314usb.rules`) that lets the A314 user mount,
unmount and power off USB sticks through udisks2. If something else mounts
sticks on the Pi (a desktop, or udiskie), a314usb uses them where they are
mounted; it's best to have only one program doing it, though.

On the Amiga, copy `a314usb` from `bin_amiga/L` (made by
`make -f Makefile-amiga`) to `L:`.

- On Workbench 2.0 and later, copy `USB0` to `USB3` with their icons from
  `bin_amiga/Devs/DOSDrivers` to `DEVS:DOSDrivers`. The drives are then there
  from every start. To mount a drive just once, double-click its icon.
- On Kickstart 1.3, add the entries in `Mountlist` to `DEVS:MountList` and
  mount the drives with `Mount USB0:` (and so on), for example in
  `S:Startup-Sequence`.

## How it works

The Amiga side is a file system handler, `L:a314usb`, built from the
a314fs handler and using the same requests (`a314fs/messages.h`); one runs
for each drive. Once a second it asks the `a314usb` service on the Pi
([a314usb.py](a314usb.py), which describes the requests) what is in its
unit, and adds or removes the volume. The service finds sticks with lsblk
(when udev reports a change, and every few seconds), mounts them through
udisks2, and serves their files.
