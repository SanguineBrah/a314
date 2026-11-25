#!/usr/bin/python3
# -*- coding: utf-8 -*-

# PiWiFi service: lets the PiWiFi program on the Amiga choose the WiFi
# network the Raspberry Pi connects to.
#
# The work is done by NetworkManager, through nmcli. A change of network is
# made safe like this:
#
# - A NetworkManager checkpoint is created first. Rolling it back restores
#   the previous connection and deletes connection profiles created since.
#   NetworkManager rolls it back by itself after ROLLBACK_SECONDS, even if
#   this service stops half way.
# - A new profile is created with autoconnect off, so that it can't be
#   chosen by itself (for example after a power cut) before it is known to
#   work. A saved profile is never changed: a new password gives a new
#   profile, and the old one is deleted once the new one works.
# - The new network must give access to the router and to the internet,
#   tested with ping through the WiFi interface. Then autoconnect is turned
#   on and the checkpoint is destroyed, which keeps the change.
#
# Changing settings needs root, so this service uses "sudo -n", which works
# for the first user of Raspberry Pi OS.
#
# The Amiga and this service exchange text lines ending in '\n'. SSIDs and
# passwords are sent as hex, as they can contain any characters.
#
# Amiga -> Pi:
#   STATUS                    send the current connection
#   SCAN                      scan for networks and send the list
#   CONNECT <ssid> [<pass>]   connect to a network (no password: use the
#                             saved one, or connect to an open network)
#   FORGET <ssid>             delete a saved network that is not in use
#
# Pi -> Amiga:
#   CUR <state> <signal> <ip> <ssid>   state: connected, connecting,
#                             disconnected or unavailable; - for unknown
#   NETS                      a network list follows
#   NET <signal> <flags> <security> <ssid>   flags: a = in use, s = saved
#   END                       end of the list
#   BUSY <text>               an operation has started
#   DONE <0|1> <text>         an operation has failed (0) or succeeded (1)

import collections
import logging
import os
import queue
import re
import select
import subprocess
import sys
import threading
import time

from a314d import A314d

logging.basicConfig(format = '%(levelname)s, %(asctime)s, %(name)s, line %(lineno)d: %(message)s')
logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

SERVICE_NAME = 'piwifi'

ROLLBACK_SECONDS = 120      # NetworkManager undoes a change after this
CONNECT_SECONDS = 45        # how long nmcli may try to connect
CHECK_SECONDS = 30          # how long to wait for internet access
INTERNET_HOSTS = ['1.1.1.1', '8.8.8.8', '9.9.9.9']     # answered by any one
MAX_NETWORKS = 30
MAX_CHUNK = 200             # largest MSG_DATA sent to the Amiga

# NM_CHECKPOINT_CREATE_FLAG_DELETE_NEW_CONNECTIONS
CHECKPOINT_DELETE_NEW = 2

NM = ['org.freedesktop.NetworkManager', '/org/freedesktop/NetworkManager',
    'org.freedesktop.NetworkManager']


def sentence(text):
    # An error message as a sentence: its first letter in upper case
    return text[:1].upper() + text[1:]


class Failed(Exception):
    pass


def run(argv, timeout=20, sudo=False):
    # Runs a command and returns its output. Raises Failed on errors.
    if sudo:
        argv = ['sudo', '-n'] + argv
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise Failed('%s took too long' % argv[2 if sudo else 0])
    except OSError as e:
        raise Failed('cannot run %s: %s' % (argv[0], e.strerror))
    if r.returncode != 0:
        lines = (r.stderr or r.stdout).strip().split('\n')
        logger.warning('%s failed: %s', ' '.join(argv[:4]), lines[-1])
        raise Failed(lines[-1].replace('Error: ', '')[:70])
    return r.stdout


def split_terse(line):
    # Splits a line of "nmcli -t" or "nmcli -g" output, where ':' separates
    # the fields and '\' escapes ':' and '\' inside them.
    fields, cur, i = [], '', 0
    while i < len(line):
        c = line[i]
        if c == '\\' and i + 1 < len(line):
            cur += line[i + 1]
            i += 2
            continue
        if c == ':':
            fields.append(cur)
            cur = ''
        else:
            cur += c
        i += 1
    fields.append(cur)
    return fields


def printable(text):
    return ''.join(c if c >= ' ' else '?' for c in text)


def to_hex(text):
    return text.encode('utf-8').hex() if text else '-'


def from_hex(word):
    return bytes.fromhex(word).decode('utf-8')


