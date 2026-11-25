#!/usr/bin/python3
# -*- coding: utf-8 -*-

# Picture service: decodes pictures for a314picture.datatype, so that
# Amiga programs that use datatypes (MultiView and many others) can show
# formats that the Amiga has no datatype for: WebP, AVIF, HEIC, JPEG XL
# and TIFF. Pictures larger than the Amiga asks for are made smaller.
#
# The decoding is done by the usual libraries, through ctypes: libwebp,
# libheif (with its AVIF and HEIC plugins), libjxl and libtiff. Each is
# optional: a format whose library is missing is reported as unsupported.
#
# The file and the pixels go through a buffer in A314 memory that the
# Amiga allocates; each step waits for the other side. The Amiga and this
# service exchange text lines, one per packet.
#
# Amiga -> Pi:
#   BUF <address> <size>    the buffer (A314 address), sent first
#   FILE <size>             a file of this size follows
#   DATA <n>                n bytes of the file are at the start of the buffer
#   DECODE <maxw> <maxh> <mode> [<colours>]
#                           decode, no larger than maxw x maxh; mode is rgb
#                           (3 bytes a pixel) or pal (a palette of up to
#                           <colours> colours, then 1 byte a pixel)
#   NEXT                    the next block, after INFO, PAL or ROWS
#
# Pi -> Amiga:
#   TAKEN                   the DATA was read
#   INFO <w> <h> <file w> <file h> <format>   decoded; NEXT for the data
#   ERR <code> <text>       code: 2000 unknown format, 2008 bad data,
#                           2009 not available (no library), 103 no memory
#   PAL <n>                 n colours (r, g, b) are in the buffer
#   ROWS <y> <n>            rows y to y + n - 1 are in the buffer
#   DONE                    no more rows

import ctypes
import ctypes.util
import logging
import operator
import os
import struct
import tempfile

from a314d import A314d

logging.basicConfig(format = '%(levelname)s, %(asctime)s, %(name)s, line %(lineno)d: %(message)s')
logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

SERVICE_NAME = 'picture'

MAX_FILE = 64 * 1024 * 1024
MAX_LINE = 251

DTERROR_UNKNOWN_DATATYPE = 2000
DTERROR_INVALID_DATA = 2008
DTERROR_NOT_AVAILABLE = 2009
ERROR_NO_FREE_STORE = 103


class DecodeError(Exception):
    def __init__(self, code, text):
        super().__init__(text)
        self.code = code


def load(name):
    path = ctypes.util.find_library(name)
    if not path:
        return None
    try:
        return ctypes.CDLL(path)
    except OSError:
        return None


# ---------------------------------------------------------------------------
# Decoders. Each returns (width, height, rgb bytes), or for HEIF also takes
# the wanted size, as libheif can scale well itself.

def detect(data):
    if data[:4] == b'RIFF' and data[8:12] == b'WEBP':
        return 'WebP'
    if data[4:8] == b'ftyp':
        brand = data[8:12]
        if brand in (b'avif', b'avis'):
            return 'AVIF'
        if brand in (b'heic', b'heix', b'heim', b'heis', b'hevc', b'hevx', b'mif1', b'msf1'):
            return 'HEIF'
    if data[:2] == b'\xff\x0a' or data[:12] == b'\0\0\0\x0cJXL \r\n\x87\n':
        return 'JPEG XL'
    if data[:4] in (b'II*\0', b'MM\0*'):
        return 'TIFF'
    return None


def decode_webp(data, maxw, maxh):
    lib = load('webp')
    if not lib:
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libwebp is not installed')
    lib.WebPDecodeRGB.restype = ctypes.c_void_p
    lib.WebPDecodeRGB.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
    lib.WebPFree.argtypes = [ctypes.c_void_p]
    w, h = ctypes.c_int(0), ctypes.c_int(0)
    p = lib.WebPDecodeRGB(data, len(data), ctypes.byref(w), ctypes.byref(h))
    if not p:
        raise DecodeError(DTERROR_INVALID_DATA, 'not a WebP picture that can be shown')
    try:
        return w.value, h.value, ctypes.string_at(p, w.value * h.value * 3)
    finally:
        lib.WebPFree(p)


class HeifError(ctypes.Structure):
    _fields_ = [('code', ctypes.c_int), ('subcode', ctypes.c_int), ('message', ctypes.c_char_p)]


