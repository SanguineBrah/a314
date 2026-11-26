# PiSerial

PiSerial lets the Amiga use serial ports on the Raspberry Pi, such as USB
serial adapters and boards with a USB serial port. On the Amiga they are
units of *a314serial.device*, which works like serial.device, so any
program that lets you choose the serial device can use them: terminal
programs such as NComm or Term, or programs that talk to a device over a
serial line.

There are four units, 0 to 3. Each unit is one Pi port, chosen with the
Serial tool in Prefs/A314. Until a unit has a port, it uses the first free
one, so with a single adapter nothing needs setting up. The port is
remembered by the name Linux gives it in `/dev/serial/by-id`, so it is
still found when it is plugged into another USB socket or the adapters are
plugged in in another order. A port that is
unplugged while in use is opened again when it comes back; until then the
unit reports that the carrier is gone.

## Installing

On the Pi, `install-pi.sh` installs the `piserial` service. The user that
runs a314d must be able to use serial ports (on Raspberry Pi OS the first
user is in the `dialout` group, which is enough).

On the Amiga, copy `a314serial.device` from `bin_amiga/Devs` (made by
`make -f Makefile-amiga`) to DEVS:, and the `Prefs/A314` drawer to SYS:Prefs.
Then, in your program, choose `a314serial.device` instead of
`serial.device`, and the unit number.

## Choosing the ports

Open the A314 drawer in Prefs and double-click Serial. It lists the units
and asks the Pi which serial ports it has. Click a unit to step through the
ports (and "Automatic", the first free port). A port that is in use is
marked as such; one that was chosen but isn't plugged in is marked as not
connected and keeps its place. Rescan asks the Pi again, for instance after
plugging in an adapter.

As with the Workbench's own preferences editors, Use keeps the settings
until the Amiga is restarted and Save keeps them for good. They are the
environment variables `A314/SerialUnit0` to `A314/SerialUnit3`, which hold
the port's name, or `auto`; Save also puts them in ENVARC: (on Workbench
1.3, which has no ENVARC:, in SYS:Prefs/Env-Archive, where
a314serial.device also looks). A unit that is open keeps its port until it
is closed.

## Settings

The baud rate, data bits, parity, stop bits and handshake are set by the
program in the usual way (SDCMD_SETPARAMS). Until it does, the defaults
come from the serial settings in Preferences, as with serial.device.
All the usual baud rates work, and odd ones such as MIDI's 31250 as well,
if the adapter supports them. XON/XOFF handshaking uses Ctrl-Q and Ctrl-S
(other characters in io_CtlChar are not used).

The modem lines (carrier, CTS, DSR) can be read with SDCMD_QUERY; DTR and
RTS can't be set.

## ModemManager

Raspberry Pi OS comes with ModemManager, which tries out serial devices
that might be mobile broadband modems by sending them AT commands as soon
as they are plugged in. That can upset other devices, particularly boards
with a native USB serial port (they appear as `/dev/ttyACM<n>`). If you
don't use a mobile broadband modem on the Pi, turn ModemManager off:

```
sudo systemctl disable --now ModemManager
sudo systemctl mask ModemManager
```

To keep ModemManager, tell it to leave a device alone instead, with a udev
rule such as this one in `/etc/udev/rules.d/99-piserial.rules` (with your
device's USB vendor and product id, from `lsusb`):

```
ATTRS{idVendor}=="239a", ATTRS{idProduct}=="8029", ENV{ID_MM_DEVICE_IGNORE}="1"
```

## How it works

Each open unit is a connection to the `piserial` service on the Pi
([piserial.py](piserial.py), which describes the messages). Data goes both
ways with credit-based flow control, so neither side can be flooded: the
Pi only sends as much as the Amiga has room for (io_RBufLen), and the
Amiga only sends as much as the Pi has room for. If the Amiga can't keep
up, data waits on the Pi and in the adapter, and with RTS/CTS handshaking
the device at the other end is held up as well.
