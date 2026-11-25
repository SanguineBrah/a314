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


def read_json(path):
    try:
        with open(path) as f:
            j = json.load(f)
        return j if isinstance(j, dict) else {}
    except (OSError, ValueError):
        return {}


def write_json(path, j):
    # Written in place: the directory (/etc/opt/a314) isn't writable, only
    # the file. One write of a small file.
    data = json.dumps(j, indent=4) + '\n'
    with open(path, 'r+' if os.path.exists(path) else 'w') as f:
        f.write(data)
        f.truncate()
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
        except OSError as e:
            self.send('ERR %s' % (e.strerror or e))

    def image_arg(self, word):
        # A file name from the Amiga: inside the home directory, an image
        rel = from_hex(word)
        path = inside_home(rel)
        if path is None or not os.path.isfile(path) or not path.lower().endswith(IMAGE_EXTS):
            return None
        return path

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
        entries = []
        for name in sorted(os.listdir(path), key=str.lower)[:MAX_FILES * 4]:
            if name.startswith('.'):
                continue
            full = os.path.join(path, name)
            # Nothing that leads outside the home directory (a symlink)
            if inside_home(os.path.relpath(full, HOME)) is None:
                continue
            try:
                if os.path.isdir(full):
                    entries.append('F d 0 %s' % to_hex(name))
                elif name.lower().endswith(IMAGE_EXTS):
                    entries.append('F f %d %s' % (os.path.getsize(full), to_hex(name)))
            except OSError:
                continue
        for e in entries[:MAX_FILES]:
            if len(e) <= MAX_LINE:      # very long names are left out
                self.send(e)
        self.send('END')

    def cmd_disks(self, args):
        status = disk_control('status')
        now = {}
        if status:
            for line in status.splitlines():
                parts = line.split(' ', 2)
                if len(parts) >= 2 and parts[0].isdigit():
                    now[int(parts[0])] = (parts[1], parts[2] if len(parts) > 2 else '')
        for unit in range(NUM_DRIVES):
            rw, name = now.get(unit, ('-', ''))
            self.send('D %d %s %s' % (unit, rw if status else '?', to_hex(relative(name)) if name else '-'))
        for e in read_json(DISK_CONF).get('auto-insert', []):
            try:
                self.send('A %d %d %s' % (int(e['unit']), 1 if e.get('rw') else 0, to_hex(relative(e['filename']))))
            except (KeyError, ValueError, TypeError):
                continue
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
        if res is None:
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
        entries = [e for e in j.get('auto-insert', []) if isinstance(e, dict) and e.get('unit') != unit]
        if path:
            entries.append({'unit': unit, 'filename': path, 'rw': rw})
        j['auto-insert'] = sorted(entries, key=lambda e: e.get('unit', 0))
        write_json(DISK_CONF, j)
        self.send('OK saved')

    def cmd_scsi(self, args):
        for e in read_json(SCSI_CONF).get('units', []):
            try:
                self.send('S %d %d %s' % (int(e['unit']), 1 if e.get('rw') else 0, to_hex(relative(e['filename']))))
            except (KeyError, ValueError, TypeError):
                continue
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
        old = [e for e in j.get('units', []) if isinstance(e, dict)]
        keep = [e for e in old if e.get('unit') != unit]
        if path:
            # Keep the block size of a unit that had one
            block = next((e.get('block_size', 512) for e in old if e.get('unit') == unit), 512)
            keep.append({'unit': unit, 'filename': path, 'rw': rw, 'block_size': block})
        j['units'] = sorted(keep, key=lambda e: e.get('unit', 0))
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
