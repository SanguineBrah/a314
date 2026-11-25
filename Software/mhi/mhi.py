#!/usr/bin/python3
# -*- coding: utf-8 -*-

# MHI service: decodes MPEG audio (MP1/MP2/MP3) for mhia314.library, an MHI
# driver for the Amiga, and plays the result through PiAudio. Players
# that support MHI, such as AmigaAMP, can then play MP3s without the
# Amiga doing any decoding.
#
# The Amiga copies the MPEG data that its player queues into a ring buffer
# in A314 memory, and says how much it has added. This service reads it
# from there (a314d's read memory request) as the decoder needs it, and
# says how much it has taken, so that the Amiga can give the buffers back
# to its player. The decoded audio, 8-bit stereo at PiAudio's rate, goes
# into the piaudio pipe, whose pace (that of the Amiga's audio hardware)
# sets how fast the stream is used.
#
# Decoding is done by libmpg123 (through ctypes), which also does the
# volume and the equalizer. Panning and cross-mixing are done here.
#
# The Amiga and this service exchange text lines, one per packet.
#
# Amiga -> Pi:
#   RING <address> <size>   the ring buffer (A314 address), sent first
#   DATA <n>                n more bytes were added to the ring
#   PLAY, PAUSE             start or pause playing
#   STOP                    stop, and throw away everything not yet played;
#                           the ring starts again at its beginning
#   PARAM <id> <value>      an MHI parameter (MHIP_..., values 0 to 100)
#
# Pi -> Amiga:
#   TAKEN <n>               n bytes of the ring were read and can be reused
#   DRY                     everything sent has been decoded
#   STOPPED                 the answer to STOP
#   PIPE <0|1>              whether the piaudio pipe is open (PiAudio runs)
#
# Without libmpg123 the connection is refused, so MHIAllocDecoder() fails.
# Without PiAudio taking the audio, it is thrown away at the pace it would
# have played at, so that players don't stall.

import ctypes
import ctypes.util
import errno
import logging
import math
import os
import select
import stat
import struct
import time

from a314d import A314d

logging.basicConfig(format = '%(levelname)s, %(asctime)s, %(name)s, line %(lineno)d: %(message)s')
logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

SERVICE_NAME = 'mhi'

PIPE_NAME = os.environ.get('MHI_PIPE', '/tmp/piaudio_pipe')
OUT_RATE = 18000                # the rate piaudio plays at, 8-bit stereo
# Decoded audio kept ready, 0.1 s: little, so that volume, tone and pause
# take effect quickly (the pipe and PiAudio hold a little more).
AHEAD = OUT_RATE // 5
READ_CHUNK = 2048               # most MPEG data read from the Amiga at once
MAX_LINE = 251

# libmpg123 (see mpg123.h and fmt123.h)
MPG123_OK, MPG123_NEED_MORE, MPG123_NEW_FORMAT, MPG123_DONE = 0, -10, -11, -12
MPG123_ADD_FLAGS, MPG123_FORCE_RATE = 2, 3
MPG123_FORCE_STEREO, MPG123_QUIET = 0x8, 0x20
MPG123_STEREO = 2
MPG123_ENC_SIGNED_8 = 0x82
MPG123_LR = 3
EQ_BANDS = 32

# MHI parameters (libraries/mhi.h)
MHIP_VOLUME, MHIP_PANNING, MHIP_CROSSMIXING = 0, 1, 2
MHIP_PREFACTOR = 6
# The tone controls and equalizer bands, by their centre frequency. The
# 3- and 5-band controls are the same ids as some of the 10 bands.
TONE_FREQS = {9: 32, 3: 64, 10: 125, 7: 250, 11: 500, 4: 1000, 12: 2000,
              8: 4000, 13: 8000, 5: 16000}
TONE_RANGE_DB = 12.0            # 0 and 100 are this much cut or boost


