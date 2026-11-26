#!/usr/bin/python3
# -*- coding: utf-8 -*-

# A314 prefs service: lets the tools in Prefs/A314 on the Amiga see and
# change settings that are kept on the Raspberry Pi, so that they can be
# changed from the Workbench. For now: the disk images of a314disk.device
# (drives PD0: to PD3:) and a314scsi.device.
#
# - Disk images are chosen from the files in the A314 user's home
#   directory on the Pi (nothing outside it is shown or used).
# - Inserting and ejecting goes to the disk service's control port, so it
#   happens at once. The images inserted when the Amiga starts are in
#   /etc/opt/a314/disk.conf ("auto-insert").
# - The SCSI units are in /etc/opt/a314/scsi.conf, which is read when the
#   Amiga starts, so changes to them take effect then.
#
# File names are sent as hex, as they can contain any characters, and
# relative to the home directory.
#
# Amiga -> Pi (one line a packet):
#   FILES <dir>             list a directory: F lines, then END
#   DISKS                   the drives and their start-up images: D and A
#                           lines, then END
#   INSERT <unit> <rw> <file>   put an image in a drive now (rw 1: writable)
#   EJECT <unit>            take it out
#   AUTO <unit> <rw> <file or ->   the image put in a drive when the Amiga
#                           starts (- for none)
#   SCSI                    the SCSI units: S lines, then END
#   SCSISET <unit> <rw> <file or ->   the image of a SCSI unit (- for none),
#                           from the next start of the Amiga
#
# Pi -> Amiga:
#   F <d or f> <size> <name>    a directory or a file (only disk images)
#   D <unit> <rw> <file or ->   what a drive has now (rw - if empty)
#   A <unit> <rw> <file>        what a drive gets at start
#   S <unit> <rw> <file>        a SCSI unit's image
#   END                     the end of a list
#   OK <text>, ERR <text>   the answer to INSERT, EJECT, AUTO and SCSISET

import json
import logging
import os
import socket

from a314d import A314d

logging.basicConfig(format = '%(levelname)s, %(asctime)s, %(name)s, line %(lineno)d: %(message)s')
logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

SERVICE_NAME = 'a314prefs'

HOME = os.path.realpath(os.path.expanduser('~'))
DISK_CONF = os.environ.get('A314PREFS_DISK_CONF', '/etc/opt/a314/disk.conf')
SCSI_CONF = os.environ.get('A314PREFS_SCSI_CONF', '/etc/opt/a314/scsi.conf')
DISK_CONTROL = ('localhost', int(os.environ.get('A314PREFS_DISK_PORT', '23890')))
NUM_DRIVES = 4
NUM_SCSI = 7
IMAGE_EXTS = ('.adf', '.hdf')
MAX_LINE = 251
MAX_FILES = 500


def to_hex(text):
    return text.encode('utf-8', 'replace').hex() or '-'


def from_hex(word):
    if word == '-':
        return ''
    return bytes.fromhex(word).decode('utf-8', 'replace')


def inside_home(rel):
    # The full path of rel (relative to the home directory), or None if it
    # would be outside it
    path = os.path.realpath(os.path.join(HOME, rel))
    if path != HOME and not path.startswith(HOME + os.sep):
        return None
    return path


def relative(path):
    path = os.path.realpath(path)
    if path.startswith(HOME + os.sep):
        return path[len(HOME) + 1:]
    return path


def disk_control(command):
    # A command to the disk service; returns its answer, or None if the
    # disk service isn't running (it starts when the Amiga uses the drives)
    try:
        with socket.create_connection(DISK_CONTROL, timeout=5) as s:
            s.sendall(command.encode('utf-8') + b'\n')
            out = b''
            while True:
                d = s.recv(4096)
                if not d:
                    break
                out += d
        return out.decode('utf-8', 'replace')
    except OSError:
        return None


class ConfigError(Exception):
    pass


def read_json(path):
    # {} if there is no file; ConfigError if there is one that can't be
    # read, so that it is never replaced by one that has lost its contents
    try:
        with open(path) as f:
            j = json.load(f)
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as e:
        raise ConfigError('%s can\'t be read (%s)' % (path, e))
    if not isinstance(j, dict):
        raise ConfigError('%s is not as expected' % path)
    return j


def entries(j, key):
    # The list of dicts under key (anything else in it is left out)
    v = j.get(key, [])
    return [e for e in v if isinstance(e, dict)] if isinstance(v, list) else []