def decode_heif(data, maxw, maxh):
    lib = load('heif')
    if not lib:
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libheif is not installed')
    vp = ctypes.c_void_p
    lib.heif_context_alloc.restype = vp
    lib.heif_context_free.argtypes = [vp]
    lib.heif_context_read_from_memory_without_copy.restype = HeifError
    lib.heif_context_read_from_memory_without_copy.argtypes = [vp, ctypes.c_char_p, ctypes.c_size_t, vp]
    lib.heif_context_get_primary_image_handle.restype = HeifError
    lib.heif_context_get_primary_image_handle.argtypes = [vp, ctypes.POINTER(vp)]
    lib.heif_decode_image.restype = HeifError
    lib.heif_decode_image.argtypes = [vp, ctypes.POINTER(vp), ctypes.c_int, ctypes.c_int, vp]
    lib.heif_image_scale_image.restype = HeifError
    lib.heif_image_scale_image.argtypes = [vp, ctypes.POINTER(vp), ctypes.c_int, ctypes.c_int, vp]
    lib.heif_image_handle_get_width.argtypes = [vp]
    lib.heif_image_handle_get_height.argtypes = [vp]
    lib.heif_image_get_plane_readonly.restype = ctypes.POINTER(ctypes.c_ubyte)
    lib.heif_image_get_plane_readonly.argtypes = [vp, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.heif_image_release.argtypes = [vp]
    lib.heif_image_handle_release.argtypes = [vp]
    HEIF_COLORSPACE_RGB, HEIF_CHROMA_INTERLEAVED_RGB, HEIF_CHANNEL_INTERLEAVED = 1, 10, 10

    ctx = lib.heif_context_alloc()
    handle, img, scaled = vp(), vp(), vp()
    try:
        for e in (lib.heif_context_read_from_memory_without_copy(ctx, data, len(data), None),
                  lib.heif_context_get_primary_image_handle(ctx, ctypes.byref(handle))):
            if e.code:
                raise DecodeError(DTERROR_INVALID_DATA, (e.message or b'bad HEIF data').decode('latin-1'))
        e = lib.heif_decode_image(handle, ctypes.byref(img), HEIF_COLORSPACE_RGB,
            HEIF_CHROMA_INTERLEAVED_RGB, None)
        if e.code:
            raise DecodeError(DTERROR_INVALID_DATA, (e.message or b'cannot decode').decode('latin-1'))
        w = lib.heif_image_handle_get_width(handle)
        h = lib.heif_image_handle_get_height(handle)
        out = img
        nw, nh = fit(w, h, maxw, maxh)
        if (nw, nh) != (w, h):
            if not lib.heif_image_scale_image(img, ctypes.byref(scaled), nw, nh, None).code:
                out, w, h = scaled, nw, nh
        stride = ctypes.c_int(0)
        plane = lib.heif_image_get_plane_readonly(out, HEIF_CHANNEL_INTERLEAVED, ctypes.byref(stride))
        if not plane:
            raise DecodeError(DTERROR_INVALID_DATA, 'no picture data')
        raw = ctypes.string_at(plane, stride.value * h)
        rows = [raw[y * stride.value:y * stride.value + w * 3] for y in range(h)]
        return w, h, b''.join(rows)
    finally:
        if scaled:
            lib.heif_image_release(scaled)
        if img:
            lib.heif_image_release(img)
        if handle:
            lib.heif_image_handle_release(handle)
        lib.heif_context_free(ctx)


def decode_jxl(data, maxw, maxh):
    lib = load('jxl')
    if not lib:
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libjxl is not installed')
    vp = ctypes.c_void_p
    JXL_DEC_SUCCESS, JXL_DEC_ERROR, JXL_DEC_NEED_MORE_INPUT = 0, 1, 2
    JXL_DEC_NEED_IMAGE_OUT_BUFFER, JXL_DEC_BASIC_INFO, JXL_DEC_FULL_IMAGE = 5, 0x40, 0x1000
    JXL_TYPE_UINT8 = 2

    class PixelFormat(ctypes.Structure):
        _fields_ = [('num_channels', ctypes.c_uint32), ('data_type', ctypes.c_int),
                    ('endianness', ctypes.c_int), ('align', ctypes.c_size_t)]

    lib.JxlDecoderCreate.restype = vp
    lib.JxlDecoderCreate.argtypes = [vp]
    lib.JxlDecoderDestroy.argtypes = [vp]
    lib.JxlDecoderSubscribeEvents.argtypes = [vp, ctypes.c_int]
    lib.JxlDecoderSetInput.argtypes = [vp, ctypes.c_char_p, ctypes.c_size_t]
    lib.JxlDecoderCloseInput.argtypes = [vp]
    lib.JxlDecoderProcessInput.argtypes = [vp]
    lib.JxlDecoderGetBasicInfo.argtypes = [vp, ctypes.c_char_p]
    lib.JxlDecoderImageOutBufferSize.argtypes = [vp, ctypes.POINTER(PixelFormat), ctypes.POINTER(ctypes.c_size_t)]
    lib.JxlDecoderSetImageOutBuffer.argtypes = [vp, ctypes.POINTER(PixelFormat), vp, ctypes.c_size_t]

    fmt = PixelFormat(3, JXL_TYPE_UINT8, 0, 0)
    dec = lib.JxlDecoderCreate(None)
    out = None
    w = h = 0
    try:
        lib.JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE)
        lib.JxlDecoderSetInput(dec, data, len(data))
        lib.JxlDecoderCloseInput(dec)
        while True:
            st = lib.JxlDecoderProcessInput(dec)
            if st == JXL_DEC_BASIC_INFO:
                info = ctypes.create_string_buffer(512)     # JxlBasicInfo is smaller
                lib.JxlDecoderGetBasicInfo(dec, info)
                w, h = struct.unpack_from('=II', info.raw, 4)
            elif st == JXL_DEC_NEED_IMAGE_OUT_BUFFER:
                size = ctypes.c_size_t(0)
                lib.JxlDecoderImageOutBufferSize(dec, ctypes.byref(fmt), ctypes.byref(size))
                out = ctypes.create_string_buffer(size.value)
                lib.JxlDecoderSetImageOutBuffer(dec, ctypes.byref(fmt), out, size.value)
            elif st == JXL_DEC_FULL_IMAGE:
                break       # the first frame only
            elif st == JXL_DEC_SUCCESS:
                break
            else:
                raise DecodeError(DTERROR_INVALID_DATA, 'bad JPEG XL data')
    finally:
        lib.JxlDecoderDestroy(dec)
    if out is None or not w or not h:
        raise DecodeError(DTERROR_INVALID_DATA, 'bad JPEG XL data')
    return w, h, out.raw[:w * h * 3]


