#!/usr/bin/python3
# -*- coding: utf-8 -*-

# PiRadio service: lets the PiRadio program on the Amiga control an RTL-SDR
# dongle attached to the Raspberry Pi.
#
# rtl_fm demodulates the signal and rtl_power sweeps the band to find active
# channels (for seek and scan). The audio is converted to the format the
# piaudio service expects (signed 8-bit stereo, 18000 Hz) and written to the
# piaudio named pipe, so PiAudio must be running on the Amiga as well.
#
# The Amiga and this service exchange text lines ending in '\n'.
#
# Amiga -> Pi:
#   HELLO               send the band list and current state, start receiving
#   REGION <i>          select region i (its band list)
#   BAND <i>            select band i
#   FREQ <hz>           tune (switches band if hz is outside the current one)
#   SQL <level>         rtl_fm squelch level, 0 = off
#   GAIN <tenths>       tuner gain in tenths of a dB, -1 = automatic
#   VOL <0..16>         volume, 8 = unchanged
#   SEEK <1|-1>         go to the next active channel up or down
#   SCAN <1|-1|0>       scan up or down through the active channels, or stop
#   SWEEP               sweep the band again for active channels
#
# Pi -> Amiga:
#   REGIONS <n>
#   R <i> <name>
#   REG <i>             the current region
#   BANDS <n>
#   B <i> <lo> <hi> <step> <mode> <short> <name>
#   GAINS <n> <g0> <g1> ...
#   ST <band> <freq> <sql> <gain> <vol> <scan>
#   RUN <0 starting | 1 receiving | 2 error | 3 sweeping>
#   PIPE <0|1>          whether the piaudio pipe is open
#   LVL <0..99>         audio level
#   ACTCLR <band>       forget the active channels of a band
#   ACT <band> <ch>...  active channels, as channel numbers from the band's lo
#   MSG <text>          status message (no text clears it)
#   HB                  heartbeat, every 2 seconds

import errno
import json
import logging
import os
import select
import stat
import subprocess
import sys
import time

from a314d import A314d

logging.basicConfig(format = '%(levelname)s, %(asctime)s, %(name)s, line %(lineno)d: %(message)s')
logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

SERVICE_NAME = 'piradio'
CONFIG_FILE = '/etc/opt/a314/piradio.conf'
STATE_FILE = os.path.expanduser('~/.config/piradio/state.json')

OUT_RATE = 18000                # sample rate piaudio plays at
MAX_AUDIO_BUFFER = OUT_RATE     # bytes of 8-bit stereo, 0.5 s
MAX_CHUNK = 200                 # largest MSG_DATA sent to the Amiga
MAX_BANDS = 10                  # limits of the Amiga program
MAX_REGIONS = 10
MAX_GAINS = 40
MAX_ACTIVE = 200
MODES = ('wbfm', 'fm', 'am', 'usb', 'lsb', 'raw')

RETUNE_DELAY = 0.2              # wait for more changes before restarting rtl_fm
RETRY_DELAY = 5.0               # after an error
SWEEP_TIMEOUT = 30.0
NO_AUDIO_TIMEOUT = 5.0          # rtl_fm hung (only checked with squelch off)
ACTIVITY_MAX_AGE = 300.0        # sweeps older than this are redone
HEARTBEAT_INTERVAL = 2.0
NOTICE_TIME = 4.0

# Friendlier text for common rtl_fm and rtl_power errors
ERROR_HINTS = [
    ('No supported devices', 'NO RTL-SDR DONGLE FOUND'),
    ('usb_claim_interface', 'THE DONGLE IS IN USE BY ANOTHER PROGRAM'),
    ('LIBUSB_ERROR_NO_DEVICE', 'THE DONGLE WAS DISCONNECTED'),
    ('Failed to open', 'CANNOT OPEN THE DONGLE'),
]

SETTINGS = {
    'rtl_fm': 'rtl_fm',
    'rtl_power': 'rtl_power',
    'device_index': 0,
    'ppm': 0,
    'audio_pipe': '/tmp/piaudio_pipe',
    'sweep_threshold_db': 8.0,
    'scan_dwell_seconds': 6.0,
    'scan_hang_seconds': 2.5,
}

