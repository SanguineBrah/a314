#!/usr/bin/python3
# -*- coding: utf-8 -*-

# PiSerial service: lets the Amiga use serial ports on the Raspberry Pi, such
# as USB serial adapters, through a314serial.device. Amiga programs open
# a314serial.device instead of serial.device; everything else is the same.
#
# Each open unit of a314serial.device is one connection to this service.
# Which Pi port a unit uses is set on the Amiga (Prefs/A314/Serial), and is
# sent here when the unit is opened, so nothing is stored on the Pi. Ports
# are named by their /dev/serial/by-id name where they have one, which stays
# the same whatever /dev/ttyUSB<n> name the port gets. A port that is
# unplugged while open is opened again when it comes back.
#
# Every packet starts with a type byte:
#   'D' + bytes             serial data
#   'C' + text              a command or reply (one per packet)
#
# Amiga -> Pi commands:
#   PORTS                   list the ports
#   OPEN <port> <baud> <bits> <parity> <stop> <handshake> <window>
#                           open a port ('auto': the first free one).
#                           parity: n, e, o, m or s; handshake: none, rts
#                           or xon. <window> is how much data the Amiga
#                           can take before it gives more CREDIT.
#   SET <baud> <bits> <parity> <stop> <handshake>   change the settings
#   CREDIT <n>              the Amiga has read n more bytes
#   BREAK <microseconds>    send a break
#   FLUSH                   throw away data not yet sent or received
#
# Pi -> Amiga replies:
#   PORT <free> <port> <description>, then END   the port list
#   OPENED <port>           OPEN worked
#   OK                      SET worked
#   ERR <code> <text>       OPEN or SET failed; code is a serial.device
#                           error (1 busy, 2 baud rate, 5 bad parameter,
#                           and 32 for no such port)
#   SENT <n>                n more bytes of Amiga data went to the port.
#                           The Amiga sends at most TX_WINDOW bytes that
#                           aren't yet SENT, and never more data than
#                           CREDIT allows comes back, so neither side can
#                           be flooded.
#   LINES <cts> <dsr> <cd> <ri>   modem lines (1 = on), when they change
#   GONE                    the port was unplugged
#   BACK                    and is back

import errno
import fcntl
import glob
import logging
import os
import select
import struct
import termios
import time

from a314d import A314d

logging.basicConfig(format = '%(levelname)s, %(asctime)s, %(name)s, line %(lineno)d: %(message)s')
logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

SERVICE_NAME = 'piserial'

BY_ID_DIR = os.environ.get('A314_SERIAL_BY_ID', '/dev/serial/by-id')
OTHER_PORTS = os.environ.get('A314_SERIAL_PORTS', '/dev/ttyUSB* /dev/ttyACM*').split()

MAX_DATA = 251              # data bytes in one packet (252 with the type)
TX_WINDOW = 4096            # Amiga data in flight, must match the device
RX_BUFFER = 4096            # data read from the port but not yet sent
POLL_SECONDS = 0.25         # how often modem lines and unplugged ports are checked

CMSPAR = 0o10000000000      # mark/space parity (Linux), missing from termios

ERR_BUSY, ERR_BAUD, ERR_PARAM, ERR_NO_PORT = 1, 2, 5, 32


def find_ports():
    # Returns {name: device path}. By-id names come first; ports without
    # one are named after their device (ttyUSB0).
    ports = {}
    seen = set()
    for link in sorted(glob.glob(os.path.join(BY_ID_DIR, '*'))):
        dev = os.path.realpath(link)
        ports[os.path.basename(link)] = dev
        seen.add(dev)
    for pattern in OTHER_PORTS:
        for dev in sorted(glob.glob(pattern)):
            if os.path.realpath(dev) not in seen:
                ports[os.path.basename(dev)] = dev
    return ports


def describe(name):
    # A shorter, readable description from a by-id name such as
    # usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0
    d = name
    if d.startswith('usb-'):
        d = d[4:]
    for suffix in ('-if00-port0', '-if00'):
        if d.endswith(suffix):
            d = d[:-len(suffix)]
    return d.replace('_', ' ')[:60]


class Settings(object):
    # The serial settings, as sent by the Amiga

    def __init__(self, words):
        if len(words) != 5:
            raise ValueError
        self.baud = int(words[0])
        self.bits = int(words[1])
        self.parity = words[2]
        self.stop = int(words[3])
        self.handshake = words[4]
        if self.bits not in (5, 6, 7, 8) or self.stop not in (1, 2) or \
                self.parity not in ('n', 'e', 'o', 'm', 's') or \
                self.handshake not in ('none', 'rts', 'xon'):
            raise ValueError

    def apply(self, fd):
        # Raises OSError if the port doesn't accept them, KeyError if the
        # baud rate isn't one Linux knows.
        speed = getattr(termios, 'B%d' % self.baud, None)
        if speed is None:
            raise KeyError(self.baud)
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)
        iflag = termios.IGNBRK if self.parity == 'n' else termios.IGNBRK | termios.INPCK
        if self.handshake == 'xon':
            iflag |= termios.IXON | termios.IXOFF
        oflag = 0
        lflag = 0
        cflag = termios.CREAD | termios.CLOCAL | \
            {5: termios.CS5, 6: termios.CS6, 7: termios.CS7, 8: termios.CS8}[self.bits]
        if self.stop == 2:
            cflag |= termios.CSTOPB
        if self.parity != 'n':
            cflag |= termios.PARENB
        if self.parity in ('o', 'm'):
            cflag |= termios.PARODD
        if self.parity in ('m', 's'):
            cflag |= CMSPAR
        if self.handshake == 'rts':
            cflag |= termios.CRTSCTS
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW,
            [iflag, oflag, cflag, lflag, speed, speed, cc])