def decode_tiff(data, maxw, maxh):
    lib = load('tiff')
    if not lib:
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libtiff is not installed')
    vp = ctypes.c_void_p
    lib.TIFFSetErrorHandler.argtypes = [vp]
    lib.TIFFSetWarningHandler.argtypes = [vp]
    lib.TIFFSetErrorHandler(None)
    lib.TIFFSetWarningHandler(None)
    lib.TIFFOpen.restype = vp
    lib.TIFFOpen.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lib.TIFFClose.argtypes = [vp]
    lib.TIFFReadRGBAImageOriented.argtypes = [vp, ctypes.c_uint32, ctypes.c_uint32, vp, ctypes.c_int, ctypes.c_int]
    TIFFTAG_IMAGEWIDTH, TIFFTAG_IMAGELENGTH, ORIENTATION_TOPLEFT = 256, 257, 1

    # libtiff reads files, so the data goes into a temporary one
    with tempfile.NamedTemporaryFile(suffix='.tif') as f:
        f.write(data)
        f.flush()
        tif = lib.TIFFOpen(f.name.encode(), b'r')
        if not tif:
            raise DecodeError(DTERROR_INVALID_DATA, 'bad TIFF data')
        try:
            w, h = ctypes.c_uint32(0), ctypes.c_uint32(0)
            lib.TIFFGetField(ctypes.c_void_p(tif), TIFFTAG_IMAGEWIDTH, ctypes.byref(w))
            lib.TIFFGetField(ctypes.c_void_p(tif), TIFFTAG_IMAGELENGTH, ctypes.byref(h))
            w, h = w.value, h.value
            if not w or not h or w * h > 100 * 1000 * 1000:
                raise DecodeError(DTERROR_INVALID_DATA, 'bad TIFF size')
            raster = ctypes.create_string_buffer(w * h * 4)
            if not lib.TIFFReadRGBAImageOriented(tif, w, h, raster, ORIENTATION_TOPLEFT, 0):
                raise DecodeError(DTERROR_INVALID_DATA, 'cannot read this TIFF')
        finally:
            lib.TIFFClose(tif)
    # Each pixel is a little-endian ABGR word: bytes R, G, B, A
    rgba = raster.raw
    rgb = bytearray(w * h * 3)
    rgb[0::3] = rgba[0::4]
    rgb[1::3] = rgba[1::4]
    rgb[2::3] = rgba[2::4]
    return w, h, bytes(rgb)


