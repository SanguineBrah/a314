# PiWiFi

PiWiFi (the WiFi tool in Prefs/A314) lets you choose the WiFi network the Raspberry Pi connects to, from
the Workbench. It shows the Pi's current connection and the networks the Pi
can see, and connects to a network or forgets a saved one.

## Requirements

- Raspberry Pi OS with NetworkManager, which is the default since Bookworm.
- The piwifi service, which `install-pi.sh` installs, with a polkit rule
  (`/etc/polkit-1/rules.d/50-a314piwifi.rules`) that lets the A314 user
  change the network settings.
- Kickstart/Workbench 1.3 or later on the Amiga.

## Using PiWiFi

Copy the Prefs/A314 drawer from `bin_amiga` (made by `make -f
Makefile-amiga`) to SYS:Prefs, open it and double-click WiFi. It asks the Pi for its connection and scans for networks,
which takes a few seconds. In the list, `»` marks the network in use, and
the networks the Pi has saved say "saved".

To connect, click a network (or type the name of a hidden network into the
NETWORK field), type the password if the network is secured and not saved,
and click CONNECT. A saved network needs no password; typing one replaces
the saved password once the network works. SCAN looks for
networks again, and FORGET deletes the selected saved network and its
password. The network in use can't be forgotten.

## Safety

Changing the network can cut off anything that reaches the Pi over WiFi,
including SSH. So that a mistake can't leave the Pi unreachable:

- Before changing anything, the service creates a NetworkManager
  checkpoint. If the new network doesn't work, the checkpoint is rolled
  back: the previous network is used again, and any connection profile
  created for the new network is deleted. NetworkManager also rolls it back
  by itself after two minutes if the service stops half way.
- The new network only counts as working when the Pi can ping its router
  and one of 1.1.1.1, 8.8.8.8 and 9.9.9.9 through the WiFi interface.
- A new network's profile is created with autoconnect turned off, and
  autoconnect is only turned on once the network works. So if the Pi loses
  power during a change, it doesn't come back up on the new network.
- A saved network is never changed. A new password for it gives a new
  profile, and the old profile is deleted when the new one works.
- The network in use, or a network while the Pi is connecting, can't be
  forgotten.

Connecting to a network on a different subnet still ends SSH sessions to
the Pi's old address, so reconnect to its new one. Networks with 802.1X
(enterprise) or WEP security are not supported.

Passwords are sent from the Amiga to the Pi in the clear and are shown
while they are typed. Use PiWiFi only where that is acceptable. On the Pi,
a password is given to `nmcli` in a temporary file that only the A314 user
can read, so it doesn't appear in the process list or the system log, and
is then kept in the network's profile like any other saved WiFi password.

## Updating an existing installation

If `install-pi.sh` was run before PiWiFi existed, install the polkit rule
(replace `pi` with the user a314d runs as):

```
sed s/##USER##/pi/ piwifi/50-a314piwifi.rules | sudo tee /etc/polkit-1/rules.d/50-a314piwifi.rules
```

Then add this line to `/etc/opt/a314/a314d.conf` and restart a314d (`sudo
systemctl restart a314d`):

```
piwifi		/opt/a314/venv/bin/python3	/opt/a314/piwifi.py
```

The service and the program exchange text lines over the a314 stream
`piwifi`. The commands are listed at the top of `piwifi.py`.