class Port(object):
    # One unit of a314serial.device, connected to a Pi serial port

    def __init__(self, service, stream_id):
        self.service = service
        self.stream_id = stream_id
        self.name = None
        self.settings = None
        self.fd = None
        self.gone = False
        self.credit = 0             # bytes the Amiga can still take
        self.rx = b''               # read from the port, not yet sent
        self.tx = b''               # from the Amiga, not yet written
        self.lines = None

    def send_line(self, text):
        self.service.send(self.stream_id, b'C' + text.encode('latin-1'))

    def open_device(self):
        dev = find_ports().get(self.name)
        if dev is None:
            raise FileNotFoundError(self.name)
        fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.ioctl(fd, termios.TIOCEXCL)   # no other opens, except by root
            self.settings.apply(fd)
        except Exception:
            self.release(fd)
            raise
        self.fd = fd

    @staticmethod
    def release(fd):
        # TIOCEXCL can outlive the close (it does on a pty), so it is
        # cleared first.
        try:
            fcntl.ioctl(fd, termios.TIOCNXCL)
        except OSError:
            pass
        os.close(fd)

    def close_device(self):
        if self.fd is not None:
            self.release(self.fd)
            self.fd = None

    def open(self, words):
        if self.name is not None or len(words) != 7:
            self.send_line('ERR %d bad OPEN' % ERR_PARAM)
            return
        try:
            settings = Settings(words[1:6])
            window = int(words[6])
        except ValueError:
            self.send_line('ERR %d bad settings' % ERR_PARAM)
            return
        name = words[0]
        if name in self.service.names_in_use():
            self.send_line('ERR %d %s is in use' % (ERR_BUSY, name))
            return
        if name == 'auto':
            free = [n for n in find_ports() if n not in self.service.names_in_use()]
            if not free:
                self.send_line('ERR %d no free serial port' % ERR_NO_PORT)
                return
            name = free[0]
        self.name, self.settings = name, settings
        try:
            self.open_device()
        except FileNotFoundError:
            self.name = None
            self.send_line('ERR %d no port %s' % (ERR_NO_PORT, name))
            return
        except KeyError:
            self.name = None
            self.send_line('ERR %d baud rate %d not supported' % (ERR_BAUD, settings.baud))
            return
        except OSError as e:
            self.name = None
            code = ERR_BUSY if e.errno in (errno.EBUSY, errno.EACCES, errno.EPERM) else ERR_PARAM
            self.send_line('ERR %d %s: %s' % (code, name, e.strerror))
            return
        self.credit = max(0, window)
        logger.info('Unit opened %s', name)
        self.send_line('OPENED ' + name)
        self.poll()

    def set(self, words):
        try:
            settings = Settings(words)
        except ValueError:
            self.send_line('ERR %d bad settings' % ERR_PARAM)
            return
        if self.fd is not None:
            try:
                settings.apply(self.fd)
            except KeyError:
                self.send_line('ERR %d baud rate %d not supported' % (ERR_BAUD, settings.baud))
                return
            except OSError as e:
                self.send_line('ERR %d %s' % (ERR_PARAM, e.strerror))
                return
        self.settings = settings
        self.send_line('OK')

    def lost(self):
        # The port was unplugged (or failed). Data for it is thrown away,
        # but reported as sent so that the Amiga doesn't wait for ever.
        logger.warning('Lost %s', self.name)
        self.close_device()
        self.gone = True
        self.send_line('GONE')
        self.drop_tx()

    def drop_tx(self):
        if self.tx:
            self.send_line('SENT %d' % len(self.tx))
            self.tx = b''

    def command(self, text):
        words = text.split()
        if not words:
            return
        cmd, args = words[0], words[1:]
        if cmd == 'PORTS':
            in_use = self.service.names_in_use()
            for name in find_ports():
                self.send_line('PORT %d %s %s' % (name not in in_use, name, describe(name)))
            self.send_line('END')
        elif cmd == 'OPEN':
            self.open(args)
        elif self.name is None:
            logger.warning('%s before OPEN', cmd)
        elif cmd == 'SET':
            self.set(args)
        elif cmd == 'CREDIT' and len(args) == 1 and args[0].isdigit():
            self.credit += int(args[0])
            self.flush_rx()
        elif cmd == 'BREAK' and len(args) == 1 and args[0].isdigit():
            if self.fd is not None:
                # tcsendbreak() only knows "about 0.25 s", so this is a
                # break of at least the requested length.
                try:
                    fcntl.ioctl(self.fd, termios.TIOCSBRK)
                    time.sleep(min(int(args[0]), 2000000) / 1000000)
                    fcntl.ioctl(self.fd, termios.TIOCCBRK)
                except OSError:
                    pass
        elif cmd == 'FLUSH':
            self.rx = b''
            self.drop_tx()
            if self.fd is not None:
                try:
                    termios.tcflush(self.fd, termios.TCIOFLUSH)
                except termios.error:
                    pass
        else:
            logger.warning('Unknown command %r', text[:40])

    def data(self, payload):
        if self.name is None:
            return
        if self.gone:
            self.send_line('SENT %d' % len(payload))
            return
        if len(self.tx) + len(payload) > TX_WINDOW:
            logger.error('The Amiga sent more than TX_WINDOW bytes, dropping some')
            payload = payload[:TX_WINDOW - len(self.tx)]
        self.tx += payload
        self.write_some()

    def write_some(self):
        if self.fd is None or not self.tx:
            return
        try:
            n = os.write(self.fd, self.tx)
        except BlockingIOError:
            return
        except OSError:
            self.lost()
            return
        self.tx = self.tx[n:]
        if n:
            self.send_line('SENT %d' % n)

    def read_some(self):
        try:
            d = os.read(self.fd, RX_BUFFER - len(self.rx))
        except BlockingIOError:
            return
        except OSError:
            self.lost()
            return
        if not d:
            self.lost()
            return
        self.rx += d
        self.flush_rx()

    def flush_rx(self):
        while self.rx and self.credit > 0:
            n = min(len(self.rx), self.credit, MAX_DATA)
            self.service.send(self.stream_id, b'D' + self.rx[:n])
            self.rx = self.rx[n:]
            self.credit -= n

    def wants_read(self):
        return self.fd is not None and len(self.rx) < RX_BUFFER

    def wants_write(self):
        return self.fd is not None and bool(self.tx)

    def poll(self):
        # Checks the modem lines, and whether an unplugged port is back
        if self.gone:
            try:
                self.open_device()
            except (OSError, KeyError):
                return
            logger.info('%s is back', self.name)
            self.gone = False
            self.send_line('BACK')
        if self.fd is None:
            return
        try:
            bits = struct.unpack('i', fcntl.ioctl(self.fd, termios.TIOCMGET, b'\0' * 4))[0]
            lines = tuple(int(bool(bits & b)) for b in
                (termios.TIOCM_CTS, termios.TIOCM_DSR, termios.TIOCM_CAR, termios.TIOCM_RNG))
        except OSError as e:
            if e.errno in (errno.EIO, errno.ENXIO, errno.ENODEV):
                self.lost()
                return
            lines = (1, 1, 1, 0)    # a port without modem lines (a pty)
        if lines != self.lines:
            self.lines = lines
            self.send_line('LINES %d %d %d %d' % lines)

    def close(self):
        if self.name:
            logger.info('Unit closed %s', self.name)
        self.close_device()