DECODERS = {'WebP': decode_webp, 'AVIF': decode_heif, 'HEIF': decode_heif,
            'JPEG XL': decode_jxl, 'TIFF': decode_tiff}


# ---------------------------------------------------------------------------
# Scaling and colour reduction (for Amigas whose picture.datatype only
# takes a palette)

def fit(w, h, maxw, maxh):
    # The largest size within maxw x maxh with the picture's proportions
    if w <= maxw and h <= maxh:
        return w, h
    scale = min(maxw / w, maxh / h)
    return max(1, int(w * scale)), max(1, int(h * scale))


def scale(w, h, rgb, nw, nh):
    # Nearest-pixel scaling (libheif scales HEIF and AVIF itself, better)
    cols = []
    for x in range(nw):
        sx = x * w // nw * 3
        cols += (sx, sx + 1, sx + 2)
    pick = operator.itemgetter(*cols)
    stride = w * 3
    rows = []
    for y in range(nh):
        sy = y * h // nh
        rows.append(bytes(pick(rgb[sy * stride:(sy + 1) * stride])))
    return b''.join(rows)


def quantize(w, h, rgb, colours):
    # Median cut on a 5-bit-per-channel histogram. Returns (palette as
    # bytes r, g, b..., one byte per pixel).
    shift = bytes(i >> 3 for i in range(256))
    r = rgb[0::3].translate(shift)
    g = rgb[1::3].translate(shift)
    b = rgb[2::3].translate(shift)
    keys = [(x << 10) | (y << 5) | z for x, y, z in zip(r, g, b)]
    hist = {}
    for k in keys:
        hist[k] = hist.get(k, 0) + 1

    boxes = [list(hist.items())]
    while len(boxes) < colours:
        # Split the box with the most pixels that has more than one colour
        best = None
        for i, box in enumerate(boxes):
            if len(box) > 1:
                n = sum(c for _, c in box)
                if best is None or n > best[0]:
                    best = (n, i)
        if best is None:
            break
        box = boxes.pop(best[1])
        ranges = []
        for sh in (10, 5, 0):
            vals = [(k >> sh) & 31 for k, _ in box]
            ranges.append((max(vals) - min(vals), sh))
        sh = max(ranges)[1]
        box.sort(key=lambda kc: (kc[0] >> sh) & 31)
        half, acc = sum(c for _, c in box) / 2, 0
        for cut, (_, c) in enumerate(box):
            acc += c
            if acc >= half:
                break
        cut = max(1, min(len(box) - 1, cut + 1))
        boxes += [box[:cut], box[cut:]]

    palette = bytearray()
    index = {}
    for i, box in enumerate(boxes):
        n = sum(c for _, c in box)
        for sh in (10, 5, 0):
            v = sum(((k >> sh) & 31) * c for k, c in box) / n
            palette.append(min(255, int(v * 8.226)))     # 31 -> 255
        for k, _ in box:
            index[k] = i
    return bytes(palette), bytes(index[k] for k in keys)


# ---------------------------------------------------------------------------