class Decoder(object):
    # An MPEG audio decoder fed with data as it arrives (libmpg123's feed
    # mode), giving 8-bit stereo at OUT_RATE.

    def __init__(self):
        name = ctypes.util.find_library('mpg123')
        if not name:
            raise OSError('libmpg123 is not installed')
        lib = ctypes.CDLL(name)
        lib.mpg123_new.restype = ctypes.c_void_p
        lib.mpg123_new.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
        lib.mpg123_delete.argtypes = [ctypes.c_void_p]
        # mpg123_param2 and mpg123_eq2 (what mpg123.h maps the plain names
        # to) only exist from libmpg123 1.32; the plain names in all
        self.param = getattr(lib, 'mpg123_param2', None) or lib.mpg123_param
        self.eq = getattr(lib, 'mpg123_eq2', None) or lib.mpg123_eq
        self.param.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_double]
        lib.mpg123_format_none.argtypes = [ctypes.c_void_p]
        lib.mpg123_format.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.c_int, ctypes.c_int]
        lib.mpg123_open_feed.argtypes = [ctypes.c_void_p]
        lib.mpg123_close.argtypes = [ctypes.c_void_p]
        lib.mpg123_feed.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
        lib.mpg123_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_size_t)]
        self.eq.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_double]
        lib.mpg123_volume.argtypes = [ctypes.c_void_p, ctypes.c_double]
        self.lib = lib
        err = ctypes.c_int(0)
        self.mh = lib.mpg123_new(None, ctypes.byref(err))
        if not self.mh:
            raise OSError('mpg123_new failed (%d)' % err.value)
        self.param(self.mh, MPG123_ADD_FLAGS, MPG123_FORCE_STEREO | MPG123_QUIET, 0.0)
        self.param(self.mh, MPG123_FORCE_RATE, OUT_RATE, 0.0)
        lib.mpg123_format_none(self.mh)
        if lib.mpg123_format(self.mh, OUT_RATE, MPG123_STEREO, MPG123_ENC_SIGNED_8) != MPG123_OK:
            raise OSError('libmpg123 cannot give 8-bit stereo at %d Hz' % OUT_RATE)
        self.out = ctypes.create_string_buffer(1024)   # about one MPEG frame
        self.done = ctypes.c_size_t(0)
        self.open()

    def open(self):
        self.lib.mpg123_open_feed(self.mh)

    def reset(self):
        # Forget the stream, for a new one
        self.lib.mpg123_close(self.mh)
        self.open()

    def feed(self, data):
        self.lib.mpg123_feed(self.mh, data, len(data))

    def read(self):
        # Returns decoded audio, or None when more input is needed
        while True:
            r = self.lib.mpg123_read(self.mh, self.out, len(self.out), ctypes.byref(self.done))
            if self.done.value:
                return self.out.raw[:self.done.value]
            if r == MPG123_NEW_FORMAT:
                continue
            if r in (MPG123_NEED_MORE, MPG123_DONE):
                return None
            if r != MPG123_OK:
                # A damaged stream: libmpg123 resyncs on the next frame
                return None

    def set_volume(self, factor):
        self.lib.mpg123_volume(self.mh, factor)

    def set_eq(self, factors):
        for band, f in enumerate(factors):
            self.eq(self.mh, MPG123_LR, band, f)

    def close(self):
        self.lib.mpg123_delete(self.mh)


def eq_factors(params):
    # The equalizer's 32 bands from the MHI tone controls, interpolated on
    # a log frequency scale. libmpg123's bands divide the stream's own
    # frequency range into 32; most MP3s are 44.1 kHz, so that is assumed.
    # Bass, mid and treble (64 Hz, 1 kHz, 16 kHz) are always points, at
    # 50 (flat) until set, so that setting only one of them shapes the
    # sound rather than changing all of it; the other bands count once set.
    # At 44.1 kHz libmpg123's first band covers 0-689 Hz, so bass settings
    # below that all act on that band.
    tones = dict.fromkeys((3, 4, 5), 50)
    tones.update((p, v) for p, v in params.items() if p in TONE_FREQS)
    points = sorted((TONE_FREQS[p], (v - 50) * TONE_RANGE_DB / 50)
        for p, v in tones.items())
    pre_db = (50 - params.get(MHIP_PREFACTOR, 50)) * TONE_RANGE_DB / 50
    factors = []
    for band in range(EQ_BANDS):
        f = (band + 0.5) * 44100 / 64
        if not points:
            db = 0.0
        elif f <= points[0][0]:
            db = points[0][1]
        elif f >= points[-1][0]:
            db = points[-1][1]
        else:
            for (f1, d1), (f2, d2) in zip(points, points[1:]):
                if f1 <= f <= f2:
                    t = (math.log(f) - math.log(f1)) / (math.log(f2) - math.log(f1))
                    db = d1 + t * (d2 - d1)
                    break
        factors.append(10 ** ((db + pre_db) / 20))
    return factors


