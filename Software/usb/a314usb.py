#!/usr/bin/python3
# -*- coding: utf-8 -*-

# a314usb service: USB memory sticks plugged into the Raspberry Pi appear on
# the Amiga as removable drives, USB0: to USB3:, the way floppies do. Each
# drive is served by the L:a314usb handler on the Amiga, which talks to this
# service with the same requests as a314fs (see a314fs/messages.h), plus:
#
#   USB_UNIT <unit>     the handler says which unit (0-3) it is, first
#   USB_STATUS          what is in the unit: a volume id (0 for none, a new
#                       id each time a stick is inserted), the volume's date
#                       (the same for the same stick, so that the Amiga sees
#                       it as the same volume when it is inserted again),
#                       whether it is write-protected, and its name
#   USB_EJECT           make the stick safe to pull out: its files are
#                       closed, it is unmounted and powered off (the handler
#                       sends this when it gets its eject packet)
#
# Sticks are found with lsblk, when udev reports a change (and every few
# seconds), and mounted through udisks2 as the user this service runs as
# (the installer adds a polkit rule for that). Any file system the Pi can
# mount works: FAT, exFAT, NTFS, ext4 and so on. A stick that is already
# mounted (by the desktop, say) is used where it is.
#
# File names are given to the Amiga in ISO-8859-1: characters it doesn't
# have become '?', and ':' and '/' become '_'. Names are looked up the same
# way, ignoring case, so a name the Amiga was given can always be found.
# Protection bits and comments are kept in a hidden file ".a314" in each
# directory (a314fs's "name:a314" files are not possible on FAT, exFAT and
# NTFS), written only for files that have any.
#
# The volume's locks are kept when its stick is pulled out, so that they
# work again when the same stick (by its file system UUID) is put back, as
# with a floppy; its open files are closed. A stick that is put back goes
# into the unit it was in, if that is free. File keys are never reused
# (one counter for all sticks), so a file left open on a stick that was
# pulled out can't reach a file on another one. Each volume's date is
# different from every other's, as AmigaDOS tells volumes apart by name and
# date.

import errno
import json
import logging
import os
import select
import shutil
import struct
import subprocess
import sys
import time
import zlib

from a314d import A314d

logging.basicConfig(format = '%(levelname)s, %(asctime)s, %(name)s, line %(lineno)d: %(message)s')
logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

SERVICE_NAME = 'a314usb'
CONFIG_FILE = '/etc/opt/a314/a314usb.conf'

UNITS = 4
SCAN_INTERVAL = 5               # seconds, besides udev events
FILE_SYSTEMS = ('vfat', 'exfat', 'ntfs', 'ntfs3', 'ext2', 'ext3', 'ext4',
                'hfsplus', 'btrfs', 'xfs', 'f2fs')
META_FILE = '.a314'             # protection bits and comments
MAX_NAME = 107                  # what fits in a FileInfoBlock
MAX_LABEL = 30

# Requests (a314fs/messages.h), and the two of this service
ACTION_LOCATE_OBJECT    = 8
ACTION_RENAME_DISK      = 9
ACTION_FREE_LOCK        = 15
ACTION_DELETE_OBJECT    = 16
ACTION_RENAME_OBJECT    = 17
ACTION_COPY_DIR         = 19
ACTION_SET_PROTECT      = 21
ACTION_CREATE_DIR       = 22
ACTION_EXAMINE_OBJECT   = 23
ACTION_EXAMINE_NEXT     = 24
ACTION_DISK_INFO        = 25
ACTION_INFO             = 26
ACTION_SET_COMMENT      = 28
ACTION_PARENT           = 29
ACTION_SET_DATE         = 34
ACTION_SAME_LOCK        = 40
ACTION_WRITE            = ord('W')
ACTION_READ             = ord('R')
ACTION_FINDUPDATE       = 1004
ACTION_FINDINPUT        = 1005
ACTION_FINDOUTPUT       = 1006
ACTION_END              = 1007
ACTION_SEEK             = 1008
ACTION_EXAMINE_FH       = 1034
ACTION_USB_UNIT         = 0x5501
ACTION_USB_STATUS       = 0x5502
ACTION_USB_EJECT        = 0x5503
ACTION_UNSUPPORTED      = 65535