def write_json(path, j):
    # Written in place: the directory (/etc/opt/a314) isn't writable, only
    # the file. A copy of the old file is kept in ~/.config/a314, and the new
    # text is padded with spaces to the old length before the file is made
    # shorter, so that if the power goes in between the file is still valid
    # JSON.
    data = json.dumps(j, indent=4) + '\n'
    old = b''
    if os.path.exists(path):
        with open(path, 'rb') as f:
            old = f.read()
        backup = os.path.join(HOME, '.config', 'a314')
        os.makedirs(backup, exist_ok=True)
        with open(os.path.join(backup, os.path.basename(path) + '.bak'), 'wb') as f:
            f.write(old)
    new = data.encode()
    with open(path, 'r+b' if old else 'wb') as f:
        f.write(new + b' ' * max(0, len(old) - len(new)))
        f.flush()
        os.fsync(f.fileno())
        if len(old) > len(new):
            f.truncate(len(new))
            f.flush()
            os.fsync(f.fileno())


class Session(object):
    def __init__(self, service, stream_id):
        self.service = service
        self.stream_id = stream_id

    def send(self, line):
        data = line.encode('latin-1', 'replace')
        if len(data) > MAX_LINE:
            data = data[:MAX_LINE]
        self.service.a314d.send_data(self.stream_id, data)

    def command(self, line):
        words = line.split()
        if not words:
            return
        cmd, args = words[0], words[1:]
        try:
            handler = getattr(self, 'cmd_' + cmd.lower(), None)
            if handler is None:
                self.send('ERR unknown command')
            else:
                handler(args)
        except (ValueError, IndexError):
            self.send('ERR bad command')
        except ConfigError as e:
            self.send('ERR %s' % e)
        except OSError as e:
            self.send('ERR %s' % (e.strerror or e))
        except Exception as e:
            # Never the service: an odd config or command fails on its own
            logger.warning('%r failed: %s', line[:40], e)
            self.send('ERR %s' % e)

    def image_arg(self, word):
        # A file name from the Amiga: an image inside the home directory, or
        # one already in use (in a drive or a config), wherever it is, so
        # that Cancel can put it back and its settings can be changed
        rel = from_hex(word)
        path = inside_home(rel)
        if path is None:
            if os.path.isabs(rel) and os.path.realpath(rel) in self.images_in_use():
                path = os.path.realpath(rel)
            else:
                return None
        if not os.path.isfile(path) or not path.lower().endswith(IMAGE_EXTS):
            return None
        return path

    def images_in_use(self):
        used = set()
        for key, conf in (('auto-insert', DISK_CONF), ('units', SCSI_CONF)):
            try:
                for e in entries(read_json(conf), key):
                    if isinstance(e.get('filename'), str) and e['filename']:
                        used.add(os.path.realpath(e['filename']))
            except ConfigError:
                pass
        for unit, (rw, name) in self.drive_status().items():
            if name:
                used.add(os.path.realpath(name))
        return used

    def drive_status(self):
        # {unit: (rw, file)} from the disk service, or None if it isn't
        # running (or is too old to answer "status")
        status = disk_control('status')
        if status is None or not status[:1].isdigit():
            return None if status is None or 'Unknown' in status else {}
        now = {}
        for line in status.splitlines():
            parts = line.split(' ', 2)
            if len(parts) >= 2 and parts[0].isdigit():
                now[int(parts[0])] = (parts[1], parts[2] if len(parts) > 2 else '')
        return now

    def unit_arg(self, word, count):
        unit = int(word)
        if not 0 <= unit < count:
            raise ValueError
        return unit

    def cmd_files(self, args):
        rel = from_hex(args[0]) if args else ''
        path = inside_home(rel)
        if path is None or not os.path.isdir(path):
            self.send('ERR no such directory')
            return
        found = []
        for name in sorted(os.listdir(path), key=str.lower):
            if name.startswith('.'):
                continue
            full = os.path.join(path, name)
            # Nothing that leads outside the home directory (a symlink)
            if inside_home(os.path.relpath(full, HOME)) is None:
                continue
            try:
                if os.path.isdir(full):
                    found.append('F d 0 %s' % to_hex(name))
                elif name.lower().endswith(IMAGE_EXTS):
                    found.append('F f %d %s' % (os.path.getsize(full), to_hex(name)))
            except OSError:
                continue
            if len(found) >= MAX_FILES:
                break
        for e in found:
            if len(e) <= MAX_LINE:      # very long names are left out
                self.send(e)
        self.send('END')

    def cmd_disks(self, args):
        now = self.drive_status()
        for unit in range(NUM_DRIVES):
            rw, name = (now or {}).get(unit, ('-', ''))
            self.send('D %d %s %s' % (unit, rw if now is not None else '?', to_hex(relative(name)) if name else '-'))
        for e in entries(read_json(DISK_CONF), 'auto-insert'):
            if isinstance(e.get('filename'), str) and e['filename'] and isinstance(e.get('unit'), int):
                self.send('A %d %d %s' % (e['unit'], 1 if e.get('rw') else 0, to_hex(relative(e['filename']))))
        self.send('END')

    def cmd_insert(self, args):
        unit = self.unit_arg(args[0], NUM_DRIVES)
        rw = args[1] == '1'
        path = self.image_arg(args[2])
        if not path:
            self.send('ERR not a disk image')
            return
        res = disk_control('insert %d %s%s' % (unit, '-rw ' if rw else '', path))
        self.answer(res)

    def cmd_eject(self, args):
        unit = self.unit_arg(args[0], NUM_DRIVES)
        self.answer(disk_control('eject %d' % unit))

    def answer(self, res):
        if res is not None and res.startswith('Unknown command'):
            self.send('ERR the disk service is too old for this')
        elif res is None:
            self.send('ERR the disk service is not running (mount a PD drive first)')
        elif res.startswith(('Inserted', 'Ejected', 'No disk')):
            self.send('OK ' + res.strip())
        else:
            self.send('ERR ' + res.strip())

    def cmd_auto(self, args):
        unit = self.unit_arg(args[0], NUM_DRIVES)
        rw = args[1] == '1'
        path = None
        if args[2] != '-':
            path = self.image_arg(args[2])
            if not path:
                self.send('ERR not a disk image')
                return
        j = read_json(DISK_CONF)
        keep = [e for e in entries(j, 'auto-insert') if e.get('unit') != unit]
        if path:
            keep.append({'unit': unit, 'filename': path, 'rw': rw})
        j['auto-insert'] = sorted(keep, key=lambda e: str(e.get('unit')))
        write_json(DISK_CONF, j)
        self.send('OK saved')

    def cmd_scsi(self, args):
        for e in entries(read_json(SCSI_CONF), 'units'):
            if isinstance(e.get('filename'), str) and e['filename'] and isinstance(e.get('unit'), int):
                self.send('S %d %d %s' % (e['unit'], 1 if e.get('rw') else 0, to_hex(relative(e['filename']))))
        self.send('END')

    def cmd_scsiset(self, args):
        unit = self.unit_arg(args[0], NUM_SCSI)
        rw = args[1] == '1'
        path = None
        if args[2] != '-':
            path = self.image_arg(args[2])
            if not path:
                self.send('ERR not a disk image')
                return
        j = read_json(SCSI_CONF)
        old = entries(j, 'units')
        keep = [e for e in old if e.get('unit') != unit]
        if path:
            # The unit's entry is updated, keeping its other settings (such
            # as the block size)
            e = next((dict(e) for e in old if e.get('unit') == unit), {'unit': unit, 'block_size': 512})
            e.update({'filename': path, 'rw': rw})
            keep.append(e)
        j['units'] = sorted(keep, key=lambda e: str(e.get('unit')))
        write_json(SCSI_CONF, j)
        self.send('OK saved')


class PrefsService(object):
    def __init__(self):
        self.a314d = A314d(SERVICE_NAME)
        self.sessions = {}

    def run(self):
        while True:
            stream_id, ptype, payload = self.a314d.wait_for_msg()
            if ptype == A314d.MSG_CONNECT:
                if payload == SERVICE_NAME.encode():
                    self.sessions[stream_id] = Session(self, stream_id)
                    self.a314d.send_connect_response(stream_id, 0)
                else:
                    self.a314d.send_connect_response(stream_id, 3)
            elif ptype == A314d.MSG_DATA:
                s = self.sessions.get(stream_id)
                if s:
                    s.command(payload.decode('latin-1'))
            elif ptype in (A314d.MSG_EOS, A314d.MSG_RESET):
                if ptype == A314d.MSG_EOS:
                    self.a314d.send_eos(stream_id)
                self.sessions.pop(stream_id, None)


if __name__ == '__main__':
    service = PrefsService()
    logger.info('%s service is running', SERVICE_NAME)
    try:
        service.run()
    except KeyboardInterrupt:
        pass
    service.a314d.close()
