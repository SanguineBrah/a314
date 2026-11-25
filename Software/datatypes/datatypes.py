#!/usr/bin/python3
# -*- coding: utf-8 -*-

# Datatypes service: decodes pictures and sounds for a314picture.datatype
# and a314sound.datatype, so that Amiga programs that use datatypes
# (MultiView and many others) can show and play formats that the Amiga has
# no datatype for: pictures in WebP, AVIF, HEIC, JPEG XL and TIFF, and
# sounds in MP3, FLAC and Ogg (Vorbis and Opus). Pictures larger than the
# Amiga asks for are made smaller; sounds are resampled for Paula.
#
# The decoding is done by the usual libraries, through ctypes: libwebp,
# libheif (with its AVIF and HEIC plugins), libjxl, libtiff, libsndfile
# (and libmpg123 for MP3 if libsndfile can't), and libsamplerate. Each is
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
#                           decode a picture, no larger than maxw x maxh;
#                           mode is rgb (3 bytes a pixel) or pal (a palette
#                           of up to <colours> colours, then 1 byte a pixel)
#   SOUND <rate> <channels> <maxbytes> <clock>
#                           decode a sound into 8-bit samples, at most this
#                           rate, 1 or 2 channels, at most maxbytes for each
#                           channel; clock is the Amiga's audio clock (for
#                           the period)
#   NEXT                    the next block, after INFO, PAL, ROWS, SINFO
#                           or PCM
#
# Pi -> Amiga:
#   TAKEN                   the DATA was read
#   INFO <w> <h> <file w> <file h> <format>   decoded; NEXT for the data
#   ERR <code> <text>       code: 2000 unknown format, 2008 bad data,
#                           2009 not available (no library), 103 too large
#   PAL <n>                 n colours (r, g, b) are in the buffer
#   ROWS <y> <n>            rows y to y + n - 1 are in the buffer
#   SINFO <rate> <channels> <length> <period> <file rate> <seconds> <format>
#                           decoded: length bytes for each channel; the
#                           sound was cut short if it was longer
#   PCM <channel> <offset> <n>   n bytes of a channel are in the buffer
#   DONE                    no more rows or samples
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

SERVICE_NAME = 'datatypes'

MAX_FILE = 64 * 1024 * 1024
MAX_LINE = 251
DECODE_SECONDS = 45             # the Amiga waits 60
MAX_CHILDREN = 2                # decodings at once; others wait
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

    # libtiff reads files, so the data goes into a temporary one, which is
    # deleted as soon as libtiff has it open (so that nothing is left behind
    # if this process is killed)
    fd, path = tempfile.mkstemp(suffix='.tif')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
        tif = lib.TIFFOpen(path.encode(), b'r')
    finally:
        os.unlink(path)
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


# ---------------------------------------------------------------------------
# Sounds

class SfInfo(ctypes.Structure):
    _fields_ = [('frames', ctypes.c_int64), ('samplerate', ctypes.c_int), ('channels', ctypes.c_int),
                ('format', ctypes.c_int), ('sections', ctypes.c_int), ('seekable', ctypes.c_int)]

SOUND_FORMATS = {0x170000: 'FLAC', 0x200000: 'Ogg', 0x230000: 'MP3'}
MAX_SOUND_BYTES = 64 * 1024 * 1024


def detect_sound(data):
    if data[:4] == b'fLaC':
        return 'FLAC'
    if data[:4] == b'OggS':
        return 'Ogg'
    if data[:3] == b'ID3' or (len(data) > 1 and data[0] == 0xff and data[1] & 0xe0 == 0xe0):
        return 'MP3'
    return None


def read_sndfile(data, max_frames):
    # Returns (rate, channels, frames as float32 bytes, format name)
    lib = load('sndfile')
    if not lib:
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libsndfile is not installed')
    vp = ctypes.c_void_p
    SFM_READ = 0x10
    lib.sf_open.restype = vp
    lib.sf_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(SfInfo)]
    lib.sf_readf_float.restype = ctypes.c_int64
    lib.sf_readf_float.argtypes = [vp, vp, ctypes.c_int64]
    lib.sf_close.argtypes = [vp]
    fd, path = tempfile.mkstemp(suffix='.snd')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
        info = SfInfo()
        snd = lib.sf_open(path.encode(), SFM_READ, ctypes.byref(info))
    finally:
        os.unlink(path)
    if not snd:
        return None
    try:
        rate, ch = info.samplerate, info.channels
        if not 1000 <= rate <= 384000 or not 1 <= ch <= 16:
            raise DecodeError(DTERROR_INVALID_DATA, 'bad sound format')
        chunks, total = [], 0
        block = 65536
        buf = ctypes.create_string_buffer(block * ch * 4)
        while total < max_frames:
            n = lib.sf_readf_float(snd, buf, min(block, max_frames - total))
            if n <= 0:
                break
            chunks.append(buf.raw[:n * ch * 4])
            total += n
    finally:
        lib.sf_close(snd)
    return rate, ch, b''.join(chunks), SOUND_FORMATS.get(info.format & 0x0FFF0000, 'sound')