class WiFi(object):
    # The nmcli commands. Each method runs in the worker thread.

    def __init__(self):
        self.security = {}          # SSID -> security, from the last scan

    def device(self):
        for line in run(['nmcli', '-t', '-f', 'DEVICE,TYPE', 'device', 'status']).split('\n'):
            f = split_terse(line)
            if len(f) == 2 and f[1] == 'wifi':
                return f[0]
        raise Failed('no WiFi adapter found')

    def device_state(self, dev):
        # Returns (state, uuid of the connection in use, IP address).
        state, uuid, ip = 'disconnected', '', '-'
        out = run(['nmcli', '-t', '-f', 'GENERAL.STATE,GENERAL.CON-UUID,IP4.ADDRESS', 'device', 'show', dev])
        for line in out.split('\n'):
            f = split_terse(line)
            if len(f) < 2:
                continue
            if f[0] == 'GENERAL.STATE':
                code = int(f[1].split()[0])     # e.g. "100 (connected)"
                if code == 100:
                    state = 'connected'
                elif 40 <= code < 100:
                    state = 'connecting'
                elif code < 30:
                    state = 'unavailable'
            elif f[0] == 'GENERAL.CON-UUID':
                uuid = f[1]
            elif f[0].startswith('IP4.ADDRESS') and ip == '-':
                ip = f[1].split('/')[0]
        return state, uuid, ip

    def status(self):
        state, uuid, ip = self.device_state(self.device())
        signal, ssid = '-', ''
        for net in self.networks(rescan=False):
            if 'a' in net[1]:
                signal, ssid = net[0], net[3]
        if state == 'connected' and not ssid:
            ssid = self.saved().get(uuid, '')     # e.g. a hidden network
        return state, signal, ip, ssid

    def saved(self):
        # Returns {uuid: ssid} of the saved WiFi profiles.
        result = {}
        out = run(['nmcli', '-t', '-f', 'UUID,TYPE', 'connection', 'show'])
        for line in out.split('\n'):
            f = split_terse(line)
            if len(f) == 2 and f[1] == '802-11-wireless':
                out = run(['nmcli', '-g', '802-11-wireless.ssid', 'connection', 'show', f[0]])
                result[f[0]] = split_terse(out.rstrip('\n'))[0]
        return result

    def networks(self, rescan):
        # Returns [(signal, flags, security, ssid)], the network in use
        # first, then strongest first, one entry per SSID.
        argv = ['nmcli', '-t', '-f', 'IN-USE,SIGNAL,SECURITY,SSID', 'device',
            'wifi', 'list', '--rescan', 'yes' if rescan else 'no']
        out = run(argv, timeout=30, sudo=rescan)
        saved = set(self.saved().values()) if rescan else set()
        best = {}
        for line in out.split('\n'):
            f = split_terse(line)
            if len(f) != 4 or not f[3]:
                continue                    # hidden networks have no SSID
            in_use, signal, security, ssid = f
            flags = ('a' if in_use == '*' else '') + ('s' if ssid in saved else '')
            security = security.replace(' ', '/') or 'open'
            # Several access points can share an SSID: keep the one in
            # use, or else the strongest
            rank = ('a' in flags, int(signal))
            if ssid not in best or rank > best[ssid][0]:
                best[ssid] = (rank, (signal, flags or '-', security, ssid))
        self.security = {net[3]: net[2] for rank, net in best.values()}
        nets = sorted(best.values(), reverse=True)
        return [net for rank, net in nets[:MAX_NETWORKS]]

    def key_mgmt(self, ssid, password):
        # The wifi-sec.key-mgmt for a new profile, or None for an open network
        security = self.security.get(ssid)
        if security is None:                # hidden: guess from the password
            return 'wpa-psk' if password else None
        if '802.1X' in security or 'WEP' in security:
            raise Failed('%s networks are not supported' % security)
        if security == 'open':
            return None
        if not password:
            raise Failed('%s needs a password' % ssid)
        if 'WPA1' in security or 'WPA2' in security:
            return 'wpa-psk'
        return 'sae'                        # WPA3 only

    def connect(self, ssid, password, progress):
        name = ssid.encode('utf-8')
        if not 1 <= len(name) <= 32 or any(c < 32 for c in name):
            raise Failed('that is not a valid network name')
        if password and not (8 <= len(password) <= 63 and password.isprintable()):
            raise Failed('the password must be 8 to 63 characters')

        dev = self.device()
        self.networks(rescan=False)         # for self.security
        uuids = [u for u, s in self.saved().items() if s == ssid]
        new_profile = bool(password) or not uuids
        key_mgmt = self.key_mgmt(ssid, password) if new_profile else None
        path = run(['busctl', 'call'] + NM + ['GetDeviceByIpIface', 's', dev]).split('"')[1]

        # From here on, NetworkManager puts the old settings back by itself
        # unless the checkpoint is destroyed within ROLLBACK_SECONDS.
        try:
            out = run(['busctl', 'call'] + NM + ['CheckpointCreate', 'aouu', '1', path,
                str(ROLLBACK_SECONDS), str(CHECKPOINT_DELETE_NEW)], sudo=True)
        except Failed as e:
            raise Failed('cannot protect the change (%s)' % e)
        checkpoint = out.split('"')[1]
        logger.info('Connecting to %s, checkpoint %s', ssid, checkpoint)

        try:
            if new_profile:
                argv = ['nmcli', 'connection', 'add', 'type', 'wifi', 'ifname', dev,
                    'con-name', ssid, 'ssid', ssid, 'connection.autoconnect', 'no']
                if ssid not in self.security:
                    argv += ['802-11-wireless.hidden', 'yes']
                if key_mgmt:
                    argv += ['wifi-sec.key-mgmt', key_mgmt, 'wifi-sec.psk', password]
                out = run(argv, sudo=True)
                # "Connection '<name>' (<uuid>) successfully added."
                uuid = re.search(r'\(([0-9a-f-]{36})\)', out).group(1)
            else:
                uuid = uuids[0]
            try:
                run(['nmcli', '--wait', str(CONNECT_SECONDS), 'connection', 'up', 'uuid', uuid],
                    timeout=CONNECT_SECONDS + 10, sudo=True)
            except Failed as e:
                if 'Secrets were required' in str(e):
                    raise Failed('wrong password for ' + ssid)
                raise

            progress('Checking internet access...')
            if not self.has_internet(dev):
                raise Failed('no internet access through ' + ssid)
            if new_profile:
                run(['nmcli', 'connection', 'modify', 'uuid', uuid, 'connection.autoconnect', 'yes'], sudo=True)
            run(['busctl', 'call'] + NM + ['CheckpointDestroy', 'o', checkpoint], sudo=True)
        except Exception as e:
            logger.warning('Going back to the previous network: %s', e)
            progress('Going back to the previous network...')
            try:
                run(['busctl', 'call'] + NM + ['CheckpointRollback', 'o', checkpoint], sudo=True)
            except Failed:
                pass    # the checkpoint times out and rolls back anyway
            raise

        # The change is kept. Old profiles for the network aren't needed.
        if new_profile:
            for old in uuids:
                try:
                    run(['nmcli', 'connection', 'delete', 'uuid', old], sudo=True)
                except Failed:
                    pass
        logger.info('Connected to %s', ssid)

    def has_internet(self, dev):
        # The router, and one of INTERNET_HOSTS, must answer through dev
        end = time.time() + CHECK_SECONDS
        while time.time() < end:
            try:
                route = run(['ip', 'route', 'show', 'default', 'dev', dev]).split()
                gateway = route[route.index('via') + 1]
                if self.ping(dev, gateway) and any(self.ping(dev, h) for h in INTERNET_HOSTS):
                    return True
            except (Failed, ValueError, IndexError):
                pass
            time.sleep(2)
        return False

    def ping(self, dev, host):
        try:
            run(['ping', '-c', '1', '-W', '3', '-I', dev, host])
            return True
        except Failed:
            return False

    def forget(self, ssid):
        uuids = [u for u, s in self.saved().items() if s == ssid]
        if not uuids:
            raise Failed(ssid + ' is not saved')
        state, in_use, ip = self.device_state(self.device())
        if state == 'connecting':
            raise Failed('the Pi is connecting, try again in a moment')
        if in_use in uuids:
            raise Failed('%s is in use, connect to another network first' % ssid)
        for uuid in uuids:
            run(['nmcli', 'connection', 'delete', 'uuid', uuid], sudo=True)


