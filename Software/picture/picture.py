#!/usr/bin/python3
# -*- coding: utf-8 -*-

# Picture service: decodes pictures for a314picture.datatype, so that
# Amiga programs that use datatypes (MultiView and many others) can show
# formats that the Amiga has no datatype for: WebP, AVIF, HEIC, JPEG XL
# and TIFF. Pictures larger than the Amiga asks for are made smaller.
#
# The decoding is done by the usual libraries, through ctypes: libwebp,
# libheif (with its AVIF and HEIC plugins), libjxl and libtiff. Each is
# optional: a format whose library is missing is reported as unavailable.
#
# Pictures come from anywhere, so each is decoded in a child process of
# its own, with limits on its memory and time, after a check that the
# picture isn't larger than this Pi can handle. A damaged or hostile file
# can then only fail its own decoding, and several pictures can be decoded
# at once.
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
#                           2009 not available (no library), 103 too large
#   PAL <n>                 n colours (r, g, b) are in the buffer
#   ROWS <y> <n>            rows y to y + n - 1 are in the buffer
#   DONE                    no more rows
#
# A row is never larger than the buffer: if it would be, the answer to
# DECODE is ERR.

import ctypes
import ctypes.util
import errno
import logging
import operator
import os
import resource
import select
import signal
import struct
import tempfile
import time

from a314d import A314d

logging.basicConfig(format = '%(levelname)s, %(asctime)s, %(name)s, line %(lineno)d: %(message)s')
logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

SERVICE_NAME = 'picture'

MAX_FILE = 64 * 1024 * 1024
MAX_LINE = 251
DECODE_SECONDS = 45             # the Amiga waits 60
PHYS_MEM = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES')
# Decoding takes several times the size of the picture in memory; a
# child may use at most half of the Pi's memory
CHILD_MEM = min(PHYS_MEM // 2, 2 << 30)
MAX_PIXELS = min(100 * 1000 * 1000, CHILD_MEM // 24)

DTERROR_UNKNOWN_DATATYPE = 2000
DTERROR_INVALID_DATA = 2008
DTERROR_NOT_AVAILABLE = 2009
ERROR_NO_FREE_STORE = 103


class DecodeError(Exception):
    def __init__(self, code, text):
        super().__init__(text)
        self.code = code


_libs = {}

def load(name):
    if name not in _libs:
        path = ctypes.util.find_library(name)
        try:
            _libs[name] = ctypes.CDLL(path) if path else None
        except OSError:
            _libs[name] = None
    return _libs[name]


def check_size(w, h):
    if w <= 0 or h <= 0:
        raise DecodeError(DTERROR_INVALID_DATA, 'bad picture size')
    if w * h > MAX_PIXELS:
        raise DecodeError(ERROR_NO_FREE_STORE, 'picture too large (%dx%d)' % (w, h))


# ---------------------------------------------------------------------------
# Decoders. Each returns (width, height, rgb bytes). HEIF also takes the
# wanted size, as libheif can scale well itself.

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
    ip = ctypes.POINTER(ctypes.c_int)
    lib.WebPGetInfo.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ip, ip]
    lib.WebPDecodeRGB.restype = ctypes.c_void_p
    lib.WebPDecodeRGB.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ip, ip]
    lib.WebPFree.argtypes = [ctypes.c_void_p]
    w, h = ctypes.c_int(0), ctypes.c_int(0)
    if not lib.WebPGetInfo(data, len(data), ctypes.byref(w), ctypes.byref(h)):
        raise DecodeError(DTERROR_INVALID_DATA, 'bad WebP data')
    check_size(w.value, h.value)
    p = lib.WebPDecodeRGB(data, len(data), ctypes.byref(w), ctypes.byref(h))
    if not p:
        # An animation: its first frame
        return decode_webp_animation(data)
    try:
        return w.value, h.value, ctypes.string_at(p, w.value * h.value * 3)
    finally:
        lib.WebPFree(p)