def pan_mix(audio, panning, crossmixing):
    # Panning (0 left .. 50 centre .. 100 right) and cross-mixing (0 stereo
    # .. 100 mono) of 8-bit stereo
    left = audio[0::2]
    right = audio[1::2]
    lg = min(1.0, (100 - panning) / 50)
    rg = min(1.0, panning / 50)
    m = crossmixing / 200           # 100 gives half of each: mono
    out = bytearray(len(audio))
    for i, (l, r) in enumerate(zip(left, right)):
        l = l - 256 if l > 127 else l
        r = r - 256 if r > 127 else r
        nl = ((1 - m) * l + m * r) * lg
        nr = ((1 - m) * r + m * l) * rg
        out[2 * i] = int(nl) & 0xff
        out[2 * i + 1] = int(nr) & 0xff
    return bytes(out)


class Player(object):
    # One MHI decoder handle on the Amiga

    def __init__(self, service, stream_id):
        self.service = service
        self.stream_id = stream_id
        self.ring = None            # (address, size)
        self.rpos = 0               # where the next read is in the ring
        self.avail = 0              # bytes added by the Amiga, not yet read
        self.playing = False
        self.dry_sent = False
        self.audio = b''            # decoded, waiting for the pipe
        self.params = {}
        self.decoder = None
        try:
            self.decoder = Decoder()
        except (OSError, AttributeError) as e:
            logger.error('Unable to start the decoder: %s', e)

    def send(self, line):
        self.service.send(self.stream_id, line.encode('latin-1')[:MAX_LINE])

    def command(self, line):
        words = line.split()
        if not words:
            return
        cmd, args = words[0], words[1:]
        try:
            nums = [int(a) for a in args]
        except ValueError:
            logger.warning('Bad command %r', line[:40])
            return
        if cmd == 'RING' and len(nums) == 2 and nums[1] > 0:
            self.ring = (nums[0], nums[1])
            self.rpos = self.avail = 0
        elif cmd == 'DATA' and len(nums) == 1 and self.ring:
            self.avail = min(self.avail + nums[0], self.ring[1])
            self.dry_sent = False
        elif cmd == 'PLAY':
            self.playing = True
            self.dry_sent = False   # say DRY again if there's still nothing
        elif cmd == 'PAUSE':
            self.playing = False
        elif cmd == 'STOP':
            self.playing = False
            self.audio = b''
            self.rpos = self.avail = 0
            self.dry_sent = False
            if self.decoder:
                self.decoder.reset()
            self.send('STOPPED')
        elif cmd == 'PARAM' and len(nums) == 2:
            self.set_param(nums[0], max(0, min(100, nums[1])))
        else:
            logger.warning('Unknown command %r', line[:40])

    def set_param(self, param, value):
        self.params[param] = value
        if not self.decoder:
            return
        if param == MHIP_VOLUME:
            self.decoder.set_volume(value / 100)
        elif param in TONE_FREQS or param == MHIP_PREFACTOR:
            self.decoder.set_eq(eq_factors(self.params))

    def read_ring(self):
        # Reads the next piece of MPEG data from the Amiga's ring buffer
        address, size = self.ring
        n = min(self.avail, READ_CHUNK, size - self.rpos)
        data = self.service.read_mem(address + self.rpos, n)
        self.rpos = (self.rpos + n) % size
        self.avail -= n
        self.send('TAKEN %d' % n)
        return data

    def decode(self):
        # Keeps AHEAD bytes of audio ready while playing
        if not self.decoder or not self.playing:
            return
        while len(self.audio) < AHEAD:
            pcm = self.decoder.read()
            if pcm is not None:
                pan = self.params.get(MHIP_PANNING, 50)
                cross = self.params.get(MHIP_CROSSMIXING, 0)
                if pan != 50 or cross:
                    pcm = pan_mix(pcm, pan, cross)
                self.audio += pcm
            elif self.avail and self.ring:
                self.decoder.feed(self.read_ring())
            else:
                # DRY once everything decoded has gone to the pipe, so
                # that a player that stops at DRY doesn't cut the end off
                if not self.dry_sent and not self.audio:
                    self.dry_sent = True
                    self.send('DRY')
                return

    def wants_pipe(self):
        return self.playing and bool(self.audio)

    def drop_audio(self, nbytes):
        # Throws away up to nbytes of decoded audio (decoding more as
        # needed); returns how many were thrown away
        dropped = 0
        while dropped < nbytes:
            if not self.audio:
                self.decode()
                if not self.audio:
                    break
            n = min(nbytes - dropped, len(self.audio)) & ~1
            if n == 0:
                break
            self.audio = self.audio[n:]
            dropped += n
        return dropped

    def to_pipe(self, fd):
        try:
            n = os.write(fd, self.audio)
        except BlockingIOError:
            return True
        except OSError:
            return False            # PiAudio went away
        self.audio = self.audio[n:]
        return True

    def close(self):
        if self.decoder:
            self.decoder.close()
            self.decoder = None