class Service(object):
    def __init__(self):
        self.a314d = A314d(SERVICE_NAME)
        self.stream_id = None
        self.inbuf = b''
        self.wifi = WiFi()
        self.jobs = collections.deque()     # requests waiting for the worker
        self.busy = False
        self.results = queue.Queue()        # lines from the worker thread
        self.wake_r, self.wake_w = os.pipe()

    def send(self, lines):
        # Sends lines to the Amiga in chunks of whole lines.
        if self.stream_id is None:
            return
        chunk = b''
        for line in lines:
            data = line.encode('latin-1', 'replace')[:MAX_CHUNK - 1] + b'\n'
            if len(chunk) + len(data) > MAX_CHUNK:
                self.a314d.send_data(self.stream_id, chunk)
                chunk = b''
            chunk += data
        if chunk:
            self.a314d.send_data(self.stream_id, chunk)

    def status_line(self):
        state, signal, ip, ssid = self.wifi.status()
        return 'CUR %s %s %s %s' % (state, signal, ip, to_hex(ssid))

    def network_lines(self):
        lines = ['NETS']
        for signal, flags, security, ssid in self.wifi.networks(rescan=True):
            lines.append('NET %s %s %s %s' % (signal, flags, security, to_hex(ssid)))
        return lines + ['END']

    # ---- the worker thread, which runs one request at a time ----

    def queue_job(self, job):
        self.jobs.append(job)
        if not self.busy:
            self.next_job()

    def next_job(self):
        if self.jobs:
            self.busy = True
            threading.Thread(target=self.work, args=self.jobs.popleft(), daemon=True).start()

    def post(self, lines):
        self.results.put([printable(l) for l in lines])
        os.write(self.wake_w, b'x')

    def work(self, job, *args):
        try:
            if job == 'STATUS':
                lines = [self.status_line()]
            elif job == 'SCAN':
                self.post(['BUSY Scanning...'])
                lines = self.network_lines() + [self.status_line(), 'DONE 1 Scan complete']
            elif job == 'CONNECT':
                ssid, password = args
                self.post(['BUSY Connecting to %s...' % ssid])
                self.wifi.connect(ssid, password, lambda text: self.post(['BUSY ' + text]))
                lines = self.network_lines() + [self.status_line(), 'DONE 1 Connected to ' + ssid]
            elif job == 'FORGET':
                self.wifi.forget(args[0])
                lines = self.network_lines() + ['DONE 1 Forgot ' + args[0]]
        except Failed as e:
            lines = ['DONE 0 ' + sentence(str(e))]
            try:
                lines.insert(0, self.status_line())
            except Failed:
                pass
        except Exception:
            logger.exception('Unexpected error')
            lines = ['DONE 0 Unexpected error, see journalctl -u a314d on the Pi']
        lines.append('IDLE')
        self.post(lines)

    def handle_results(self):
        os.read(self.wake_r, 1024)
        while not self.results.empty():
            lines = self.results.get()
            if lines[-1] == 'IDLE':
                self.busy = False
                lines = lines[:-1]
                self.next_job()
            self.send(lines)

    # ---- messages from the Amiga ----

    def handle_command(self, line):
        words = line.split()
        if not words:
            return
        cmd = words[0]
        try:
            if cmd in ('STATUS', 'SCAN') and len(words) == 1:
                self.queue_job((cmd,))
            elif cmd == 'CONNECT' and len(words) in (2, 3):
                password = from_hex(words[2]) if len(words) == 3 else ''
                self.queue_job((cmd, from_hex(words[1]), password))
            elif cmd == 'FORGET' and len(words) == 2:
                self.queue_job((cmd, from_hex(words[1])))
        except ValueError:
            pass    # bad hex or UTF-8

    def handle_msg(self):
        stream_id, ptype, payload = self.a314d.wait_for_msg()

        if ptype == A314d.MSG_CONNECT:
            if payload != SERVICE_NAME.encode():
                self.a314d.send_connect_response(stream_id, 3)
                return
            if self.stream_id is not None:
                # There is only one Amiga, so the old connection is dead
                # (the Amiga was probably reset).
                self.a314d.send_reset(self.stream_id)
            logger.info('Amiga connected')
            self.a314d.send_connect_response(stream_id, 0)
            self.stream_id = stream_id
            self.inbuf = b''
        elif stream_id == self.stream_id:
            if ptype == A314d.MSG_DATA:
                self.inbuf += payload
                while b'\n' in self.inbuf:
                    line, self.inbuf = self.inbuf.split(b'\n', 1)
                    self.handle_command(line.decode('latin-1'))
                self.inbuf = self.inbuf[-512:]
            elif ptype in (A314d.MSG_EOS, A314d.MSG_RESET):
                if ptype == A314d.MSG_EOS:
                    self.a314d.send_reset(stream_id)
                logger.info('Amiga disconnected')
                self.stream_id = None
                self.jobs.clear()

    def run(self):
        logger.info('piwifi service is running')
        while True:
            fds = [self.a314d.drv, self.wake_r]
            if '-ondemand' not in sys.argv:
                fds.append(sys.stdin)
            for fd in select.select(fds, [], [])[0]:
                if fd == self.a314d.drv:
                    self.handle_msg()
                elif fd == self.wake_r:
                    self.handle_results()
                else:
                    line = sys.stdin.readline()
                    if not line or line.startswith('quit'):
                        return


if __name__ == '__main__':
    Service().run()