def decode_webp_animation(data):
    lib = load('webpdemux')
    if not lib:
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libwebpdemux is not installed')
    WEBP_DEMUX_ABI_VERSION, MODE_RGBA = 0x0107, 1

    class WebPData(ctypes.Structure):
        _fields_ = [('bytes', ctypes.c_char_p), ('size', ctypes.c_size_t)]

    class Options(ctypes.Structure):
        _fields_ = [('color_mode', ctypes.c_int), ('use_threads', ctypes.c_int),
                    ('padding', ctypes.c_uint32 * 7)]

    class Info(ctypes.Structure):
        _fields_ = [('canvas_width', ctypes.c_uint32), ('canvas_height', ctypes.c_uint32),
                    ('loop_count', ctypes.c_uint32), ('bgcolor', ctypes.c_uint32),
                    ('frame_count', ctypes.c_uint32), ('pad', ctypes.c_uint32 * 4)]

    vp = ctypes.c_void_p
    lib.WebPAnimDecoderOptionsInitInternal.argtypes = [ctypes.POINTER(Options), ctypes.c_int]
    lib.WebPAnimDecoderNewInternal.restype = vp
    lib.WebPAnimDecoderNewInternal.argtypes = [ctypes.POINTER(WebPData), ctypes.POINTER(Options), ctypes.c_int]
    lib.WebPAnimDecoderGetInfo.argtypes = [vp, ctypes.POINTER(Info)]
    lib.WebPAnimDecoderGetNext.argtypes = [vp, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_int)]
    lib.WebPAnimDecoderDelete.argtypes = [vp]

    opts = Options()
    if not lib.WebPAnimDecoderOptionsInitInternal(ctypes.byref(opts), WEBP_DEMUX_ABI_VERSION):
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libwebpdemux version mismatch')
    opts.color_mode = MODE_RGBA
    wd = WebPData(data, len(data))
    dec = lib.WebPAnimDecoderNewInternal(ctypes.byref(wd), ctypes.byref(opts), WEBP_DEMUX_ABI_VERSION)
    if not dec:
        raise DecodeError(DTERROR_INVALID_DATA, 'not a WebP picture that can be shown')
    try:
        info = Info()
        if not lib.WebPAnimDecoderGetInfo(dec, ctypes.byref(info)):
            raise DecodeError(DTERROR_INVALID_DATA, 'bad WebP animation')
        w, h = info.canvas_width, info.canvas_height
        check_size(w, h)
        buf, ts = ctypes.c_void_p(), ctypes.c_int(0)
        if not lib.WebPAnimDecoderGetNext(dec, ctypes.byref(buf), ctypes.byref(ts)) or not buf:
            raise DecodeError(DTERROR_INVALID_DATA, 'bad WebP animation')
        rgba = ctypes.string_at(buf, w * h * 4)
    finally:
        lib.WebPAnimDecoderDelete(dec)
    return w, h, rgba_to_rgb(rgba, w, h)


class HeifError(ctypes.Structure):
    _fields_ = [('code', ctypes.c_int), ('subcode', ctypes.c_int), ('message', ctypes.c_char_p)]