ERROR_OBJECT_IN_USE             = 202
ERROR_OBJECT_EXISTS             = 203
ERROR_OBJECT_NOT_FOUND          = 205
ERROR_ACTION_NOT_KNOWN          = 209
ERROR_INVALID_LOCK              = 211
ERROR_OBJECT_WRONG_TYPE         = 212
ERROR_DISK_WRITE_PROTECTED      = 214
ERROR_DIRECTORY_NOT_EMPTY       = 216
ERROR_SEEK_ERROR                = 219
ERROR_COMMENT_TOO_BIG           = 220
ERROR_DISK_FULL                 = 221
ERROR_DELETE_PROTECTED          = 222
ERROR_WRITE_PROTECTED           = 223
ERROR_READ_PROTECTED            = 224
ERROR_NO_DISK                   = 226
ERROR_NO_MORE_ENTRIES           = 232

SHARED_LOCK         = -2
LOCK_DIFFERENT      = -1
LOCK_SAME           = 0
LOCK_SAME_VOLUME    = 1
MODE_OLDFILE        = 1005
MODE_NEWFILE        = 1006
MODE_READWRITE      = 1004
OFFSET_CURRENT      = 0
OFFSET_END          = 1
ST_USERDIR          = 2
ST_FILE             = -3
INFO_BLOCK_SIZE     = 512


def fail(error):
    return struct.pack('>HH', 0, error)


def ok(fmt='', *values):
    return struct.pack('>HH' + fmt, 1, 0, *values)


def amiga_name(name):
    # A name as the Amiga sees it (see the top)
    out = ''.join(c if ord(c) < 256 and c not in ':/' else ('_' if c in ':/' else '?')
                  for c in name)
    if len(out) > MAX_NAME:
        out = out[:MAX_NAME - 9] + '~%08x' % (zlib.crc32(name.encode('utf-8', 'surrogateescape')))
    return out


def pstr(s):
    b = s.encode('latin-1', 'replace')[:255]
    return bytes((len(b),)) + b


def mtime_to_dmt(mtime):
    mtime = int(mtime)
    days = mtime // 86400
    left = mtime - days * 86400
    mins = left // 60
    ticks = (left - mins * 60) * 50
    return (max(0, days - 2922), mins, ticks)   # 2922 days from 1970 to 1978


def dmt_to_mtime(days, mins, ticks):
    return (days + 2922) * 86400 + mins * 60 + ticks // 50


class Lock(object):
    def __init__(self, path, mode):
        self.path = path            # a tuple of the Pi's names, () for the root
        self.mode = mode
        self.entries = None         # for ExamineNext


class OpenFile(object):
    def __init__(self, f, path, protection):
        self.f = f
        self.path = path
        self.protection = protection


_file_keys = [int(time.time()) & 0xffffff]  # one counter for every stick's files and locks


def new_file_key():
    _file_keys[0] = _file_keys[0] + 1 if _file_keys[0] < 0x7fffffff else 1
    return _file_keys[0]


