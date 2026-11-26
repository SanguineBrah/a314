#!/usr/bin/python3
# Makes the datatype descriptors (DEVS:DataTypes/...) that let datatypes
# recognise the formats a314picture.datatype shows and a314sound.datatype
# plays. Each is an IFF FORM
# DTYP with a DTHD chunk (struct DataTypeHeader), the same layout as the
# Workbench's own descriptors.
#
#   make_descriptors.py <directory>

import os
import struct
import sys

# File name, name, ID, the first bytes of such files, and which of those
# bytes can be anything (a size). Every name is different, and ends in
# "(A314)", so that none is taken for another datatype's descriptor (for
# example one for WebP that a user already has).
PICTURES = [
    ('WebP-A314', 'WebP (A314)', b'webp', b'RIFF' + bytes(4) + b'WEBP', range(4, 8)),
    ('AVIF-A314', 'AVIF (A314)', b'avif', bytes(4) + b'ftypavif', range(0, 4)),
    ('AVIF-Sequence-A314', 'AVIF sequence (A314)', b'avis', bytes(4) + b'ftypavis', range(0, 4)),
    ('HEIC-A314', 'HEIC (A314)', b'heic', bytes(4) + b'ftypheic', range(0, 4)),
    ('HEIC-Extended-A314', 'HEIC extended (A314)', b'heix', bytes(4) + b'ftypheix', range(0, 4)),
    ('HEIF-A314', 'HEIF (A314)', b'heif', bytes(4) + b'ftypmif1', range(0, 4)),
    ('JPEG-XL-A314', 'JPEG XL (A314)', b'jxl ', b'\xff\x0a', ()),
    ('JPEG-XL-Container-A314', 'JPEG XL container (A314)', b'jxlc', b'\0\0\0\x0cJXL \r\n\x87\n', ()),
    ('TIFF-Intel-A314', 'TIFF Intel (A314)', b'tiff', b'II*\0', ()),
    ('TIFF-Motorola-A314', 'TIFF Motorola (A314)', b'tifm', b'MM\0*', ()),
]

SOUNDS = [
    ('MP3-ID3-A314', 'MP3 (A314)', b'mp3 ', b'ID3', ()),
    ('MP3-Frame-A314', 'MP3 frame (A314)', b'mp3f', b'\xff\xfb', ()),
    ('MP3-Frame-CRC-A314', 'MP3 frame with CRC (A314)', b'mp3c', b'\xff\xfa', ()),
    ('MP3-MPEG2-A314', 'MP3 MPEG-2 (A314)', b'mp32', b'\xff\xf3', ()),
    ('MP3-MPEG2-CRC-A314', 'MP3 MPEG-2 with CRC (A314)', b'mp3d', b'\xff\xf2', ()),
    ('FLAC-A314', 'FLAC (A314)', b'flac', b'fLaC', ()),
    ('Ogg-A314', 'Ogg (A314)', b'ogg ', b'OggS', ()),
]

DTF_BINARY, DTF_CASE = 0x0000, 0x0010      # the mask is compared exactly
GID_PICTURE, GID_SOUND = b'pict', b'soun'
VERSION = b'$VER: %s 1.0 (25.11.2026)'


def chunk(cid, body):
    data = cid + struct.pack('>I', len(body)) + body
    return data + (b'\0' if len(body) & 1 else b'')


def descriptor(name, did, pattern, wild, base_name, gid):
    n = name.encode('latin-1')
    mask = [-1 if i in wild else b for i, b in enumerate(pattern)]
    # struct DataTypeHeader; the pointers are offsets into the chunk
    mask_off = 32
    strings_off = mask_off + 2 * len(mask)
    name_off = strings_off
    base_off = name_off + len(n) + 1
    pattern_off = base_off + len(base_name) + 1
    header = struct.pack('>IIIIIIhhHH', name_off, base_off, pattern_off, mask_off,
        struct.unpack('>I', gid)[0], struct.unpack('>I', did)[0],
        len(mask), 0, DTF_BINARY | DTF_CASE, 0)     # priority 0
    body = header + struct.pack('>%dh' % len(mask), *mask) + n + b'\0' + base_name + b'\0' + b'#?\0'
    form = b'DTYP' + chunk(b'FVER', VERSION % n + b'\0') + chunk(b'NAME', n) + chunk(b'DTHD', body)
    return b'FORM' + struct.pack('>I', len(form)) + form


if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for formats, base_name, gid in ((PICTURES, b'a314picture', GID_PICTURE),
                                    (SOUNDS, b'a314sound', GID_SOUND)):
        for filename, name, did, pattern, wild in formats:
            with open(os.path.join(out, filename), 'wb') as f:
                f.write(descriptor(name, did, pattern, wild, base_name, gid))