def decode_heif(data, maxw, maxh):
    lib = load('heif')
    if not lib:
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libheif is not installed')
    vp = ctypes.c_void_p
    lib.heif_context_alloc.restype = vp
    lib.heif_context_free.argtypes = [vp]
    lib.heif_context_set_maximum_image_size_limit.argtypes = [vp, ctypes.c_int]
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
    lib.heif_image_get_width.argtypes = [vp, ctypes.c_int]
    lib.heif_image_get_height.argtypes = [vp, ctypes.c_int]
    lib.heif_image_get_plane_readonly.restype = ctypes.POINTER(ctypes.c_ubyte)
    lib.heif_image_get_plane_readonly.argtypes = [vp, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.heif_image_release.argtypes = [vp]
    lib.heif_image_handle_release.argtypes = [vp]
    HEIF_COLORSPACE_RGB, HEIF_CHROMA_INTERLEAVED_RGB, HEIF_CHANNEL_INTERLEAVED = 1, 10, 10

    ctx = lib.heif_context_alloc()
    handle, img, scaled = vp(), vp(), vp()
    try:
        lib.heif_context_set_maximum_image_size_limit(ctx, int(MAX_PIXELS ** 0.5))
        for e in (lib.heif_context_read_from_memory_without_copy(ctx, data, len(data), None),
                  lib.heif_context_get_primary_image_handle(ctx, ctypes.byref(handle))):
            if e.code:
                raise DecodeError(DTERROR_INVALID_DATA, (e.message or b'bad HEIF data').decode('latin-1'))
        check_size(lib.heif_image_handle_get_width(handle), lib.heif_image_handle_get_height(handle))
        e = lib.heif_decode_image(handle, ctypes.byref(img), HEIF_COLORSPACE_RGB,
            HEIF_CHROMA_INTERLEAVED_RGB, None)
        if e.code:
            raise DecodeError(DTERROR_INVALID_DATA, (e.message or b'cannot decode').decode('latin-1'))
        out = img
        w = lib.heif_image_get_width(out, HEIF_CHANNEL_INTERLEAVED)
        h = lib.heif_image_get_height(out, HEIF_CHANNEL_INTERLEAVED)
        check_size(w, h)
        nw, nh = fit(w, h, maxw, maxh)
        if (nw, nh) != (w, h):
            if not lib.heif_image_scale_image(img, ctypes.byref(scaled), nw, nh, None).code:
                out = scaled
                # The size of what was made, not what was asked for
                w = lib.heif_image_get_width(out, HEIF_CHANNEL_INTERLEAVED)
                h = lib.heif_image_get_height(out, HEIF_CHANNEL_INTERLEAVED)
        stride = ctypes.c_int(0)
        plane = lib.heif_image_get_plane_readonly(out, HEIF_CHANNEL_INTERLEAVED, ctypes.byref(stride))
        if not plane or w <= 0 or h <= 0 or stride.value < w * 3:
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
        if lib.JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) or \
                lib.JxlDecoderSetInput(dec, data, len(data)):
            raise DecodeError(DTERROR_INVALID_DATA, 'bad JPEG XL data')
        lib.JxlDecoderCloseInput(dec)
        for _ in range(16):     # a few events, never an endless loop
            st = lib.JxlDecoderProcessInput(dec)
            if st == JXL_DEC_BASIC_INFO:
                info = ctypes.create_string_buffer(512)     # JxlBasicInfo is smaller
                if lib.JxlDecoderGetBasicInfo(dec, info):
                    raise DecodeError(DTERROR_INVALID_DATA, 'bad JPEG XL data')
                w, h = struct.unpack_from('=II', info.raw, 4)
                check_size(w, h)    # before libjxl makes its frame buffers
            elif st == JXL_DEC_NEED_IMAGE_OUT_BUFFER and out is None:
                size = ctypes.c_size_t(0)
                if lib.JxlDecoderImageOutBufferSize(dec, ctypes.byref(fmt), ctypes.byref(size)) or \
                        size.value != w * h * 3:
                    raise DecodeError(DTERROR_INVALID_DATA, 'bad JPEG XL data')
                out = ctypes.create_string_buffer(size.value)
                if lib.JxlDecoderSetImageOutBuffer(dec, ctypes.byref(fmt), out, size.value):
                    raise DecodeError(DTERROR_INVALID_DATA, 'bad JPEG XL data')
            elif st in (JXL_DEC_FULL_IMAGE, JXL_DEC_SUCCESS):
                break       # the first frame only
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
            check_size(w, h)
            raster = ctypes.create_string_buffer(w * h * 4)
            if not lib.TIFFReadRGBAImageOriented(tif, w, h, raster, ORIENTATION_TOPLEFT, 0):
                raise DecodeError(DTERROR_INVALID_DATA, 'cannot read this TIFF')
        finally:
            lib.TIFFClose(tif)
    # Each pixel is a little-endian ABGR word: bytes R, G, B, A
    return w, h, rgba_to_rgb(raster.raw, w, h)


def rgba_to_rgb(rgba, w, h):
    rgb = bytearray(w * h * 3)
    rgb[0::3] = rgba[0::4]
    rgb[1::3] = rgba[1::4]
    rgb[2::3] = rgba[2::4]
    return bytes(rgb)


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