class Volume(object):
    # A file system on a stick, as long as this service runs (its locks are
    # kept while the stick is out)

    def __init__(self, uuid):
        self.uuid = uuid
        self.dev = None
        self.mount = None           # where it is mounted, while it is in
        self.label = ''
        self.read_only = False
        self.volume_id = 0
        self.last_unit = None
        self.date = mtime_to_dmt(time.time())
        self.locks = {}
        self.files = {}

    def new_key(self, table):
        # From the one counter for every stick: a lock on one stick can
        # never name a lock on another
        key = new_file_key()
        while key in table:
            key = new_file_key()
        return key

    def full(self, cp):
        return os.path.join(self.mount, *cp) if cp else self.mount

    def close_files(self):
        for of in self.files.values():
            try:
                of.f.close()
            except OSError:
                pass
        self.files = {}

    # Paths -------------------------------------------------------------

    def find(self, key, name):
        # The Pi path (a tuple) for an Amiga name relative to a lock, or
        # None; a last component that doesn't exist is kept as given
        i = name.find(':')
        if i != -1:
            key = 0
            name = name[i + 1:]
        if key == 0:
            cp = ()
        elif key in self.locks:
            cp = self.locks[key].path
        else:
            return None
        if name == '':
            return cp               # the directory itself ("VOL:", or "")
        parts = name.split('/')
        for n, comp in enumerate(parts):
            last = n == len(parts) - 1
            if comp == '':
                if last and n > 0:
                    break           # a trailing '/'
                if not cp:
                    return None
                cp = cp[:-1]
                continue
            try:
                entries = os.listdir(self.full(cp))
            except OSError:
                return None
            want = comp.lower()
            match = next((e for e in entries if e != META_FILE and amiga_name(e).lower() == want), None)
            if match is None:
                if not last:
                    return None
                match = comp
            cp = cp + (match,)
        return cp

    # Metadata (protection bits and comments) ---------------------------

    def read_meta_dir(self, cp):
        try:
            with open(os.path.join(self.full(cp), META_FILE), encoding='utf-8') as f:
                meta = json.load(f)
            return meta if isinstance(meta, dict) else {}
        except (OSError, ValueError):
            return {}

    def read_meta(self, cp):
        if not cp:
            return (0, '')
        entry = self.read_meta_dir(cp[:-1]).get(cp[-1])
        if not isinstance(entry, list) or len(entry) != 2:
            return (0, '')
        try:
            return (int(entry[0]) & 0xffffffff, str(entry[1])[:79])
        except (TypeError, ValueError):
            return (0, '')

    def write_meta(self, cp, protection=None, comment=None):
        if not cp:
            return False
        p, c = self.read_meta(cp)
        p = p if protection is None else protection
        c = c if comment is None else comment
        meta = self.read_meta_dir(cp[:-1])
        if p == 0 and not c:
            if cp[-1] not in meta:
                return True
            del meta[cp[-1]]
        else:
            meta[cp[-1]] = [p, c]
        path = os.path.join(self.full(cp[:-1]), META_FILE)
        try:
            if meta:
                tmp = path + '.new'
                with open(tmp, 'w', encoding='utf-8') as f:
                    json.dump(meta, f)
                os.replace(tmp, path)
            elif os.path.exists(path):
                os.remove(path)
        except OSError as e:
            logger.warning('Unable to write %s: %s', path, e)
            return False
        return True

    def move_meta(self, cp1, cp2):
        p, c = self.read_meta(cp1)
        if p or c:
            self.write_meta(cp1, 0, '')
            self.write_meta(cp2, p, c)

    # Requests ----------------------------------------------------------

    def examine(self, cp, disk_key):
        path = self.full(cp)
        st = os.stat(path)
        days, mins, ticks = mtime_to_dmt(st.st_mtime)
        protection, comment = self.read_meta(cp)
        if os.path.isdir(path):
            size, type_ = 0, ST_USERDIR
        else:
            size, type_ = min(st.st_size, 2 ** 31 - 1), ST_FILE
        name = amiga_name(cp[-1]) if cp else self.label
        return ok('HhIIIII', disk_key, type_, size, protection, days, mins, ticks) + \
            pstr(name) + pstr(comment)

    def locate_object(self, key, mode, name):
        cp = self.find(key, name)
        if cp is None or not os.path.exists(self.full(cp)):
            return fail(ERROR_OBJECT_NOT_FOUND)
        key = self.new_key(self.locks)
        self.locks[key] = Lock(cp, mode)
        return ok('I', key)

    def free_lock(self, key):
        self.locks.pop(key, None)
        return ok()

    def copy_dir(self, key):
        if key == 0:
            cp = ()
        elif key in self.locks:
            cp = self.locks[key].path
        else:
            return fail(ERROR_INVALID_LOCK)
        new = self.new_key(self.locks)
        self.locks[new] = Lock(cp, SHARED_LOCK)
        return ok('I', new)

    def parent(self, key):
        if key not in self.locks:
            return fail(ERROR_INVALID_LOCK)
        cp = self.locks[key].path
        if not cp:
            return ok('I', 0)
        new = self.new_key(self.locks)
        self.locks[new] = Lock(cp[:-1], SHARED_LOCK)
        return ok('I', new)

    def examine_object(self, key):
        lock = self.locks.get(key) if key else Lock((), SHARED_LOCK)
        if lock is None:
            return fail(ERROR_INVALID_LOCK)
        try:
            res = self.examine(lock.path, 666)
            if os.path.isdir(self.full(lock.path)):
                lock.entries = sorted(e for e in os.listdir(self.full(lock.path)) if e != META_FILE)
            return res
        except OSError:
            return fail(ERROR_OBJECT_NOT_FOUND)

    def examine_next(self, key, disk_key):
        lock = self.locks.get(key)
        if lock is None:
            return fail(ERROR_INVALID_LOCK)
        if lock.entries is None:
            try:
                lock.entries = sorted(e for e in os.listdir(self.full(lock.path)) if e != META_FILE)
            except OSError:
                return fail(ERROR_OBJECT_WRONG_TYPE)
        while lock.entries:
            e = lock.entries.pop(0)
            try:
                return self.examine(lock.path + (e,), disk_key + 1)
            except OSError:
                continue            # gone since the listing
        return fail(ERROR_NO_MORE_ENTRIES)

    def examine_fh(self, fp):
        of = self.files.get(fp)
        if of is None:
            return fail(ERROR_INVALID_LOCK)
        try:
            return self.examine(of.path, 666)
        except OSError:
            return fail(ERROR_OBJECT_NOT_FOUND)

    def findxxx(self, mode, key, name):
        cp = self.find(key, name)
        if cp is None:
            return fail(ERROR_OBJECT_NOT_FOUND)
        path = self.full(cp)
        if not cp or os.path.isdir(path):
            return fail(ERROR_OBJECT_WRONG_TYPE)
        if mode != ACTION_FINDINPUT and self.read_only:
            return fail(ERROR_DISK_WRITE_PROTECTED)
        protection, _ = self.read_meta(cp)
        try:
            if mode == ACTION_FINDINPUT:
                f = open(path, 'r+b' if not self.read_only else 'rb')
            elif mode == ACTION_FINDUPDATE:
                f = open(path, 'r+b' if os.path.exists(path) else 'w+b')
            else:
                if protection & 0x1:
                    return fail(ERROR_DELETE_PROTECTED)
                if protection & 0x4:
                    return fail(ERROR_WRITE_PROTECTED)
                f = open(path, 'w+b')
        except FileNotFoundError:
            return fail(ERROR_OBJECT_NOT_FOUND)
        except PermissionError:
            try:
                f = open(path, 'rb')    # read-only on the Pi
            except OSError:
                return fail(ERROR_READ_PROTECTED)
        except OSError:
            return fail(ERROR_OBJECT_NOT_FOUND)
        if mode != ACTION_FINDINPUT and protection & 0x10:
            self.write_meta(cp, protection=protection & ~0x10)   # the archive bit
            protection &= ~0x10
        fp = new_file_key()
        self.files[fp] = OpenFile(f, cp, protection)
        return ok('I', fp)

    def read(self, service, fp, address, length):
        of = self.files.get(fp)
        if of is None:
            return fail(ERROR_INVALID_LOCK)
        if of.protection & 0x8:
            return fail(ERROR_READ_PROTECTED)
        try:
            data = of.f.read(length)
        except OSError:
            return fail(ERROR_SEEK_ERROR)
        if data:
            service.write_mem(address, data)
        return ok('I', len(data))

    def write(self, service, fp, address, length):
        of = self.files.get(fp)
        if of is None:
            return fail(ERROR_INVALID_LOCK)
        if of.protection & 0x4:
            return fail(ERROR_WRITE_PROTECTED)
        data = service.read_mem(address, length)
        try:
            of.f.seek(0, 1)
            of.f.write(data)
        except OSError as e:
            if e.errno == errno.EROFS or e.errno == errno.EBADF:
                return fail(ERROR_DISK_WRITE_PROTECTED)
            return fail(ERROR_DISK_FULL)
        return ok('I', length)

    def seek(self, fp, new_pos, mode):
        of = self.files.get(fp)
        if of is None:
            return fail(ERROR_INVALID_LOCK)
        try:
            old = of.f.tell()
            whence = 1 if mode == OFFSET_CURRENT else 2 if mode == OFFSET_END else 0
            of.f.seek(new_pos, whence)
        except (OSError, ValueError):
            return fail(ERROR_SEEK_ERROR)
        return ok('i', old)

    def end(self, fp):
        of = self.files.pop(fp, None)
        if of:
            try:
                of.f.close()
            except OSError:
                pass
        return ok()

    def delete_object(self, key, name):
        cp = self.find(key, name)
        if not cp:
            return fail(ERROR_OBJECT_NOT_FOUND)
        path = self.full(cp)
        protection, _ = self.read_meta(cp)
        if protection & 0x1:
            return fail(ERROR_DELETE_PROTECTED)
        try:
            if os.path.isdir(path):
                leftover = [e for e in os.listdir(path) if e != META_FILE]
                if leftover:
                    return fail(ERROR_DIRECTORY_NOT_EMPTY)
                if os.path.exists(os.path.join(path, META_FILE)):
                    os.remove(os.path.join(path, META_FILE))
                os.rmdir(path)
            else:
                os.remove(path)
        except FileNotFoundError:
            return fail(ERROR_OBJECT_NOT_FOUND)
        except OSError as e:
            return fail(ERROR_DISK_WRITE_PROTECTED if e.errno == errno.EROFS else ERROR_OBJECT_IN_USE)
        self.write_meta(cp, 0, '')
        return ok()

    def rename_object(self, key, name, target_key, new_name):
        cp1 = self.find(key, name)
        cp2 = self.find(target_key, new_name)
        if not cp1 or not cp2 or not os.path.exists(self.full(cp1)):
            return fail(ERROR_OBJECT_NOT_FOUND)
        # The new name as given (find() gives an existing name's own case,
        # which would make a change of case only do nothing)
        wanted = new_name.rsplit('/', 1)[-1].split(':')[-1]
        same = cp1[:-1] == cp2[:-1] and cp1[-1].lower() == cp2[-1].lower()
        if same:
            if cp1[-1] == wanted or not wanted:
                return ok()
            cp2 = cp2[:-1] + (wanted,)
        elif os.path.exists(self.full(cp2)):
            return fail(ERROR_OBJECT_EXISTS)
        try:
            if same:
                # FAT ignores a change of case in a plain rename: go
                # through another name
                tmp = self.full(cp1) + '.a314tmp'
                os.rename(self.full(cp1), tmp)
                os.rename(tmp, self.full(cp2))
            else:
                os.rename(self.full(cp1), self.full(cp2))
        except OSError as e:
            return fail(ERROR_DISK_WRITE_PROTECTED if e.errno == errno.EROFS else ERROR_OBJECT_IN_USE)
        self.move_meta(cp1, cp2)
        return ok()

    def create_dir(self, key, name):
        cp = self.find(key, name)
        if not cp:
            return fail(ERROR_OBJECT_NOT_FOUND)
        if os.path.exists(self.full(cp)):
            return fail(ERROR_OBJECT_EXISTS)
        try:
            os.mkdir(self.full(cp))
        except OSError as e:
            return fail(ERROR_DISK_WRITE_PROTECTED if e.errno == errno.EROFS else ERROR_OBJECT_NOT_FOUND)
        new = self.new_key(self.locks)
        self.locks[new] = Lock(cp, SHARED_LOCK)
        return ok('I', new)

    def set_protect(self, key, name, mask):
        cp = self.find(key, name)
        if not cp or not os.path.exists(self.full(cp)):
            return fail(ERROR_OBJECT_NOT_FOUND)
        return ok() if self.write_meta(cp, protection=mask) else fail(ERROR_DISK_WRITE_PROTECTED)

    def set_comment(self, key, name, comment):
        if len(comment) > 79:
            return fail(ERROR_COMMENT_TOO_BIG)
        cp = self.find(key, name)
        if not cp or not os.path.exists(self.full(cp)):
            return fail(ERROR_OBJECT_NOT_FOUND)
        return ok() if self.write_meta(cp, comment=comment) else fail(ERROR_DISK_WRITE_PROTECTED)

    def set_date(self, key, name, days, mins, ticks):
        cp = self.find(key, name)
        if not cp:
            return fail(ERROR_OBJECT_NOT_FOUND)
        try:
            path = self.full(cp)
            os.utime(path, (os.stat(path).st_atime, dmt_to_mtime(days, mins, ticks)))
        except OSError:
            return fail(ERROR_OBJECT_NOT_FOUND)
        return ok()

    def same_lock(self, key1, key2):
        l1, l2 = self.locks.get(key1), self.locks.get(key2)
        if not l1 or not l2:
            return struct.pack('>HH', 0, LOCK_DIFFERENT & 0xffff)
        if l1.path == l2.path:
            return struct.pack('>HH', 1, LOCK_SAME)
        return struct.pack('>HH', 0, LOCK_SAME_VOLUME)

    def info(self):
        try:
            total, used, _ = shutil.disk_usage(self.mount)
        except OSError:
            total = used = 0
        return ok('III', total // INFO_BLOCK_SIZE, used // INFO_BLOCK_SIZE, INFO_BLOCK_SIZE)


class UsbService(object):
    def __init__(self):
        self.a314d = A314d(SERVICE_NAME)
        self.pending = []           # messages that came while reading memory
        self.streams = {}           # stream id -> unit (or None before USB_UNIT)
        self.units = [None] * UNITS     # the Volume in each unit
        self.volumes = {}           # uuid -> Volume, every one seen
        self.next_volume_id = 1
        self.ejected = set()        # uuids ejected but not yet pulled out
        # Loop devices count as sticks too, for testing (the config file's
        # "allow_loop", or A314USB_ALLOW_LOOP=1)
        self.allow_loop = os.environ.get('A314USB_ALLOW_LOOP') == '1'
        try:
            with open(CONFIG_FILE, encoding='utf-8') as f:
                cfg = json.load(f)
            self.allow_loop = self.allow_loop or bool(cfg.get('allow_loop', False))
        except (OSError, ValueError):
            pass
        self.monitor = None
        try:
            import pyudev
            ctx = pyudev.Context()
            self.monitor = pyudev.Monitor.from_netlink(ctx)
            self.monitor.filter_by('block')
            self.monitor.start()
        except Exception as e:
            logger.warning('No udev events (%s); checking every %d seconds', e, SCAN_INTERVAL)
        self.last_scan = 0

    # a314d -------------------------------------------------------------

    def read_mem(self, address, length):
        self.a314d.send_read_mem_req(address, length)
        while True:
            stream_id, ptype, payload = self.a314d.wait_for_msg()
            if ptype == A314d.MSG_READ_MEM_RES:
                return payload
            self.pending.append((stream_id, ptype, payload))

    def write_mem(self, address, data):
        self.a314d.send_write_mem_req(address, data)
        while True:
            stream_id, ptype, payload = self.a314d.wait_for_msg()
            if ptype == A314d.MSG_WRITE_MEM_RES:
                return
            self.pending.append((stream_id, ptype, payload))

    # Sticks ------------------------------------------------------------

    def find_sticks(self):
        # {uuid: (device, fstype, label, mountpoint or None, read-only)}
        try:
            out = subprocess.run(['lsblk', '-J', '-o', 'PATH,PKNAME,NAME,FSTYPE,LABEL,UUID,MOUNTPOINTS,TRAN,TYPE,RO'],
                                 capture_output=True, text=True, timeout=10).stdout
            devs = json.loads(out).get('blockdevices', [])
        except (OSError, ValueError, subprocess.SubprocessError) as e:
            logger.warning('lsblk failed: %s', e)
            return None
        tran = {d['name']: d.get('tran') for d in devs}
        dtype = {d['name']: d.get('type') for d in devs}
        found = {}
        for d in devs:
            parent = d.get('pkname') or d['name']
            usb = tran.get(parent) == 'usb' or (self.allow_loop and dtype.get(parent) == 'loop')
            if not usb or d.get('fstype') not in FILE_SYSTEMS or not d.get('uuid'):
                continue
            mounts = [m for m in (d.get('mountpoints') or []) if m]
            found[d['uuid']] = (d['path'], d['fstype'], d.get('label') or '', mounts[0] if mounts else None,
                                bool(d.get('ro')))
        return found

    def mount(self, dev):
        try:
            r = subprocess.run(['udisksctl', 'mount', '-b', dev, '--no-user-interaction'],
                               capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            logger.warning('Unable to mount %s: %s', dev, e)
            return None
        if r.returncode != 0:
            logger.warning('Unable to mount %s: %s', dev, r.stderr.strip())
            return None
        # "Mounted /dev/sda1 at /media/user/LABEL"
        line = r.stdout.strip()
        return line.split(' at ', 1)[1].rstrip('.') if ' at ' in line else None

    def scan(self):
        self.last_scan = time.time()
        found = self.find_sticks()
        if found is None:
            return
        # Sticks that were pulled out
        for unit, vol in enumerate(self.units):
            if vol and vol.uuid not in found:
                logger.info('USB%d: %s (%s) removed', unit, vol.label, vol.dev)
                vol.close_files()
                vol.mount = None
                vol.dev = None
                vol.volume_id = 0
                self.units[unit] = None
        # An ejected stick is not mounted again until it has been pulled out
        self.ejected &= set(found)
        # Sticks that were put in
        for uuid, (dev, fstype, label, mountpoint, ro) in found.items():
            if uuid in self.ejected or any(v and v.uuid == uuid for v in self.units):
                continue
            if None not in self.units:
                logger.warning('No free unit for %s', dev)
                continue
            if mountpoint is None:
                mountpoint = self.mount(dev)
                if mountpoint is None:
                    continue
            vol = self.volumes.get(uuid)
            if vol is None:
                vol = Volume(uuid)
                # A date no other volume has (a tick later, if need be)
                taken = {v.date for v in self.volumes.values()}
                while vol.date in taken:
                    d, m, t = vol.date
                    vol.date = (d, m, t + 1) if t < 2999 else (d, m + 1, 0) if m < 1439 else (d + 1, 0, 0)
                self.volumes[uuid] = vol
            vol.dev = dev
            vol.mount = mountpoint
            vol.read_only = ro or not os.access(mountpoint, os.W_OK)
            vol.label = (amiga_name(label).replace('?', '_')[:MAX_LABEL] if label else '') or \
                'USB_' + uuid.replace('-', '')[:8]
            vol.volume_id = self.next_volume_id
            self.next_volume_id += 1
            if vol.last_unit is not None and self.units[vol.last_unit] is None:
                unit = vol.last_unit        # back where it was
            else:
                unit = self.units.index(None)
            vol.last_unit = unit
            self.units[unit] = vol
            logger.info('USB%d: %s (%s, %s) at %s%s', unit, vol.label, dev, fstype, mountpoint,
                        ', read-only' if vol.read_only else '')

    def eject(self, unit):
        # Makes the stick in the unit safe to pull out
        vol = self.units[unit]
        vol.close_files()
        try:
            r = subprocess.run(['udisksctl', 'unmount', '-b', vol.dev, '--no-user-interaction'],
                               capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.SubprocessError) as e:
            logger.warning('Unable to unmount %s: %s', vol.dev, e)
            return fail(ERROR_OBJECT_IN_USE)
        if r.returncode != 0:
            logger.warning('Unable to unmount %s: %s', vol.dev, r.stderr.strip())
            return fail(ERROR_OBJECT_IN_USE)
        # Power the stick off (not possible for every device, which is fine)
        try:
            out = subprocess.run(['lsblk', '-n', '-o', 'PKNAME', vol.dev], capture_output=True,
                                 text=True, timeout=10).stdout.strip()
            disk = '/dev/' + out if out else vol.dev
            subprocess.run(['udisksctl', 'power-off', '-b', disk, '--no-user-interaction'],
                           capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.SubprocessError):
            pass
        logger.info('USB%d: %s (%s) ejected', unit, vol.label, vol.dev)
        self.ejected.add(vol.uuid)
        vol.mount = None
        vol.dev = None
        vol.volume_id = 0
        self.units[unit] = None
        return ok()

    # Requests ----------------------------------------------------------

    def status(self, unit):
        vol = self.units[unit] if unit is not None else None
        if not vol:
            return ok('IIIIB', 0, 0, 0, 0, 0) + pstr('')
        days, mins, ticks = vol.date
        return ok('IIIIB', vol.volume_id, days, mins, ticks, int(vol.read_only)) + pstr(vol.label)

    def process(self, stream_id, req):
        (rtype,) = struct.unpack('>H', req[:2])
        if rtype == ACTION_USB_UNIT:
            (unit,) = struct.unpack('>H', req[2:4])
            if unit >= UNITS:
                return fail(ERROR_OBJECT_NOT_FOUND)
            self.streams[stream_id] = unit
            return ok()
        unit = self.streams.get(stream_id)
        if rtype == ACTION_USB_STATUS:
            return self.status(unit)
        if rtype == ACTION_USB_EJECT:
            if unit is None or not self.units[unit]:
                return fail(ERROR_NO_DISK)
            return self.eject(unit)
        vol = self.units[unit] if unit is not None else None
        if not vol:
            return fail(ERROR_NO_DISK)
        try:
            return self.request(vol, rtype, req)
        except (struct.error, IndexError, KeyError) as e:
            logger.warning('Bad request %d: %s', rtype, e)
            return fail(ERROR_ACTION_NOT_KNOWN)

    def request(self, vol, rtype, req):
        def name_at(offset, n):
            return req[offset:offset + n].decode('latin-1')
        if rtype == ACTION_LOCATE_OBJECT:
            key, mode, n = struct.unpack('>IHB', req[2:9])
            return vol.locate_object(key, mode, name_at(9, n))
        if rtype == ACTION_FREE_LOCK:
            return vol.free_lock(struct.unpack('>I', req[2:6])[0])
        if rtype == ACTION_COPY_DIR:
            return vol.copy_dir(struct.unpack('>I', req[2:6])[0])
        if rtype == ACTION_PARENT:
            return vol.parent(struct.unpack('>I', req[2:6])[0])
        if rtype == ACTION_EXAMINE_OBJECT:
            return vol.examine_object(struct.unpack('>I', req[2:6])[0])
        if rtype == ACTION_EXAMINE_NEXT:
            key, disk_key = struct.unpack('>IH', req[2:8])
            return vol.examine_next(key, disk_key)
        if rtype == ACTION_EXAMINE_FH:
            return vol.examine_fh(struct.unpack('>I', req[2:6])[0])
        if rtype in (ACTION_FINDINPUT, ACTION_FINDOUTPUT, ACTION_FINDUPDATE):
            key, n = struct.unpack('>IB', req[2:7])
            return vol.findxxx(rtype, key, name_at(7, n))
        if rtype == ACTION_READ:
            fp, address, length = struct.unpack('>III', req[2:14])
            return vol.read(self, fp, address, length)
        if rtype == ACTION_WRITE:
            fp, address, length = struct.unpack('>III', req[2:14])
            if vol.read_only:
                return fail(ERROR_DISK_WRITE_PROTECTED)
            return vol.write(self, fp, address, length)
        if rtype == ACTION_SEEK:
            fp, pos, mode = struct.unpack('>Iii', req[2:14])
            return vol.seek(fp, pos, mode)
        if rtype == ACTION_END:
            return vol.end(struct.unpack('>I', req[2:6])[0])
        if rtype == ACTION_SAME_LOCK:
            return vol.same_lock(*struct.unpack('>II', req[2:10]))
        if rtype in (ACTION_DISK_INFO, ACTION_INFO):
            return vol.info()
        if rtype == ACTION_UNSUPPORTED:
            return fail(ERROR_ACTION_NOT_KNOWN)
        # The rest change the disk
        if vol.read_only and rtype in (ACTION_DELETE_OBJECT, ACTION_RENAME_OBJECT, ACTION_CREATE_DIR,
                                       ACTION_SET_PROTECT, ACTION_SET_DATE, ACTION_SET_COMMENT):
            return fail(ERROR_DISK_WRITE_PROTECTED)
        if rtype == ACTION_DELETE_OBJECT:
            key, n = struct.unpack('>IB', req[2:7])
            return vol.delete_object(key, name_at(7, n))
        if rtype == ACTION_RENAME_OBJECT:
            key, target, n1, n2 = struct.unpack('>IIBB', req[2:12])
            return vol.rename_object(key, name_at(12, n1), target, name_at(12 + n1, n2))
        if rtype == ACTION_CREATE_DIR:
            key, n = struct.unpack('>IB', req[2:7])
            return vol.create_dir(key, name_at(7, n))
        if rtype == ACTION_SET_PROTECT:
            key, mask, n = struct.unpack('>IIB', req[2:11])
            return vol.set_protect(key, name_at(11, n), mask)
        if rtype == ACTION_SET_DATE:
            key, days, mins, ticks, n = struct.unpack('>IIIIB', req[2:19])
            return vol.set_date(key, name_at(19, n), days, mins, ticks)
        if rtype == ACTION_SET_COMMENT:
            key, n, c = struct.unpack('>IBB', req[2:8])
            return vol.set_comment(key, name_at(8, n), name_at(8 + n, c))
        logger.warning('Unsupported request %d', rtype)
        return fail(ERROR_ACTION_NOT_KNOWN)

    def handle_msg(self, stream_id, ptype, payload):
        if ptype == A314d.MSG_CONNECT:
            if payload == SERVICE_NAME.encode():
                self.streams[stream_id] = None
                self.a314d.send_connect_response(stream_id, 0)
            else:
                self.a314d.send_connect_response(stream_id, 3)
        elif ptype == A314d.MSG_DATA:
            if stream_id not in self.streams or len(payload) < 8:
                return
            address, length = struct.unpack('>II', payload[:8])
            req = self.read_mem(address + 2, length - 2)
            res = self.process(stream_id, req)
            self.write_mem(address + 2, res)
            self.a314d.send_data(stream_id, struct.pack('>I', len(res) + 2))
        elif ptype in (A314d.MSG_EOS, A314d.MSG_RESET):
            if stream_id in self.streams:
                if ptype == A314d.MSG_EOS:
                    self.a314d.send_eos(stream_id)
                del self.streams[stream_id]

    def run(self):
        self.scan()
        while True:
            while self.pending:
                self.handle_msg(*self.pending.pop(0))
            fds = [self.a314d] + ([self.monitor] if self.monitor else [])
            timeout = max(0.1, SCAN_INTERVAL - (time.time() - self.last_scan))
            rl, _, _ = select.select(fds, [], [], timeout)
            if self.monitor and self.monitor in rl:
                while self.monitor.poll(timeout=0) is not None:
                    pass
                time.sleep(0.5)     # let udisks2 and the kernel settle
                self.scan()
            if self.a314d in rl:
                self.handle_msg(*self.a314d.wait_for_msg())
            if time.time() - self.last_scan >= SCAN_INTERVAL:
                self.scan()


if __name__ == '__main__':
    service = UsbService()
    logger.info('%s service is running', SERVICE_NAME)
    try:
        service.run()
    except KeyboardInterrupt:
        pass
    service.a314d.close()