def read_mp3(data, max_frames):
    # MP3 with libmpg123, if libsndfile can't (older libsndfile has no MP3)
    lib = load('mpg123')
    if not lib:
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libmpg123 is not installed')
    vp = ctypes.c_void_p
    MPG123_ENC_FLOAT_32, MPG123_NEED_MORE, MPG123_NEW_FORMAT, MPG123_OK = 0x200, -10, -11, 0
    lib.mpg123_new.restype = vp
    lib.mpg123_new.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
    lib.mpg123_delete.argtypes = [vp]
    lib.mpg123_open_feed.argtypes = [vp]
    lib.mpg123_feed.argtypes = [vp, ctypes.c_char_p, ctypes.c_size_t]
    lib.mpg123_read.argtypes = [vp, vp, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    lib.mpg123_format_none.argtypes = [vp]
    lib.mpg123_format.argtypes = [vp, ctypes.c_long, ctypes.c_int, ctypes.c_int]
    lib.mpg123_getformat.argtypes = [vp, ctypes.POINTER(ctypes.c_long), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
    err = ctypes.c_int(0)
    mh = lib.mpg123_new(None, ctypes.byref(err))
    if not mh:
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libmpg123 failed')
    try:
        lib.mpg123_format_none(mh)
        for r in (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000):
            lib.mpg123_format(mh, r, 3, MPG123_ENC_FLOAT_32)   # mono or stereo
        lib.mpg123_open_feed(mh)
        lib.mpg123_feed(mh, data, len(data))
        out = ctypes.create_string_buffer(65536)
        done = ctypes.c_size_t(0)
        chunks, total = [], 0
        rate, ch = ctypes.c_long(0), ctypes.c_int(0)
        enc = ctypes.c_int(0)
        for _ in range(10000000):
            r = lib.mpg123_read(mh, out, len(out), ctypes.byref(done))
            if r == MPG123_NEW_FORMAT:
                lib.mpg123_getformat(mh, ctypes.byref(rate), ctypes.byref(ch), ctypes.byref(enc))
                continue
            if done.value:
                chunks.append(out.raw[:done.value])
                total += done.value
                if ch.value and total >= max_frames * ch.value * 4:
                    break
            if r != MPG123_OK:
                break
    finally:
        lib.mpg123_delete(mh)
    if not rate.value or not ch.value or not total:
        raise DecodeError(DTERROR_INVALID_DATA, 'bad MP3 data')
    return rate.value, ch.value, b''.join(chunks), 'MP3'


def resample(samples, rate, out_rate):
    # One channel of float32 samples to out_rate (libsamplerate)
    if rate == out_rate:
        return samples
    lib = load('samplerate')
    if not lib:
        raise DecodeError(DTERROR_NOT_AVAILABLE, 'libsamplerate is not installed')

    class SrcData(ctypes.Structure):
        _fields_ = [('data_in', ctypes.c_void_p), ('data_out', ctypes.c_void_p),
                    ('input_frames', ctypes.c_long), ('output_frames', ctypes.c_long),
                    ('input_frames_used', ctypes.c_long), ('output_frames_gen', ctypes.c_long),
                    ('end_of_input', ctypes.c_int), ('src_ratio', ctypes.c_double)]

    lib.src_simple.argtypes = [ctypes.POINTER(SrcData), ctypes.c_int, ctypes.c_int]
    SRC_SINC_MEDIUM_QUALITY = 1
    n = len(samples) // 4
    out_n = int(n * out_rate / rate) + 16
    inbuf = ctypes.create_string_buffer(samples, len(samples))
    outbuf = ctypes.create_string_buffer(out_n * 4)
    d = SrcData(ctypes.cast(inbuf, ctypes.c_void_p), ctypes.cast(outbuf, ctypes.c_void_p),
                n, out_n, 0, 0, 1, out_rate / rate)
    if lib.src_simple(ctypes.byref(d), SRC_SINC_MEDIUM_QUALITY, 1):
        raise DecodeError(DTERROR_INVALID_DATA, 'resampling failed')
    return outbuf.raw[:d.output_frames_gen * 4]


def float_to_8bit(samples):
    # float32 samples to signed 8-bit (through libsamplerate's clipping
    # conversion to 16-bit, then the high bytes)
    lib = load('samplerate')
    n = len(samples) // 4
    lib.src_float_to_short_array.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int]
    shorts = ctypes.create_string_buffer(n * 2)
    lib.src_float_to_short_array(samples, shorts, n)
    return shorts.raw[1::2]         # little-endian: the high byte of each


def decode_sound(data, rate, channels, maxbytes, clock):
    # Returns (rate, channels, [bytes for each channel], period, file rate,
    # seconds, format)
    fmt = detect_sound(data)
    if fmt is None:
        raise DecodeError(DTERROR_UNKNOWN_DATATYPE, 'not a sound this service knows')
    maxbytes = max(1, min(maxbytes, MAX_SOUND_BYTES))
    out_rate = max(4000, min(rate, 28000))
    # Enough of the file for maxbytes (at the highest likely file rate)
    max_frames = maxbytes * 48000 // out_rate + 4096
    got = read_sndfile(data, max_frames) if load('sndfile') else None
    if got is None and fmt == 'MP3':
        got = read_mp3(data, max_frames)
    if got is None:
        raise DecodeError(DTERROR_INVALID_DATA, 'cannot decode this sound')
    src_rate, ch, pcm, fmt = got
    import array
    a = array.array('f')
    a.frombytes(pcm[:len(pcm) // (4 * ch) * 4 * ch])
    frames = len(a) // ch
    if not frames:
        raise DecodeError(DTERROR_INVALID_DATA, 'no sound data')
    out_rate = min(out_rate, src_rate)
    if channels >= 2 and ch >= 2:
        planes = [a[0::ch], a[1::ch]]
    elif ch >= 2:
        left, right = a[0::ch], a[1::ch]
        planes = [array.array('f', ((l + r) * 0.5 for l, r in zip(left, right)))]
    else:
        planes = [a]
    out = []
    for plane in planes:
        res = resample(plane.tobytes(), src_rate, out_rate)
        out.append(float_to_8bit(res)[:maxbytes])
    length = min(len(c) for c in out)
    out = [c[:length] for c in out]
    period = max(124, clock // out_rate) if clock else 0
    return out_rate, len(out), out, period, src_rate, frames // src_rate, fmt


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


def child_sound(data, args, fd):
    try:
        rate, ch, chans, period, src_rate, secs, fmt = decode_sound(data, *args)
        head = struct.pack('=BIIIIII', 2, rate, ch, len(chans[0]), period, src_rate, secs) + fmt.encode().ljust(16)
        out = [head] + chans
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


def child(data, args, fd, close_fds):
    # The child process: limits, decoding, the result into the pipe
    for f in close_fds:
        try:
            os.close(f)
        except OSError:
            pass
    resource.setrlimit(resource.RLIMIT_DATA, (CHILD_MEM, CHILD_MEM))
    resource.setrlimit(resource.RLIMIT_CPU, (DECODE_SECONDS, DECODE_SECONDS + 5))
    if args[0] == 'sound':
        child_sound(data, args[1:], fd)
        return
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
        self.waiting = None         # a DECODE or SOUND waiting for a free child
        self.sound = None           # [bytes for each channel] of a sound
        self.pos = (0, 0)           # (channel, offset) of the next PCM

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
            self.waiting = ('picture', max(16, args[0]), max(16, args[1]), args[2], colours)
            self.service.start_waiting()
        elif cmd == 'SOUND' and len(args) == 4 and nums and self.buf and self.pid is None:
            self.waiting = ('sound', args[0], max(1, min(2, args[1])), args[2], args[3])
            self.service.start_waiting()
        elif cmd == 'NEXT' and self.pid is None and self.waiting is None:
            self.next_block()
        else:
            logger.warning('Unexpected command %r', line[:40])

    def start_decode(self):
        if self.waiting[0] == 'sound':
            args = self.waiting
        else:
            _, maxw, maxh, mode, colours = self.waiting
            self.mode = 'pal' if mode == 'pal' else 'rgb'
            args = (maxw, maxh, self.mode, colours, self.buf[1])
        self.waiting = None
        data = self.data
        self.data = bytearray()
        # The child closes what it doesn't need of the parent's: the a314d
        # socket and the other children's pipes
        others = [self.service.a314d.fileno()] + \
            [j.pipe for j in self.service.jobs.values() if j.pipe is not None]
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(r)
            try:
                child(bytes(data), args, w, others)
            finally:
                os._exit(0)
        os.close(w)
        os.set_blocking(r, False)
        self.pid, self.pipe = pid, r
        self.result = bytearray()
        self.deadline = time.monotonic() + DECODE_SECONDS + 5

    def child_output(self):
        # Reads what the child has written; True when it has finished
        try:
            d = os.read(self.pipe, 1 << 20)
        except BlockingIOError:
            return False
        if d:
            self.result += d
            # Still sending its result: not stuck
            self.deadline = max(self.deadline, time.monotonic() + 5)
            return False
        self.finish_decode()
        return True

    def finish_decode(self, timed_out=False):
        if timed_out:
            os.kill(self.pid, signal.SIGKILL)
        os.close(self.pipe)
        _, status = os.waitpid(self.pid, 0)
        self.pid = self.pipe = None
        r = self.result
        self.result = bytearray()
        if timed_out:
            self.send('ERR %d decoding took too long' % DTERROR_INVALID_DATA)
        elif r[:1] == b'\1' and len(r) >= 5:
            self.send('ERR %d %s' % (struct.unpack_from('=I', r, 1)[0], bytes(r[5:]).decode('latin-1')))
        elif r[:1] == b'\2' and len(r) >= 41:
            rate, ch, length, period, src_rate, secs = struct.unpack_from('=IIIIII', r, 1)
            fmt = bytes(r[25:41]).rstrip(b' ').decode()
            data = memoryview(r)[41:]
            if not 1 <= ch <= 2 or len(data) != ch * length or not length:
                self.send('ERR %d bad decoder result' % DTERROR_INVALID_DATA)
                return
            self.sound = [data[i * length:(i + 1) * length] for i in range(ch)]
            self.pos = (0, 0)
            logger.info('%s %d Hz, %d s, sent as %d channel(s) at %d Hz', fmt, src_rate, secs, ch, rate)
            self.send('SINFO %d %d %d %d %d %d %s' % (rate, ch, length, period, src_rate, secs, fmt))
        elif r[:1] == b'\0' and len(r) >= 37:
            w, h, fw, fh, plen = struct.unpack_from('=IIIII', r, 1)
            fmt = bytes(r[21:37]).rstrip(b' ').decode()
            self.bpp = 1 if self.mode == 'pal' else 3
            self.palette = bytes(r[37:37 + plen]) or None
            self.pixels = memoryview(r)[37 + plen:]    # not another copy
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
        if self.sound is not None and self.buf:
            ch, off = self.pos
            if ch >= len(self.sound):
                self.sound = None
                self.send('DONE')
                return
            data = self.sound[ch]
            n = min(self.buf[1], len(data) - off)
            self.service.write_mem(self.buf[0], bytes(data[off:off + n]))
            self.send('PCM %d %d %d' % (ch, off, n))
            self.pos = (ch, off + n) if off + n < len(data) else (ch + 1, 0)
            return
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
        self.service.write_mem(address, bytes(self.pixels[y * row:(y + n) * row]))
        self.next_row += n
        self.send('ROWS %d %d' % (y, n))

    def close(self):
        self.waiting = None
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
        # Loaded once here, so that every child has them already
        for name in ('webp', 'webpdemux', 'heif', 'jxl', 'tiff', 'sndfile', 'mpg123', 'samplerate'):
            load(name)

    def start_waiting(self):
        # Starts waiting decodings while there are free children
        running = sum(1 for j in self.jobs.values() if j.pid is not None)
        for j in self.jobs.values():
            if running >= MAX_CHILDREN:
                break
            if j.waiting is not None and j.pid is None:
                j.start_decode()
                running += 1

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
            self.start_waiting()

    def run(self):
        while True:
            while self.pending:
                self.handle_msg(*self.pending.pop(0))
            pipes = {j.pipe: j for j in self.jobs.values() if j.pipe is not None}
            rl, _, _ = select.select([self.a314d] + list(pipes), [], [], 1.0)
            for fd in rl:
                if fd in pipes and pipes[fd].child_output():
                    self.start_waiting()
            if self.a314d in rl:
                self.handle_msg(*self.a314d.wait_for_msg())
            now = time.monotonic()
            for j in list(self.jobs.values()):
                if j.pid is not None and now > j.deadline:
                    j.finish_decode(timed_out=True)
                    self.start_waiting()


if __name__ == '__main__':
    service = PictureService()
    logger.info('%s service is running', SERVICE_NAME)
    try:
        service.run()
    except KeyboardInterrupt:
        pass
    service.a314d.close()