class SerialService(object):
    def __init__(self):
        self.a314d = A314d(SERVICE_NAME)
        self.ports = {}             # stream id -> Port

    def send(self, stream_id, payload):
        self.a314d.send_data(stream_id, payload)

    def names_in_use(self):
        return {p.name for p in self.ports.values() if p.name}

    def handle_msg(self):
        stream_id, ptype, payload = self.a314d.wait_for_msg()
        if ptype == A314d.MSG_CONNECT:
            if payload == SERVICE_NAME.encode():
                self.ports[stream_id] = Port(self, stream_id)
                self.a314d.send_connect_response(stream_id, 0)
            else:
                self.a314d.send_connect_response(stream_id, 3)
            return
        port = self.ports.get(stream_id)
        if port is None:
            return
        if ptype == A314d.MSG_DATA and payload:
            if payload[:1] == b'D':
                port.data(payload[1:])
            elif payload[:1] == b'C':
                port.command(payload[1:].decode('latin-1'))
        elif ptype in (A314d.MSG_EOS, A314d.MSG_RESET):
            if ptype == A314d.MSG_EOS:
                self.a314d.send_eos(stream_id)
            port.close()
            del self.ports[stream_id]

    def run(self):
        next_poll = time.time()
        while True:
            readers = [self.a314d] + [p.fd for p in self.ports.values() if p.wants_read()]
            writers = [p.fd for p in self.ports.values() if p.wants_write()]
            timeout = max(0, next_poll - time.time())
            rl, wl, _ = select.select(readers, writers, [], timeout)
            if self.a314d in rl:
                self.handle_msg()
            for port in list(self.ports.values()):
                if port.fd in wl:
                    port.write_some()
                if port.fd in rl:
                    port.read_some()
            if time.time() >= next_poll:
                next_poll = time.time() + POLL_SECONDS
                for port in self.ports.values():
                    port.poll()


if __name__ == '__main__':
    service = SerialService()
    logger.info('%s service is running', SERVICE_NAME)
    try:
        service.run()
    except KeyboardInterrupt:
        pass
    service.a314d.close()