class MhiService(object):
    def __init__(self):
        self.a314d = A314d(SERVICE_NAME)
        self.players = {}           # stream id -> Player
        self.pending = []           # messages that came while reading memory
        self.pipe = None
        self.pipe_tried = 0
        self.pipe_status = None
        self.last_output = time.time()  # when audio last went to the pipe
        self.dropping_since = None      # see run()

    def send(self, stream_id, data):
        self.a314d.send_data(stream_id, data)

    def read_mem(self, address, length):
        # Like A314d.read_mem, but keeps the messages that arrive before
        # the answer, instead of failing on them.
        self.a314d.send_read_mem_req(address, length)
        while True:
            stream_id, ptype, payload = self.a314d.wait_for_msg()
            if ptype == A314d.MSG_READ_MEM_RES:
                return payload
            self.pending.append((stream_id, ptype, payload))

    def handle_msg(self, stream_id, ptype, payload):
        if ptype == A314d.MSG_CONNECT:
            if payload == SERVICE_NAME.encode() and not self.players:
                player = Player(self, stream_id)
                if player.decoder is None:
                    # No libmpg123: refused, so that MHIAllocDecoder fails
                    self.a314d.send_connect_response(stream_id, 3)
                    return
                self.players[stream_id] = player
                self.a314d.send_connect_response(stream_id, 0)
                self.pipe_status = None     # tell the new player
            else:
                # One player at a time: there is one piaudio pipe
                self.a314d.send_connect_response(stream_id, 3)
            return
        player = self.players.get(stream_id)
        if player is None:
            return
        if ptype == A314d.MSG_DATA:
            player.command(payload.decode('latin-1'))
        elif ptype in (A314d.MSG_EOS, A314d.MSG_RESET):
            if ptype == A314d.MSG_EOS:
                self.a314d.send_eos(stream_id)
            player.close()
            del self.players[stream_id]

    def open_pipe(self):
        if self.pipe is not None or time.time() - self.pipe_tried < 1:
            return
        self.pipe_tried = time.time()
        try:
            if not stat.S_ISFIFO(os.stat(PIPE_NAME).st_mode):
                logger.error('%s is not a named pipe', PIPE_NAME)
                return
            self.pipe = os.open(PIPE_NAME, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as e:
            # ENXIO: PiAudio isn't running; ENOENT: it never has
            if e.errno not in (errno.ENXIO, errno.ENOENT):
                logger.warning('Unable to open %s: %s', PIPE_NAME, e)

    def close_pipe(self):
        if self.pipe is not None:
            os.close(self.pipe)
            self.pipe = None

    def run(self):
        while True:
            while self.pending:
                self.handle_msg(*self.pending.pop(0))
            for p in self.players.values():
                p.decode()
            if self.players:
                self.open_pipe()
            else:
                self.close_pipe()
            status = int(self.pipe is not None)
            if status != self.pipe_status:
                self.pipe_status = status
                for p in self.players.values():
                    p.send('PIPE %d' % status)
            writers = [self.pipe] if self.pipe is not None and \
                any(p.wants_pipe() for p in self.players.values()) else []
            timeout = 0.05 if self.dropping_since else 0.25
            rl, wl, _ = select.select([self.a314d], writers, [], timeout)
            if self.a314d in rl:
                self.handle_msg(*self.a314d.wait_for_msg())
            # With no PiAudio taking the audio for a second, it is thrown
            # away at the pace it would have played at, so that the player
            # still sees the stream move on and end (silently).
            now = time.time()
            if wl:
                for p in list(self.players.values()):
                    if p.wants_pipe() and not p.to_pipe(self.pipe):
                        self.close_pipe()
                        break
                self.last_output = now
                self.dropping_since = None
            elif not any(p.wants_pipe() for p in self.players.values()):
                self.last_output = now
                self.dropping_since = None
            elif now - self.last_output > 1.0:
                if self.dropping_since is None:
                    self.dropping_since = now
                    self.dropped = 0
                owed = int((now - self.dropping_since) * 2 * OUT_RATE) - self.dropped
                for p in self.players.values():
                    self.dropped += p.drop_audio(owed)


if __name__ == '__main__':
    service = MhiService()
    logger.info('%s service is running', SERVICE_NAME)
    try:
        service.run()
    except KeyboardInterrupt:
        pass
    service.a314d.close()
