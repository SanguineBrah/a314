# A314 settings (Prefs/A314)

The A314 drawer in Prefs holds the tools that change A314 settings from the
Workbench, without the shell. Settings that belong to one service are
described with that service; this directory has the drawer's icon and the
tools for the services that came with the A314 software.

## Disks

*Disks* shows and changes the disk images (ADF and HDF files on the Pi) of
[a314disk.device](../disk), the drives PD0: to PD3:, and of
[a314scsi.device](../scsi).

- **Drives:** Change... chooses an image on the Pi and puts it in the drive at
  once (tick Writable for writing to it), and Eject takes it out. Tick "At
  start" to have the drive get that image whenever the Amiga starts.
- **Hard disks:** Change... and Remove choose a SCSI unit's image, and
  Writable whether it can be written to. These are used from the next start
  of the Amiga, so Save them and restart. Take care with the image that the
  Amiga starts from.
- **Save, Use, Cancel:** as with the Workbench's preferences editors, Save
  keeps the start-up images and hard disks, Use keeps the drives as they
  are now without saving, and Cancel puts the drives back as they were.

The images are chosen from the files in the home directory of the Pi user
that runs the A314 software (for example in `a314shared/adfs` and
`a314shared/hdfs`). The drives can only be used once PD0: is mounted (see
the [disk README](../disk/README.md)).

## The a314prefs service

The tools talk to the `a314prefs` service on the Pi
([a314prefs.py](a314prefs.py), which describes the messages). It reads and
changes `/etc/opt/a314/disk.conf` and `/etc/opt/a314/scsi.conf`, which
`install-pi.sh` gives to the A314 user so that it can, and inserts and
ejects images through the disk service.

If `install-pi.sh` was run before this service existed, add this line to
`/etc/opt/a314/a314d.conf` and restart a314d:

```
a314prefs	/opt/a314/venv/bin/python3	/opt/a314/a314prefs.py
```

and give the two files to the A314 user:

```
sudo chown $USER: /etc/opt/a314/disk.conf /etc/opt/a314/scsi.conf
```