class Job(object):
    # One picture for one Amiga connection

    def __init__(self, service, stream_id):
        self.service = service
        self.stream_id = stream_id
        self.buf = None             # (address, size)
        self.data = bytearray()
        self.size = 0
        self.w = self.h = 0
        self.pixels = None          # bytes per row: 3 (rgb) or 1 (pal)
        self.palette = None
        self.next_row = 0
        self.bpp = 3

    def send(self, line):
        self.service.send(self.stream_id, line.encode('latin-1')[:MAX_LINE])

    def command(self, line):
        words = line.split()
        if not words:
            return
        cmd, args = words[0], words[1:]
        try:
            if cmd == 'BUF' and len(args) == 2:
                self.buf = (int(args[0]), int(args[1]))
            elif cmd == 'FILE' and len(args) == 1:
                self.size = min(int(args[0]), MAX_FILE)
                self.data = bytearray()
            elif cmd == 'DATA' and len(args) == 1 and self.buf:
                n = min(int(args[0]), self.buf[1], self.size - len(self.data))
                self.data += self.service.read_mem(self.buf[0], n)
                self.send('TAKEN')
            elif cmd == 'DECODE' and len(args) >= 3:
                self.decode(int(args[0]), int(args[1]), args[2],
                    int(args[3]) if len(args) > 3 else 256)
            elif cmd == 'NEXT':
                self.next_block()
            else:
                logger.warning('Unknown command %r', line[:40])
        except ValueError:
            logger.warning('Bad command %r', line[:40])

    def decode(self, maxw, maxh, mode, colours):
        data = bytes(self.data)
        self.data = bytearray()
        fmt = detect(data)
        if fmt is None:
            self.send('ERR %d not a picture this service knows' % DTERROR_UNKNOWN_DATATYPE)
            return
        maxw, maxh = max(16, maxw), max(16, maxh)
        try:
            w, h, rgb = DECODERS[fmt](data, maxw, maxh)
        except DecodeError as e:
            logger.info('%s: %s', fmt, e)
            self.send('ERR %d %s' % (e.code, e))
            return
        except MemoryError:
            self.send('ERR %d picture too large' % ERROR_NO_FREE_STORE)
            return
        src = (w, h)
        nw, nh = fit(w, h, maxw, maxh)
        if (nw, nh) != (w, h):
            rgb = scale(w, h, rgb, nw, nh)
            w, h = nw, nh
        if mode == 'pal':
            self.palette, self.pixels = quantize(w, h, rgb, max(2, min(256, colours)))
            self.bpp = 1
        else:
            self.palette, self.pixels, self.bpp = None, rgb, 3
        self.w, self.h, self.next_row = w, h, 0
        logger.info('%s %dx%d, sent as %dx%d %s', fmt, src[0], src[1], w, h, mode)
        self.send('INFO %d %d %d %d %s' % (w, h, src[0], src[1], fmt))

    def next_block(self):
        if self.pixels is None or not self.buf:
            self.send('DONE')
            return
        address, size = self.buf
        if self.palette is not None:
            self.service.write_mem(address, self.palette)
            self.send('PAL %d' % (len(self.palette) // 3))
            self.palette = None
            return
        row = self.w * self.bpp
        if self.next_row >= self.h:
            self.pixels = None
            self.send('DONE')
            return
        n = max(1, min(size // row, self.h - self.next_row))
        y = self.next_row
        self.service.write_mem(address, self.pixels[y * row:(y + n) * row])
        self.next_row += n
        self.send('ROWS %d %d' % (y, n))


class PictureService(object):
    def __init__(self):
        self.a314d = A314d(SERVICE_NAME)
        self.jobs = {}
        self.pending = []

    def send(self, stream_id, data):
        self.a314d.send_data(stream_id, data)

    def wait_for(self, ptype):
        # Waits for the answer to a memory request, keeping the messages
        # that arrive before it
        while True:
            msg = self.a314d.wait_for_msg()
            if msg[1] == ptype:
                return msg[2]
            self.pending.append(msg)

    def read_mem(self, address, length):
        self.a314d.send_read_mem_req(address, length)
        return self.wait_for(A314d.MSG_READ_MEM_RES)

    def write_mem(self, address, data):
        self.a314d.send_write_mem_req(address, data)
        self.wait_for(A314d.MSG_WRITE_MEM_RES)

    def handle_msg(self, stream_id, ptype, payload):
        if ptype == A314d.MSG_CONNECT:
            if payload == SERVICE_NAME.encode():
                self.jobs[stream_id] = Job(self, stream_id)
                self.a314d.send_connect_response(stream_id, 0)
            else:
                self.a314d.send_connect_response(stream_id, 3)
            return
        job = self.jobs.get(stream_id)
        if job is None:
            return
        if ptype == A314d.MSG_DATA:
            job.command(payload.decode('latin-1'))
        elif ptype in (A314d.MSG_EOS, A314d.MSG_RESET):
            if ptype == A314d.MSG_EOS:
                self.a314d.send_eos(stream_id)
            del self.jobs[stream_id]

    def run(self):
        while True:
            while self.pending:
                self.handle_msg(*self.pending.pop(0))
            self.handle_msg(*self.a314d.wait_for_msg())


if __name__ == '__main__':
    service = PictureService()
    logger.info('%s service is running', SERVICE_NAME)
    try:
        service.run()
    except KeyboardInterrupt:
        pass
    service.a314d.close()