def decode_all(data, maxw, maxh, mode, colours, bufsize):
    # Everything that is done with the file; runs in the child process.
    # Returns (w, h, file w, file h, format, palette or None, pixels).
    fmt = detect(data)
    if fmt is None:
        raise DecodeError(DTERROR_UNKNOWN_DATATYPE, 'not a picture this service knows')
    w, h, rgb = DECODERS[fmt](data, maxw, maxh)
    if len(rgb) != w * h * 3:
        raise DecodeError(DTERROR_INVALID_DATA, 'bad picture data')
    src = (w, h)
    nw, nh = fit(w, h, maxw, maxh)
    if (nw, nh) != (w, h):
        rgb = scale(w, h, rgb, nw, nh)
        w, h = nw, nh
    if mode == 'pal':
        palette, pixels = quantize(w, h, rgb, max(2, min(256, colours)))
    else:
        palette, pixels = None, rgb
    # Never more than the Amiga's buffer at once
    if w * (1 if mode == 'pal' else 3) > bufsize or (palette and len(palette) > bufsize):
        raise DecodeError(ERROR_NO_FREE_STORE, 'picture too wide for the buffer')
    return w, h, src[0], src[1], fmt, palette, pixels


def child(data, args, fd):
    # The child process: limits, decoding, the result into the pipe
    resource.setrlimit(resource.RLIMIT_DATA, (CHILD_MEM, CHILD_MEM))
    resource.setrlimit(resource.RLIMIT_CPU, (DECODE_SECONDS, DECODE_SECONDS + 5))
    try:
        w, h, fw, fh, fmt, palette, pixels = decode_all(data, *args)
        head = struct.pack('=BIIIII', 0, w, h, fw, fh, len(palette or b'')) + fmt.encode().ljust(16)
        out = [head, palette or b'', pixels]
    except DecodeError as e:
        out = [struct.pack('=BI', 1, e.code) + str(e).encode('latin-1', 'replace')[:200]]
    except MemoryError:
        out = [struct.pack('=BI', 1, ERROR_NO_FREE_STORE) + b'not enough memory on the Pi']
    except Exception as e:
        out = [struct.pack('=BI', 1, DTERROR_INVALID_DATA) + ('cannot decode: %s' % e).encode('latin-1', 'replace')[:200]]
    for part in out:
        view = memoryview(part)
        while view:
            n = os.write(fd, view)
            view = view[n:]


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
        self.pixels = None
        self.palette = None
        self.next_row = 0
        self.bpp = 3
        self.pid = None             # the decoding child
        self.pipe = None
        self.result = bytearray()
        self.deadline = 0
        self.mode = 'rgb'

    def send(self, line):
        self.service.send(self.stream_id, line.encode('latin-1')[:MAX_LINE])

    def command(self, line):
        words = line.split()
        if not words:
            return
        cmd, args = words[0], [int(a) if a.isdigit() else a for a in words[1:]]
        nums = all(isinstance(a, int) for a in args)
        if cmd == 'BUF' and len(args) == 2 and nums and args[1] > 0:
            self.buf = (args[0], args[1])
        elif cmd == 'FILE' and len(args) == 1 and nums:
            if args[0] > MAX_FILE:
                self.send('ERR %d file too large' % ERROR_NO_FREE_STORE)
                return
            self.size = args[0]
            self.data = bytearray()
        elif cmd == 'DATA' and len(args) == 1 and nums and self.buf:
            n = min(args[0], self.buf[1], self.size - len(self.data))
            if n > 0:
                self.data += self.service.read_mem(self.buf[0], n)
            self.send('TAKEN')
        elif cmd == 'DECODE' and len(args) >= 3 and isinstance(args[0], int) and isinstance(args[1], int) \
                and self.buf and self.pid is None:
            colours = args[3] if len(args) > 3 and isinstance(args[3], int) else 256
            self.start_decode(max(16, args[0]), max(16, args[1]), args[2], colours)
        elif cmd == 'NEXT' and self.pid is None:
            self.next_block()
        else:
            logger.warning('Unexpected command %r', line[:40])

    def start_decode(self, maxw, maxh, mode, colours):
        self.mode = 'pal' if mode == 'pal' else 'rgb'
        data = bytes(self.data)
        self.data = bytearray()
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(r)
            try:
                child(data, (maxw, maxh, self.mode, colours, self.buf[1]), w)
            finally:
                os._exit(0)
        os.close(w)
        os.set_blocking(r, False)
        self.pid, self.pipe = pid, r
        self.result = bytearray()
        self.deadline = time.time() + DECODE_SECONDS + 5

    def child_output(self):
        # Reads what the child has written; True when it has finished
        try:
            d = os.read(self.pipe, 1 << 20)
        except BlockingIOError:
            return False
        if d:
            self.result += d
            return False
        self.finish_decode()
        return True

    def finish_decode(self, timed_out=False):
        if timed_out:
            os.kill(self.pid, signal.SIGKILL)
        os.close(self.pipe)
        _, status = os.waitpid(self.pid, 0)
        self.pid = self.pipe = None
        r = bytes(self.result)
        self.result = bytearray()
        if timed_out:
            self.send('ERR %d decoding took too long' % DTERROR_INVALID_DATA)
        elif r[:1] == b'\1' and len(r) >= 5:
            self.send('ERR %d %s' % (struct.unpack_from('=I', r, 1)[0], r[5:].decode('latin-1')))
        elif r[:1] == b'\0' and len(r) >= 37:
            w, h, fw, fh, plen = struct.unpack_from('=IIIII', r, 1)
            fmt = r[21:37].rstrip(b' ').decode()
            self.bpp = 1 if self.mode == 'pal' else 3
            self.palette = r[37:37 + plen] or None
            self.pixels = r[37 + plen:]
            if len(self.pixels) != w * h * self.bpp:
                self.pixels = self.palette = None
                self.send('ERR %d bad decoder result' % DTERROR_INVALID_DATA)
                return
            self.w, self.h, self.next_row = w, h, 0
            logger.info('%s %dx%d, sent as %dx%d %s', fmt, fw, fh, w, h, self.mode)
            self.send('INFO %d %d %d %d %s' % (w, h, fw, fh, fmt))
        else:
            # Killed: a crash in a decoder, or over its memory or time
            logger.warning('A decoder stopped (status %d)', status)
            self.send('ERR %d the picture could not be decoded' % DTERROR_INVALID_DATA)

    def next_block(self):
        if self.pixels is None or not self.buf:
            self.send('DONE')
            return
        address, size = self.buf
        if self.palette is not None:
            if len(self.palette) > size:
                self.pixels = self.palette = None
                self.send('ERR %d palette too large' % DTERROR_INVALID_DATA)
                return
            self.service.write_mem(address, self.palette)
            self.send('PAL %d' % (len(self.palette) // 3))
            self.palette = None
            return
        row = self.w * self.bpp
        if self.next_row >= self.h:
            self.pixels = None
            self.send('DONE')
            return
        if row > size:      # checked when decoding, but never write past the buffer
            self.pixels = None
            self.send('ERR %d picture too wide' % ERROR_NO_FREE_STORE)
            return
        n = min(size // row, self.h - self.next_row)
        y = self.next_row
        self.service.write_mem(address, self.pixels[y * row:(y + n) * row])
        self.next_row += n
        self.send('ROWS %d %d' % (y, n))

    def close(self):
        if self.pid is not None:
            os.kill(self.pid, signal.SIGKILL)
            os.close(self.pipe)
            os.waitpid(self.pid, 0)
            self.pid = self.pipe = None


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
            try:
                job.command(payload.decode('latin-1'))
            except Exception as e:
                # Never the service: bad commands only fail their picture
                logger.warning('Command %r failed: %s', payload[:40], e)
                job.send('ERR %d %s' % (DTERROR_INVALID_DATA, e))
        elif ptype in (A314d.MSG_EOS, A314d.MSG_RESET):
            if ptype == A314d.MSG_EOS:
                self.a314d.send_eos(stream_id)
            job.close()
            del self.jobs[stream_id]

    def run(self):
        while True:
            while self.pending:
                self.handle_msg(*self.pending.pop(0))
            pipes = {j.pipe: j for j in self.jobs.values() if j.pipe is not None}
            rl, _, _ = select.select([self.a314d] + list(pipes), [], [], 1.0)
            for fd in rl:
                if fd in pipes:
                    pipes[fd].child_output()
            if self.a314d in rl:
                self.handle_msg(*self.a314d.wait_for_msg())
            now = time.time()
            for j in list(self.jobs.values()):
                if j.pid is not None and now > j.deadline:
                    j.finish_decode(timed_out=True)


if __name__ == '__main__':
    service = PictureService()
    logger.info('%s service is running', SERVICE_NAME)
    try:
        service.run()
    except KeyboardInterrupt:
        pass
    service.a314d.close()