DEFAULT_GAINS = [0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254,
    280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496]

DEFAULT_BAND = {'short': 'FM', 'name': 'FM Broadcast', 'lo': 87500000,
    'hi': 108000000, 'step': 100000, 'mode': 'wbfm'}


class Band(object):
    def __init__(self, d):
        self.short = str(d.get('short', '?'))[:6].replace(' ', '_')
        self.name = str(d.get('name', self.short))[:30]
        self.lo = int(d['lo'])
        self.hi = int(d['hi'])
        self.step = int(d.get('step', 25000))
        self.mode = str(d.get('mode', 'fm'))
        self.squelch = int(d.get('squelch', 0))
        self.args = [str(a) for a in d.get('args', [])]
        if not (0 < self.lo < self.hi <= 2000000000) or self.step < 1:
            raise ValueError('bad lo, hi or step')
        if self.mode not in MODES:
            raise ValueError('unknown mode ' + self.mode)

        # rtl_fm can only reduce the sample rate by a whole number, so the
        # demodulation rate must be a multiple of the output rate.
        want = int(d.get('rate', 180000 if self.mode == 'wbfm' else OUT_RATE))
        self.rate = max(1, -(-want // OUT_RATE)) * OUT_RATE

    def channels(self):
        return (self.hi - self.lo) // self.step + 1

    def channel_of(self, freq):
        return (freq - self.lo + self.step // 2) // self.step

    def freq_of(self, ch):
        return self.lo + max(0, min(self.channels() - 1, ch)) * self.step

    def snap(self, freq):
        return self.freq_of(self.channel_of(freq))

    def contains(self, freq):
        return self.lo <= freq <= self.hi


def read_bands(region, band_list):
    bands = []
    if not isinstance(band_list, list):
        band_list = []
    for i, d in enumerate(band_list):
        try:
            bands.append(Band(d))
        except (TypeError, ValueError, KeyError, AttributeError) as e:
            logger.error('Ignoring band %d of %s in %s: %s', i + 1, region, CONFIG_FILE, e)
    if len(bands) > MAX_BANDS:
        logger.warning('Only the first %d bands of %s are used', MAX_BANDS, region)
    return bands[:MAX_BANDS]


def load_config():
    try:
        with open(CONFIG_FILE) as f:
            user = json.load(f)
        if not isinstance(user, dict):
            raise ValueError('not a JSON object')
    except FileNotFoundError:
        logger.warning('No %s, using default settings', CONFIG_FILE)
        user = {}
    except Exception as e:
        logger.error('Unable to read %s (%s), using default settings', CONFIG_FILE, e)
        user = {}

    cfg = {}
    for key, default in SETTINGS.items():
        try:
            cfg[key] = type(default)(user.get(key, default))
        except (TypeError, ValueError):
            logger.error('Bad value for %s in %s, using %r', key, CONFIG_FILE, default)
            cfg[key] = default

    try:
        cfg['gains'] = sorted(int(g) for g in user.get('gains', DEFAULT_GAINS) if int(g) >= 0)[:MAX_GAINS]
    except (TypeError, ValueError):
        logger.error('Bad gains list in %s, using the default list', CONFIG_FILE)
        cfg['gains'] = DEFAULT_GAINS

    # "regions" maps a region name to its band list. A plain "bands" list
    # (as in older configurations) is a single region.
    regions = user.get('regions')
    if not isinstance(regions, dict):
        regions = {'Custom': user.get('bands', [])}
    cfg['regions'] = []
    for name, band_list in list(regions.items())[:MAX_REGIONS]:
        bands = read_bands(str(name), band_list)
        if bands:
            cfg['regions'].append((str(name)[:30], bands))
    if not cfg['regions']:
        cfg['regions'] = [('Default', [Band(DEFAULT_BAND)])]
    return cfg


class Proc(object):
    # A child process (rtl_fm or rtl_power) with non-blocking output pipes.

    def __init__(self, argv):
        logger.info('Starting %s', ' '.join(argv))
        self.p = subprocess.Popen(argv, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.out = self.p.stdout.fileno()
        self.err = self.p.stderr.fileno()
        os.set_blocking(self.out, False)
        os.set_blocking(self.err, False)
        self.err_text = b''
        self.started = time.time()

    def read(self, fd, size):
        try:
            data = os.read(fd, size)
        except BlockingIOError:
            return None
        if fd == self.err:
            self.err_text = (self.err_text + data)[-2000:]
        return data

    def error_text(self):
        text = self.err_text.decode('latin-1')
        for key, hint in ERROR_HINTS:
            if key in text:
                return hint
        lines = text.split('\n')
        lines = [l.strip() for l in lines if l.strip()]
        return lines[-1].upper()[:60] if lines else 'EXITED UNEXPECTEDLY'

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                self.p.kill()
                try:
                    self.p.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    # Stuck in USB I/O. The subprocess module reaps it later.
                    logger.error('Process %d does not exit', self.p.pid)
        self.p.stdout.close()
        self.p.stderr.close()


def volume_table(vol):
    # Maps a signed 8-bit sample (as a byte) to the sample at volume vol/8.
    t = bytearray(256)
    for i in range(256):
        s = (((i + 128) & 255) - 128) * vol // 8
        t[i] = max(-128, min(127, s)) & 255
    return bytes(t)

# Maps a signed 8-bit sample (as a byte) to its size.
ABS_TABLE = bytes(min(abs(((i + 128) & 255) - 128), 99) for i in range(256))


class Radio(object):
    def __init__(self, cfg, send):
        self.cfg = cfg
        self.send = send            # function that sends a line to the Amiga
        self.regions = cfg['regions']   # [(name, [Band])]
        self.region = 0
        self.bands = self.regions[0][1]
        self.band = 0
        self.freqs = {}             # band index -> frequency
        self.sqls = {}              # band index -> squelch level
        self.saved = {}             # region name -> its band, freqs and sqls
        self.gain = -1
        self.vol = 8
        self.load_state()

        self.active = False         # an Amiga is connected and said HELLO
        self.fm = None              # rtl_fm process
        self.sweep = None           # rtl_power process
        self.sweep_out = b''
        self.after_sweep = None     # 'seek', 'scan' or None
        self.scan_dir = 0
        self.seek_dir = 1
        self.activity = {}          # band index -> (time, [channels])
        self.retune_at = None       # when to restart rtl_fm
        self.error = None
        self.notice = ''
        self.notice_until = 0
        self.run_status = None
        self.pipe = None
        self.pipe_status = None
        self.pipe_tried_at = 0
        self.audio = b''            # converted audio waiting for the pipe
        self.odd_byte = b''
        self.last_audio = 0
        self.last_open = 0          # last time the squelch was open
        self.tuned_at = 0
        self.level_sum = 0
        self.level_n = 0
        self.level = None
        self.level_at = 0
        self.msg = None
        self.state_changed = False

    # ---- settings and state ----

    @property
    def cur(self):
        return self.bands[self.band]

    @property
    def freq(self):
        return self.freqs.get(self.band, self.cur.lo)

    @freq.setter
    def freq(self, f):
        self.freqs[self.band] = self.cur.snap(f)
        self.state_changed = True

    @property
    def sql(self):
        return self.sqls.get(self.band, self.cur.squelch)

    @sql.setter
    def sql(self, level):
        self.sqls[self.band] = max(0, min(999, level))
        self.state_changed = True

    def set_region(self, i):
        # Keeps where we were in this region, and goes back to where we
        # were in the other one.
        self.saved[self.regions[self.region][0]] = {
            'band': self.band, 'freqs': self.freqs, 'sqls': self.sqls}
        self.region = i
        self.bands = self.regions[i][1]
        self.band, self.freqs, self.sqls = 0, {}, {}
        s = self.saved.get(self.regions[i][0])
        if s:
            try:
                n = len(self.bands)
                self.band = int(s['band']) if 0 <= int(s['band']) < n else 0
                self.freqs = {int(k): self.bands[int(k)].snap(int(v))
                    for k, v in s['freqs'].items() if 0 <= int(k) < n}
                self.sqls = {int(k): int(v) for k, v in s['sqls'].items() if 0 <= int(k) < n}
            except (TypeError, ValueError, KeyError, AttributeError) as e:
                logger.warning('Ignoring the saved state of %s: %s', self.regions[i][0], e)
        self.activity = {}
        self.state_changed = True

    def load_state(self):
        try:
            with open(STATE_FILE) as f:
                s = json.load(f)
            self.gain = int(s['gain'])
            self.vol = max(0, min(16, int(s['vol'])))
            self.saved = dict(s.get('regions', {}))
            names = [r[0] for r in self.regions]
            if s.get('region') in names:
                self.set_region(names.index(s['region']))
            else:
                self.set_region(0)
        except FileNotFoundError:
            pass
        except Exception as e:
            logger.warning('Ignoring %s: %s', STATE_FILE, e)
            self.gain, self.vol, self.saved = -1, 8, {}
            self.set_region(0)
        self.state_changed = False
        self.vol_table = volume_table(self.vol)

    def save_state(self):
        if not self.state_changed:
            return
        self.saved[self.regions[self.region][0]] = {
            'band': self.band, 'freqs': self.freqs, 'sqls': self.sqls}
        s = {'region': self.regions[self.region][0], 'regions': self.saved,
            'gain': self.gain, 'vol': self.vol}
        try:
            os.makedirs(os.path.dirname(STATE_FILE), exist_ok=True)
            with open(STATE_FILE + '.tmp', 'w') as f:
                json.dump(s, f)
            os.replace(STATE_FILE + '.tmp', STATE_FILE)
            self.state_changed = False
        except OSError as e:
            logger.warning('Unable to save %s: %s', STATE_FILE, e)

    # ---- messages to the Amiga ----

    def send_info(self):
        self.send('REGIONS %d' % len(self.regions))
        for i, (name, bands) in enumerate(self.regions):
            self.send('R %d %s' % (i, name))
        self.send('REG %d' % self.region)
        self.send('BANDS %d' % len(self.bands))
        for i, b in enumerate(self.bands):
            self.send('B %d %d %d %d %s %s %s' % (i, b.lo, b.hi, b.step, b.mode, b.short, b.name))
        self.send(' '.join(['GAINS', str(len(self.cfg['gains']))] + [str(g) for g in self.cfg['gains']]))
        self.send_state()
        for band in self.activity:
            self.send_activity(band)
        self.run_status = self.pipe_status = self.level = self.msg = None

    def send_state(self):
        self.send('ST %d %d %d %d %d %d' % (self.band, self.freq, self.sql, self.gain, self.vol, self.scan_dir))

    def send_activity(self, band):
        self.send('ACTCLR %d' % band)
        chans = [str(c) for c in self.activity[band][1]]
        for i in range(0, len(chans), 20):
            self.send(' '.join(['ACT', str(band)] + chans[i:i + 20]))

    def notify(self, text):
        self.notice = text
        self.notice_until = time.time() + NOTICE_TIME

    def update_status(self, now):
        # Sends RUN, PIPE, LVL and MSG lines when they change.
        if self.error:
            run = 2
        elif self.sweep:
            run = 3
        elif self.fm and now - self.fm.started > 1.0:
            run = 1     # running (with the squelch closed it is silent)
        else:
            run = 0
        if run != self.run_status:
            self.run_status = run
            self.send('RUN %d' % run)

        pipe = 1 if self.pipe is not None else 0
        if pipe != self.pipe_status:
            self.pipe_status = pipe
            self.send('PIPE %d' % pipe)

        if now - self.level_at >= 0.1:
            self.level_at = now
            level = self.level_sum // self.level_n if self.level_n else 0
            if now - self.last_audio > 0.25:
                level = 0
            if level >= 2:
                self.last_open = now
            self.level_sum = self.level_n = 0
            if level != self.level:
                self.level = level
                self.send('LVL %d' % level)

        if self.error:
            msg = self.error
        elif now < self.notice_until:
            msg = self.notice
        elif self.pipe is None:
            msg = 'NO AUDIO OUTPUT - IS THE PIAUDIO SERVICE RUNNING?'
        else:
            msg = ''
        if msg != self.msg:
            self.msg = msg
            self.send('MSG ' + msg)

    # ---- rtl_fm ----

    def start_fm(self):
        self.stop_fm()
        self.retune_at = None
        b = self.cur
        argv = [self.cfg['rtl_fm'], '-d', str(self.cfg['device_index']),
            '-M', b.mode, '-s', str(b.rate), '-f', str(self.freq),
            '-p', str(self.cfg['ppm'])]
        if b.rate != OUT_RATE:
            argv += ['-r', str(OUT_RATE)]
        if self.gain >= 0:
            argv += ['-g', '%.1f' % (self.gain / 10.0)]
        if self.sql > 0:
            argv += ['-l', str(self.sql)]
        argv += b.args + ['-']
        try:
            self.fm = Proc(argv)
        except OSError as e:
            self.fail('CANNOT RUN RTL_FM: ' + e.strerror.upper())
            return
        self.error = None
        self.tuned_at = time.time()

    def stop_fm(self):
        if self.fm:
            self.fm.stop()
            self.fm = None
        self.odd_byte = b''

    def fail(self, text):
        logger.error('Tuner error: %s', text)
        self.error = text
        self.scan_dir = 0
        self.send_state()
        self.retune_at = time.time() + RETRY_DELAY

    def retune(self, now=False):
        # Restarts rtl_fm soon, so that a burst of changes (a slider being
        # dragged) only restarts it once.
        if self.active and not self.sweep:
            self.retune_at = time.time() + (0 if now else RETUNE_DELAY)

    def handle_fm_output(self):
        data = self.fm.read(self.fm.out, 8192)
        if data is None:
            return
        if not data:
            self.fm.read(self.fm.err, 65536)
            text = self.fm.error_text()
            self.stop_fm()
            self.fail(text)
            return

        # rtl_fm writes signed 16-bit little-endian mono. Keep the high
        # bytes and write each one twice, for the left and right channels.
        data = self.odd_byte + data
        if len(data) & 1:
            self.odd_byte, data = data[-1:], data[:-1]
        else:
            self.odd_byte = b''
        samples = data[1::2]
        if not samples:
            return
        self.last_audio = time.time()
        self.level_sum += sum(samples.translate(ABS_TABLE))
        self.level_n += len(samples)
        if self.vol != 8:
            samples = samples.translate(self.vol_table)
        out = bytearray(2 * len(samples))
        out[0::2] = samples
        out[1::2] = samples
        if self.pipe is not None:
            # rtl_fm writes in bursts larger than the pipe, so keep a little
            # in hand, but drop audio rather than fall far behind.
            self.audio = (self.audio + out)[-MAX_AUDIO_BUFFER:]
            self.write_audio()

    # ---- the piaudio pipe ----

    def open_pipe(self):
        try:
            fd = os.open(self.cfg['audio_pipe'], os.O_WRONLY | os.O_NONBLOCK)
        except OSError as e:
            # ENXIO: the pipe has no reader, ENOENT: piaudio hasn't made it
            if e.errno not in (errno.ENXIO, errno.ENOENT):
                logger.warning('Unable to open %s: %s', self.cfg['audio_pipe'], e)
            return
        if not stat.S_ISFIFO(os.fstat(fd).st_mode):
            logger.error('%s is not a named pipe', self.cfg['audio_pipe'])
            os.close(fd)
            return
        self.pipe = fd

    def close_pipe(self):
        if self.pipe is not None:
            os.close(self.pipe)
            self.pipe = None
        self.audio = b''

    def write_audio(self):
        try:
            n = os.write(self.pipe, self.audio)
            self.audio = self.audio[n:]
        except BlockingIOError:
            pass
        except OSError:
            self.close_pipe()   # the piaudio service went away

    # ---- sweeps, seek and scan ----

    def start_sweep(self, after):
        self.stop_fm()
        self.stop_sweep()
        b = self.cur
        # Bins half a channel wide, so that every channel gets one
        lo = b.lo - b.step
        hi = max(b.hi + b.step, lo + 4 * b.step)
        argv = [self.cfg['rtl_power'], '-d', str(self.cfg['device_index']),
            '-f', '%d:%d:%d' % (lo, hi, max(1, b.step // 2)),
            '-i', '1', '-1', '-c', '20%', '-p', str(self.cfg['ppm'])]
        if self.gain >= 0:
            argv += ['-g', '%.1f' % (self.gain / 10.0)]
        try:
            self.sweep = Proc(argv)
        except OSError as e:
            self.fail('CANNOT RUN RTL_POWER: ' + e.strerror.upper())
            self.retune_at = time.time()    # carry on listening
            return
        self.sweep_out = b''
        self.after_sweep = after
        self.error = None

    def stop_sweep(self):
        if self.sweep:
            self.sweep.stop()
            self.sweep = None
        self.after_sweep = None

    def handle_sweep_output(self):
        data = self.sweep.read(self.sweep.out, 65536)
        if data:
            self.sweep_out += data
        elif data is not None:
            self.finish_sweep()

    def finish_sweep(self):
        proc, after = self.sweep, self.after_sweep
        proc.read(proc.err, 65536)
        self.stop_sweep()
        chans = self.find_active(self.sweep_out.decode('latin-1'))
        if chans is None:
            self.scan_dir = 0
            self.send_state()
            self.notify('SWEEP FAILED: ' + proc.error_text())
        else:
            logger.info('Sweep of %s found %d active channels', self.cur.name, len(chans))
            self.activity[self.band] = (time.time(), chans)
            self.send_activity(self.band)
            if not chans:
                self.scan_dir = 0
                self.send_state()
                self.notify('NO ACTIVE CHANNELS FOUND')
            elif after == 'seek':
                self.go_to_active(self.seek_dir, wrap=True)
            elif after == 'scan':
                self.go_to_active(self.scan_dir, wrap=True)
            elif after == 'scan_again':
                # Start from the end of the band, without skipping its edge
                self.freq = self.cur.freq_of(chans[0] if self.scan_dir > 0 else chans[-1])
                self.send_state()
            else:
                self.notify('FOUND %d ACTIVE CHANNELS' % len(chans))
        self.start_fm()

    def find_active(self, csv):
        # rtl_power writes lines of: date, time, Hz low, Hz high, Hz step,
        # samples, dB, dB, ... Returns the channels that are local peaks at
        # least sweep_threshold_db above the median, or None on failure.
        b = self.cur
        best = {}
        for line in csv.split('\n'):
            parts = line.split(',')
            try:
                hz_lo = float(parts[2])
                hz_step = float(parts[4])
                values = [float(v) for v in parts[6:]]
            except (IndexError, ValueError):
                continue
            for i, v in enumerate(values):
                ch = round((hz_lo + (i + 0.5) * hz_step - b.lo) / b.step)
                if 0 <= ch < b.channels() and v == v and v > best.get(ch, -999.0):
                    best[ch] = v
        if not best:
            return None
        floor = sorted(best.values())[len(best) // 2]
        threshold = floor + self.cfg['sweep_threshold_db']
        active = [ch for ch, v in sorted(best.items())
            if v >= threshold and v > best.get(ch - 1, -999.0) and v >= best.get(ch + 1, -999.0)]
        return active[:MAX_ACTIVE]

    def fresh_activity(self):
        a = self.activity.get(self.band)
        return a is not None and a[1] and time.time() - a[0] < ACTIVITY_MAX_AGE

    def go_to_active(self, direction, wrap):
        # Tunes to the next active channel up or down. Returns False if
        # there is none before the end of the band and wrap is False.
        chans = self.activity[self.band][1]
        cur = self.cur.channel_of(self.freq)
        if direction > 0:
            ahead = [c for c in chans if c > cur]
        else:
            ahead = [c for c in reversed(chans) if c < cur]
        if not ahead:
            if not wrap:
                return False
            ahead = chans if direction > 0 else chans[::-1]
        self.freq = self.cur.freq_of(ahead[0])
        self.send_state()
        self.retune(now=True)
        return True

    def scan_next(self):
        # At the end of the band, sweep again so that new signals are found
        if not self.go_to_active(self.scan_dir, wrap=False):
            self.start_sweep('scan_again')

    # ---- commands from the Amiga ----

    def handle_command(self, line):
        parts = line.split()
        try:
            cmd = parts[0]
            arg = int(parts[1]) if len(parts) > 1 else 0
        except (IndexError, ValueError):
            return

        if cmd == 'HELLO':
            self.send_info()
            if not self.active:
                self.active = True
                self.open_pipe()
                self.start_fm()
            return
        if not self.active:
            return
        band_before = self.band

        if cmd in ('REGION', 'BAND', 'FREQ', 'SEEK', 'SCAN', 'SWEEP'):
            # The user wants something else: stop scanning and sweeping
            self.scan_dir = 0
            if self.sweep:
                self.stop_sweep()
                self.retune(now=True)

        if cmd == 'REGION':
            if 0 <= arg < len(self.regions) and arg != self.region:
                self.set_region(arg)
                self.send_info()
                self.notify('REGION: ' + self.regions[arg][0].upper())
                self.retune(now=True)
            return
        if cmd == 'BAND':
            if 0 <= arg < len(self.bands) and arg != self.band:
                self.band = arg
                self.state_changed = True
                self.retune(now=True)
        elif cmd == 'FREQ':
            if not self.cur.contains(arg):
                for i, b in enumerate(self.bands):
                    if b.contains(arg):
                        self.band = i
                        break
            self.freq = arg
            self.retune()
        elif cmd == 'SQL':
            self.sql = arg
            self.retune()
        elif cmd == 'GAIN':
            self.gain = arg if arg >= 0 else -1
            self.state_changed = True
            self.retune()
        elif cmd == 'VOL':
            self.vol = max(0, min(16, arg))
            self.vol_table = volume_table(self.vol)
            self.state_changed = True
        elif cmd == 'SEEK':
            self.seek_dir = 1 if arg >= 0 else -1
            if self.fresh_activity():
                self.go_to_active(self.seek_dir, wrap=True)
            else:
                self.start_sweep('seek')
        elif cmd == 'SCAN' and arg != 0:
            self.scan_dir = 1 if arg > 0 else -1
            if self.fresh_activity():
                self.scan_next()
            else:
                self.start_sweep('scan')
        elif cmd == 'SWEEP':
            self.start_sweep(None)
        self.send_state()
        if self.band != band_before and self.band in self.activity:
            self.send_activity(self.band)   # the Amiga forgets them on a band change

    # ---- called from the main loop ----

    def stop(self):
        self.active = False
        self.scan_dir = 0
        self.stop_fm()
        self.stop_sweep()
        self.close_pipe()
        self.retune_at = None
        self.save_state()

    def read_fds(self):
        fds = []
        for p in (self.fm, self.sweep):
            if p:
                fds += [p.out, p.err]
        return fds

    def write_fds(self):
        return [self.pipe] if self.pipe is not None and self.audio else []

    def handle_fd(self, fd):
        if self.fm and fd == self.fm.out:
            self.handle_fm_output()
        elif self.fm and fd == self.fm.err:
            self.fm.read(fd, 4096)
        elif self.sweep and fd == self.sweep.out:
            self.handle_sweep_output()
        elif self.sweep and fd == self.sweep.err:
            self.sweep.read(fd, 4096)
        elif fd == self.pipe:
            self.write_audio()

    def tick(self):
        if not self.active:
            return
        now = time.time()

        if self.retune_at is not None and now >= self.retune_at and not self.sweep:
            self.start_fm()

        if self.sweep and now - self.sweep.started > SWEEP_TIMEOUT:
            self.stop_sweep()
            self.fail('THE SWEEP TOOK TOO LONG')

        # rtl_fm can hang without exiting if the dongle glitches. With the
        # squelch on it is silent anyway, so this is only checked without.
        if (self.fm and self.sql == 0 and self.retune_at is None and
                now - max(self.last_audio, self.fm.started) > NO_AUDIO_TIMEOUT):
            self.stop_fm()
            self.fail('NO DATA FROM THE DONGLE')

        if self.pipe is None and self.fm and now - self.pipe_tried_at >= 1.0:
            self.pipe_tried_at = now
            self.open_pipe()

        if self.scan_dir and self.fm and self.retune_at is None:
            if self.sql > 0:
                quiet_for = now - max(self.last_open, self.tuned_at + 1.0)
                if quiet_for > self.cfg['scan_hang_seconds']:
                    self.scan_next()
            elif now - self.tuned_at > self.cfg['scan_dwell_seconds']:
                self.scan_next()

        self.update_status(now)


class Service(object):
    def __init__(self):
        self.a314d = A314d(SERVICE_NAME)
        self.stream_id = None
        self.inbuf = b''
        self.outbuf = []
        self.heartbeat_at = 0
        self.radio = Radio(load_config(), self.send)
        self.state_saved_at = 0

    def send(self, line):
        if self.stream_id is not None:
            data = line.encode('latin-1', 'replace')[:MAX_CHUNK - 1]
            self.outbuf.append(data + b'\n')

    def flush(self):
        # Send the queued lines in chunks of whole lines.
        chunk = b''
        for line in self.outbuf:
            if len(chunk) + len(line) > MAX_CHUNK:
                self.a314d.send_data(self.stream_id, chunk)
                chunk = b''
            chunk += line
        if chunk:
            self.a314d.send_data(self.stream_id, chunk)
        self.outbuf = []

    def disconnected(self):
        logger.info('Amiga disconnected')
        self.stream_id = None
        self.outbuf = []
        self.radio.stop()

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
                self.disconnected()
            logger.info('Amiga connected')
            self.a314d.send_connect_response(stream_id, 0)
            self.stream_id = stream_id
            self.inbuf = b''
            self.heartbeat_at = 0
        elif stream_id == self.stream_id:
            if ptype == A314d.MSG_DATA:
                self.inbuf += payload
                while b'\n' in self.inbuf:
                    line, self.inbuf = self.inbuf.split(b'\n', 1)
                    self.radio.handle_command(line.decode('latin-1'))
                self.inbuf = self.inbuf[-256:]
            elif ptype == A314d.MSG_EOS:
                self.a314d.send_reset(stream_id)
                self.disconnected()
            elif ptype == A314d.MSG_RESET:
                self.disconnected()

    def run(self):
        logger.info('piradio service is running')
        while True:
            fds = [self.a314d.drv] + self.radio.read_fds()
            if '-ondemand' not in sys.argv:
                fds.append(sys.stdin)
            timeout = 0.1 if self.stream_id is not None else None
            rfds, wfds, _ = select.select(fds, self.radio.write_fds(), [], timeout)

            for fd in rfds:
                if fd == self.a314d.drv:
                    self.handle_msg()
                elif fd == sys.stdin:
                    line = sys.stdin.readline()
                    if not line or line.startswith('quit'):
                        return
                else:
                    self.radio.handle_fd(fd)
            for fd in wfds:
                self.radio.handle_fd(fd)

            if self.stream_id is not None:
                now = time.time()
                self.radio.tick()
                if now >= self.heartbeat_at:
                    self.heartbeat_at = now + HEARTBEAT_INTERVAL
                    self.send('HB')
                if now - self.state_saved_at > 10.0:
                    self.state_saved_at = now
                    self.radio.save_state()
                self.flush()


if __name__ == '__main__':
    service = Service()
    try:
        service.run()
    finally:
        service.radio.stop()
        try:
            if service.stream_id is not None:
                service.a314d.send_reset(service.stream_id)
            service.a314d.close()
        except OSError:
            pass
