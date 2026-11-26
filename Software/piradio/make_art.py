#!/usr/bin/python3
# Generates the artwork for the PiRadio Amiga program.
#
#   python3 make_art.py [preview.png]
#
# Writes piradio_art.c and piradio_art.h next to this script:
#  - the 640x256, 16-colour background (ByteRun1-compressed bitplanes),
#  - an atlas of alternate-state images (pressed keys/buttons, lit lamps),
#  - a proportional bitmap font used for all text on the panel,
#  - layout constants, so that the C code draws its dynamic parts (needle,
#    scale, digits, pointers) exactly where the artwork expects them.
#
# Everything is drawn procedurally with ordered dithering. Hires PAL pixels are
# roughly twice as tall as they are wide, which is why horizontal radii are
# about twice the vertical ones throughout.

import math
import os
import struct
import sys
import zlib

W, H = 640, 256
DEPTH = 4
ASPECT = 1.85   # hires PAL pixel height / width

# ---------------------------------------------------------------------------
# Palette (12-bit Amiga colours)

PALETTE = [
    0x000,  # 0  black
    0x210,  # 1  deep walnut / dial ink
    0x421,  # 2  dark walnut
    0x742,  # 3  walnut
    0xA63,  # 4  light walnut / deep amber
    0xDA6,  # 5  dial glow
    0xFEB,  # 6  dial cream
    0x223,  # 7  gunmetal dark
    0x446,  # 8  gunmetal
    0x778,  # 9  steel
    0xAAB,  # 10 light steel
    0xEEF,  # 11 highlight
    0xE32,  # 12 red
    0x5EA,  # 13 VFD green
    0x021,  # 14 VFD ghost
    0xFA3,  # 15 amber
]

BLACK, INK, WOOD_D, WOOD, WOOD_L, GLOW, CREAM = 0, 1, 2, 3, 4, 5, 6
GUN_D, GUN, STEEL, STEEL_L, HILITE = 7, 8, 9, 10, 11
RED, VFD, VFD_DIM, AMBER = 12, 13, 14, 15

R_WOOD = [INK, WOOD_D, WOOD, WOOD_L]
R_GLOW = [WOOD, WOOD_L, GLOW, CREAM, HILITE]
R_METAL = [BLACK, GUN_D, GUN, STEEL, STEEL_L, HILITE]
R_PANEL = [BLACK, GUN_D, GUN, STEEL]
R_IVORY = [WOOD_L, GLOW, CREAM, HILITE]

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def bayer(x, y):
    return (BAYER[y & 3][x & 3] + 0.5) / 16.0


def shade(ramp, t, x, y):
    """Pick a ramp colour for brightness t (0..1) with ordered dithering."""
    t = max(0.0, min(1.0, t))
    v = t * (len(ramp) - 1)
    i = int(v)
    if i >= len(ramp) - 1:
        return ramp[-1]
    return ramp[i + 1] if (v - i) > bayer(x, y) else ramp[i]


def _hash(ix, iy, seed):
    n = (ix * 374761393 + iy * 668265263 + seed * 1442695041) & 0xffffffff
    n = ((n ^ (n >> 13)) * 1274126177) & 0xffffffff
    return ((n ^ (n >> 16)) & 0xffff) / 65535.0


def noise(x, y, seed=0):
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a = _hash(ix, iy, seed)
    b = _hash(ix + 1, iy, seed)
    c = _hash(ix, iy + 1, seed)
    d = _hash(ix + 1, iy + 1, seed)
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy


# ---------------------------------------------------------------------------
# Font: 7 rows cap height, vertical stems two pixels wide (hires).

GLYPHS = {
    ' ': ['   '] * 7,
    '0': [' #### ', '##  ##', '## ###', '######', '### ##', '##  ##', ' #### '],
    '1': ['  ## ', ' ### ', '  ## ', '  ## ', '  ## ', '  ## ', ' ####'],
    '2': [' #### ', '##  ##', '    ##', '  ### ', ' ##   ', '##    ', '######'],
    '3': [' #### ', '##  ##', '    ##', '  ### ', '    ##', '##  ##', ' #### '],
    '4': ['   ###', '  ####', ' ## ##', '##  ##', '######', '    ##', '    ##'],
    '5': ['######', '##    ', '##### ', '    ##', '    ##', '##  ##', ' #### '],
    '6': ['  ### ', ' ##   ', '##    ', '##### ', '##  ##', '##  ##', ' #### '],
    '7': ['######', '    ##', '   ## ', '  ##  ', '  ##  ', ' ##   ', ' ##   '],
    '8': [' #### ', '##  ##', '##  ##', ' #### ', '##  ##', '##  ##', ' #### '],
    '9': [' #### ', '##  ##', '##  ##', ' #####', '    ##', '   ## ', ' ###  '],
    'A': ['  ##  ', ' #### ', '##  ##', '##  ##', '######', '##  ##', '##  ##'],
    'B': ['##### ', '##  ##', '##  ##', '##### ', '##  ##', '##  ##', '##### '],
    'C': [' #### ', '##  ##', '##    ', '##    ', '##    ', '##  ##', ' #### '],
    'D': ['####  ', '## ## ', '##  ##', '##  ##', '##  ##', '## ## ', '####  '],
    'E': ['######', '##    ', '##    ', '##### ', '##    ', '##    ', '######'],
    'F': ['######', '##    ', '##    ', '##### ', '##    ', '##    ', '##    '],
    'G': [' #### ', '##  ##', '##    ', '## ###', '##  ##', '##  ##', ' #####'],
    'H': ['##  ##', '##  ##', '##  ##', '######', '##  ##', '##  ##', '##  ##'],
    'I': ['####', ' ## ', ' ## ', ' ## ', ' ## ', ' ## ', '####'],
    'J': ['   ###', '    ##', '    ##', '    ##', '##  ##', '##  ##', ' #### '],
    'K': ['##  ##', '## ## ', '####  ', '###   ', '####  ', '## ## ', '##  ##'],
    'L': ['##    ', '##    ', '##    ', '##    ', '##    ', '##    ', '######'],
    'M': ['##   ##', '### ###', '#######', '## # ##', '##   ##', '##   ##', '##   ##'],
    'N': ['##  ##', '### ##', '######', '## ###', '##  ##', '##  ##', '##  ##'],
    'O': [' #### ', '##  ##', '##  ##', '##  ##', '##  ##', '##  ##', ' #### '],
    'P': ['##### ', '##  ##', '##  ##', '##### ', '##    ', '##    ', '##    '],
    'Q': [' #### ', '##  ##', '##  ##', '##  ##', '## ###', '##  ##', ' ### #'],
    'R': ['##### ', '##  ##', '##  ##', '##### ', '## ## ', '##  ##', '##  ##'],
    'S': [' #### ', '##  ##', '##    ', ' #### ', '    ##', '##  ##', ' #### '],
    'T': ['######', '  ##  ', '  ##  ', '  ##  ', '  ##  ', '  ##  ', '  ##  '],
    'U': ['##  ##', '##  ##', '##  ##', '##  ##', '##  ##', '##  ##', ' #### '],
    'V': ['##  ##', '##  ##', '##  ##', '##  ##', '##  ##', ' #### ', '  ##  '],
    'W': ['##   ##', '##   ##', '##   ##', '## # ##', '#######', '### ###', '##   ##'],
    'X': ['##  ##', '##  ##', ' #### ', '  ##  ', ' #### ', '##  ##', '##  ##'],
    'Y': ['##  ##', '##  ##', '##  ##', ' #### ', '  ##  ', '  ##  ', '  ##  '],
    'Z': ['######', '    ##', '   ## ', '  ##  ', ' ##   ', '##    ', '######'],
    '.': ['  ', '  ', '  ', '  ', '  ', '##', '##'],
    ',': ['   ', '   ', '   ', '   ', '   ', ' ##', '## '],
    ':': ['  ', '##', '##', '  ', '##', '##', '  '],
    '-': ['     ', '     ', '     ', '#####', '     ', '     ', '     '],
    '+': ['      ', '  ##  ', '  ##  ', '######', '  ##  ', '  ##  ', '      '],
    '=': ['     ', '     ', '#####', '     ', '#####', '     ', '     '],
    '/': ['    ##', '    ##', '   ## ', '  ##  ', ' ##   ', '##    ', '##    '],
    '!': ['##', '##', '##', '##', '##', '  ', '##'],
    '?': [' #### ', '##  ##', '    ##', '   ## ', '  ##  ', '      ', '  ##  '],
    '%': ['##   #', '##  ##', '   ## ', '  ##  ', ' ##   ', '##  ##', '#   ##'],
    '(': ['  ##', ' ## ', '##  ', '##  ', '##  ', ' ## ', '  ##'],
    ')': ['##  ', ' ## ', '  ##', '  ##', '  ##', ' ## ', '##  '],
    '<': ['   ##', '  ## ', ' ##  ', '##   ', ' ##  ', '  ## ', '   ##'],
    '>': ['##   ', ' ##  ', '  ## ', '   ##', '  ## ', ' ##  ', '##   '],
    "'": ['##', '##', '  ', '  ', '  ', '  ', '  '],
    '_': ['      ', '      ', '      ', '      ', '      ', '      ', '######'],
}
FONT_H = 8          # 7 rows of glyph plus one of spacing
FONT_BASELINE = 6
FONT_SPACING = 1


def glyph(ch):
    if 'a' <= ch <= 'z':
        ch = ch.upper()
    return GLYPHS.get(ch, GLYPHS['?'])


def text_width(s, scale=1):
    if not s:
        return 0
    return sum((len(glyph(c)[0]) + FONT_SPACING) * scale for c in s) - FONT_SPACING * scale


# ---------------------------------------------------------------------------
# Canvas

class Canvas(object):
    def __init__(self, w, h, fill=0):
        self.w, self.h = w, h
        self.px = [[fill] * w for _ in range(h)]

    def get(self, x, y):
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.px[y][x]
        return 0

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, c)

    def hline(self, x0, x1, y, c):
        self.rect(x0, y, x1, y, c)

    def vline(self, x, y0, y1, c):
        self.rect(x, y0, x, y1, c)

    def text(self, x, y, s, c, scale=1):
        for ch in s:
            g = glyph(ch)
            for gy, row in enumerate(g):
                for gx, p in enumerate(row):
                    if p == '#':
                        self.rect(x + gx * scale, y + gy * scale,
                                  x + gx * scale + scale - 1, y + gy * scale + scale - 1, c)
            x += (len(g[0]) + FONT_SPACING) * scale

    def text_mask(self, x, y, s, scale=1):
        pts = set()
        for ch in s:
            g = glyph(ch)
            for gy, row in enumerate(g):
                for gx, p in enumerate(row):
                    if p == '#':
                        for sy in range(scale):
                            for sx in range(scale):
                                pts.add((x + gx * scale + sx, y + gy * scale + sy))
            x += (len(g[0]) + FONT_SPACING) * scale
        return pts

    def engrave(self, x, y, s, paint=STEEL_L, shadow=BLACK):
        """Painted panel lettering with a crisp drop shadow."""
        m = self.text_mask(x, y, s)
        for (px, py) in m:
            if (px + 1, py + 1) not in m:
                self.set(px + 1, py + 1, shadow)
        for (px, py) in m:
            self.set(px, py, paint)

    def copy(self):
        c = Canvas(self.w, self.h)
        c.px = [row[:] for row in self.px]
        return c


def ellipse_iter(cx, cy, rx, ry, scale=1.0):
    """Yields pixels inside an ellipse with normalised coordinates."""
    rx *= scale
    ry *= scale
    for y in range(int(cy - ry) - 1, int(cy + ry) + 2):
        for x in range(int(cx - rx) - 1, int(cx + rx) + 2):
            nx = (x + 0.5 - cx) / rx
            ny = (y + 0.5 - cy) / ry
            r = math.hypot(nx, ny)
            if r <= 1.0:
                yield x, y, nx, ny, r


def rounded(x, y, x0, y0, x1, y1, r):
    """True if (x, y) lies inside a rectangle with rounded corners."""
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    ry = max(1, r / ASPECT)
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + ry), y1 - ry)
    return ((x - cx) / r) ** 2 + ((y - cy) / ry) ** 2 <= 1.0


# ---------------------------------------------------------------------------
# Layout (hires pixel coordinates)

L = {}

# Everything the user interacts with lies in the top NTSC_H lines, so that
# the program works on NTSC screens. The extra PAL lines below hold only
# decoration (a speaker grille with the nameplate).
NTSC_H = 200
L.update(NTSC_H=NTSC_H)

BEZEL = (16, 4, 623, 89)
DIAL = (24, 8, 615, 85)              # glowing face
DIAL_W = DIAL[2] - DIAL[0] + 1
DIAL_H = DIAL[3] - DIAL[1] + 1
L.update(DIAL_X=DIAL[0], DIAL_Y=DIAL[1], DIAL_W=DIAL_W, DIAL_H=DIAL_H,
         SC_X0=28, SC_W=535, TITLE_Y=4, LABEL_Y=15, TICK_Y0=25, MINOR_Y0=31,
         BASE_Y=37, MARK_Y=41, NEEDLE_Y0=12, NEEDLE_Y1=51,
         MSG_X0=14, MSG_X1=DIAL_W - 15, MSG_Y0=57, MSG_Y1=71, MSG_TEXT_Y=61,
         HELP_PITCH=8)

PANEL = (8, 93, 631, 197)
SEPARATOR_Y = 151

VFD_BOX = (24, 98, 299, 147)
L.update(VFD_X=VFD_BOX[0], VFD_Y=VFD_BOX[1], VFD_W=VFD_BOX[2] - VFD_BOX[0] + 1, VFD_H=VFD_BOX[3] - VFD_BOX[1] + 1,
         DIG_W=22, DIG_H=30, DIG_Y=10, DIG_T=4, DOT_X=118)
DIGIT_X = [10, 38, 66, 94, 128, 156, 184, 212]   # relative to VFD_X

METER = (308, 97, 451, 148)
L.update(METER_X=METER[0], METER_Y=METER[1], METER_W=METER[2] - METER[0] + 1,
         METER_H=METER[3] - METER[1] + 1)
METER_CX, METER_CY, METER_RX, METER_RY = 379, 177, 125, 70
METER_DEG0, METER_DEG1 = 117.0, 63.0
L.update(METER_CX=METER_CX - METER[0], METER_CY=METER_CY - METER[1],
         METER_RX=METER_RX, METER_RY=METER_RY,
         METER_A0=round(METER_DEG0 * 256 / 360), METER_A1=round(METER_DEG1 * 256 / 360),
         METER_PCT0=60, METER_PCT1=95)

LED_X = 460
LED_YS = [101, 112, 123, 134]
LED_LABELS = ['TUNER', 'SIGNAL', 'AUDIO', 'SCAN']
L.update(LED_X=LED_X, LED_W=16, LED_H=11)

TK = (572, 117, 40, 20)              # tuning knob cx, cy, rx, ry
L.update(TK_X=524, TK_Y=96, TK_W=100, TK_H=52,
         TK_CX=TK[0] - 524, TK_CY=TK[1] - 96, TK_RX=TK[2], TK_RY=TK[3], TK_DIMPLE_PCT=48)

# Band keys: one per configured band, sharing the width of a recessed
# channel. The program builds each key from a left cap, repeated middle tiles
# and a right cap, so that any number of bands (up to KEY_COUNT) fills it.
KEYS_X, KEYS_Y, KEYS_W, KEYS_H = 24, 155, 350, 23
KEY_COUNT = 10
KEY_FACE_H = 14
KEY_CAP, KEY_TILE, KEY_GAP = 6, 32, 2
L.update(KEYS_X=KEYS_X, KEYS_Y=KEYS_Y, KEYS_W=KEYS_W, KEYS_H=KEYS_H,
         KEY_COUNT=KEY_COUNT, KEY_CAP=KEY_CAP, KEY_TILE=KEY_TILE, KEY_GAP=KEY_GAP,
         KEY_LABEL_Y_UP=5, KEY_LABEL_Y_DOWN=8)


def key_span(i, n):
    """Left edge and width of key i of n, as the C code computes them."""
    x0 = i * KEYS_W // n
    x1 = (i + 1) * KEYS_W // n - KEY_GAP
    return KEYS_X + x0, x1 - x0

BTN_X, BTN_Y, BTN_PITCH, BTN_W, BTN_H = 24, 182, 44, 42, 13
BUTTONS = ['< SEEK', 'SEEK >', 'SCAN', 'SWEEP', 'REGION', 'HELP', 'TO WB', 'POWER']
L.update(BTN_X=BTN_X, BTN_Y=BTN_Y, BTN_PITCH=BTN_PITCH, BTN_W=BTN_W, BTN_H=BTN_H,
         BTN_COUNT=len(BUTTONS))

KNOB_X, KNOB_Y, KNOB_W, KNOB_H = 386, 154, 78, 42
KNOB_CAPTIONS = ['VOL', 'SQL', 'GAIN']
L.update(KNOB_X=KNOB_X, KNOB_Y=KNOB_Y, KNOB_W=KNOB_W, KNOB_H=KNOB_H,
         KNOB_CX=39, KNOB_CY=15, KNOB_RX=21, KNOB_RY=11, KNOB_VALUE_Y=33,
         KNOB_VALUE_X1=72)

# PAL-only speaker section
GRILLE = (16, 205, 623, 249)
PLATE = (264, 218, 375, 236)

SCRATCH_W, SCRATCH_H = 352, 64
L.update(SCRATCH_W=SCRATCH_W, SCRATCH_H=SCRATCH_H)

L.update(C_BLACK=BLACK, C_INK=INK, C_GLOW=GLOW, C_CREAM=CREAM, C_WOOD_L=WOOD_L,
         C_GUN_D=GUN_D, C_GUN=GUN, C_STEEL=STEEL, C_STEEL_L=STEEL_L, C_HILITE=HILITE,
         C_RED=RED, C_VFD=VFD, C_VFD_DIM=VFD_DIM, C_AMBER=AMBER, C_WOOD=WOOD)


# ---------------------------------------------------------------------------
# Drawing

def draw_wood(c):
    for y in range(H):
        for x in range(W):
            v = y + 7 * math.sin(x / 113.0 + 0.4 * math.sin(y / 37.0)) + 5 * noise(x / 70.0, y / 9.0, 1)
            g = 0.5 + 0.5 * math.sin(v * 0.62 + 2.2 * noise(x / 45.0, y / 5.0, 2))
            fine = noise(x / 2.5, y / 1.3, 3)
            t = 0.30 + 0.30 * g + 0.22 * (fine - 0.5)
            # gentle vertical sheen of the varnish
            t += 0.10 * math.exp(-((y - 60) / 70.0) ** 2)
            c.set(x, y, shade(R_WOOD, t, x, y))
    # cabinet edge
    for i, col in enumerate([BLACK, INK, WOOD_L]):
        c.hline(i, W - 1 - i, i, col if i < 2 else WOOD_L)
        c.vline(i, i, H - 1 - i, col if i < 2 else WOOD_L)
        c.hline(i, W - 1 - i, H - 1 - i, col if i < 2 else INK)
        c.vline(W - 1 - i, i, H - 1 - i, col if i < 2 else INK)
    # groove between the radio (all an NTSC screen shows) and the speaker
    c.hline(2, W - 3, NTSC_H - 2, INK)
    c.hline(2, W - 3, NTSC_H - 1, BLACK)
    c.hline(2, W - 3, NTSC_H, WOOD_L)


def drop_shadow(c, inside, x0, y0, x1, y1, dx=3, dy=2, strength=0.6):
    for y in range(y0, y1 + dy + 1):
        for x in range(x0, x1 + dx + 1):
            if inside(x - dx, y - dy) and not inside(x, y):
                if bayer(x, y) < strength:
                    c.set(x, y, BLACK if c.get(x, y) in (INK, WOOD_D, GUN_D, BLACK) else INK)


def draw_speaker(c):
    """PAL-only decoration: grille cloth behind chrome slats."""
    gx0, gy0, gx1, gy1 = GRILLE
    inside = lambda x, y: rounded(x, y, gx0, gy0, gx1, gy1, 10)
    drop_shadow(c, inside, gx0, gy0, gx1, gy1, 4, 2, 0.7)
    for y in range(gy0, gy1 + 1):
        for x in range(gx0, gx1 + 1):
            if not inside(x, y):
                continue
            ring = not (inside(x - 5, y) and inside(x + 5, y) and inside(x, y - 3) and inside(x, y + 3))
            if ring:
                light = 1.0 if y < (gy0 + gy1) / 2 else 0.3
                t = 0.35 + 0.45 * light + 0.06 * (noise(x / 3.0, y, 8) - 0.5)
                c.set(x, y, shade(R_METAL, t, x, y))
                continue
            # woven cloth: alternating warp and weft, darker towards the edges
            weave = ((x >> 1) + y) & 1
            v = 0.40 + 0.22 * weave + 0.08 * (noise(x / 5.0, y / 2.0, 9) - 0.5)
            v -= 0.25 * abs((x - (gx0 + gx1) / 2.0) / ((gx1 - gx0) / 2.0)) ** 3
            v -= 0.15 * abs((y - (gy0 + gy1) / 2.0) / ((gy1 - gy0) / 2.0)) ** 2
            c.set(x, y, shade([WOOD_D, WOOD, WOOD_L, GLOW], v, x, y))
    for y in range(gy0, gy1 + 1):
        for x in range(gx0, gx1 + 1):
            if inside(x, y) and not inside(x + 1, y + 1):
                c.set(x, y, BLACK)
    # chrome slats across the cloth
    for sy in (gy0 + 10, gy0 + 22, gy0 + 34):
        for x in range(gx0 + 8, gx1 - 7):
            if PLATE[0] - 6 <= x <= PLATE[2] + 6:
                continue
            c.set(x, sy - 1, STEEL_L)
            c.set(x, sy, shade(R_METAL, 0.5, x, sy))
            c.set(x, sy + 1, BLACK)
    draw_plate(c)


def draw_plate(c):
    x0, y0, x1, y1 = PLATE
    inside = lambda x, y: rounded(x, y, x0, y0, x1, y1, 6)
    drop_shadow(c, inside, x0, y0, x1, y1)
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if not inside(x, y):
                continue
            edge = not (inside(x - 2, y) and inside(x + 2, y) and inside(x, y - 1) and inside(x, y + 1))
            if edge:
                t = 0.85 if y < (y0 + y1) / 2 else 0.35
                c.set(x, y, shade(R_METAL, t, x, y))
            else:
                t = 0.10 + 0.12 * (1 - (y - y0) / (y1 - y0))
                c.set(x, y, shade([BLACK, GUN_D, GUN], t, x, y))
    s = 'PIRADIO'
    tw = text_width(s, 2)
    tx = (x0 + x1 + 1 - tw) // 2
    ty = y0 + 2
    m = c.text_mask(tx, ty, s, 2)
    for (px, py) in m:
        if (px + 1, py + 1) not in m:
            c.set(px + 1, py + 1, BLACK)
    for (px, py) in m:
        u = (py - ty) / 13.0
        # chrome lettering: bright sky above the horizon, dark ground below
        t = 0.98 - 0.5 * u if u < 0.5 else 0.30 + 0.6 * (u - 0.5)
        c.set(px, py, shade(R_METAL, t, px, py))


def draw_bezel(c):
    bx0, by0, bx1, by1 = BEZEL
    dx0, dy0, dx1, dy1 = DIAL
    outer = lambda x, y: rounded(x, y, bx0, by0, bx1, by1, 10)
    inner = lambda x, y: rounded(x, y, dx0 - 1, dy0 - 1, dx1 + 1, dy1 + 1, 4)
    drop_shadow(c, outer, bx0, by0, bx1, by1, 4, 2, 0.7)
    for y in range(by0, by1 + 1):
        for x in range(bx0, bx1 + 1):
            if not outer(x, y) or inner(x, y):
                continue
            # position across the ring, 0 at the outer edge, 1 at the inner
            if y < dy0:
                u = (y - by0) / float(dy0 - by0)
                light = 1.0
            elif y > dy1:
                u = (by1 - y) / float(by1 - dy1)
                light = 0.25
            elif x < dx0:
                u = (x - bx0) / float(dx0 - bx0)
                light = 0.75
            else:
                u = (bx1 - x) / float(bx1 - dx1)
                light = 0.45
            t = 0.25 + 0.55 * math.sin(math.pi * min(1, u)) * (0.5 + 0.5 * light) + 0.15 * light
            t += 0.08 * (noise(x / 3.0, y, 4) - 0.5)
            c.set(x, y, shade(R_METAL, t, x, y))
    for y in range(by0, by1 + 1):
        for x in range(bx0, bx1 + 1):
            if outer(x, y) and not outer(x + 1, y + 1) and not inner(x, y):
                c.set(x, y, BLACK)
            if inner(x, y) and not inner(x, y - 1):
                pass
    # dark seal where the glass meets the bezel
    for y in range(dy0 - 1, dy1 + 2):
        for x in range(dx0 - 1, dx1 + 2):
            if inner(x, y) and not (dx0 <= x <= dx1 and dy0 <= y <= dy1):
                c.set(x, y, BLACK)


def draw_dial_face(c):
    x0, y0, x1, y1 = DIAL
    cx, cy = (x0 + x1) / 2.0, y0 + 32
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            dx = (x - cx) / 330.0
            dy = (y - cy) / 85.0
            d = math.hypot(dx, dy)
            t = 0.74 - 0.55 * d ** 1.6
            # shadow cast by the bezel onto the face
            t -= 0.35 * max(0, (5 - (y - y0)) / 5.0)
            t -= 0.25 * max(0, (8 - (x - x0)) / 8.0)
            t -= 0.25 * max(0, (8 - (x1 - x)) / 8.0)
            t += 0.03 * (noise(x / 9.0, y / 5.0, 5) - 0.5)
            # reflection on the glass
            u = (x - x0) - 3.2 * (y - y0)
            if 30 < u < 150:
                t += 0.09 * math.sin(math.pi * (u - 30) / 120.0)
            c.set(x, y, shade(R_GLOW, t, x, y))
    # message window, a strip of dark smoked glass
    mx0, mx1 = x0 + L['MSG_X0'], x0 + L['MSG_X1']
    my0, my1 = y0 + L['MSG_Y0'], y0 + L['MSG_Y1']
    for y in range(my0, my1 + 1):
        for x in range(mx0, mx1 + 1):
            t = 0.28 - 0.25 * (y - my0) / (my1 - my0)
            u = (x - mx0) - 3.2 * (y - my0)
            if 60 < u < 160:
                t += 0.12
            c.set(x, y, shade([BLACK, GUN_D, GUN], t, x, y))
    c.hline(mx0 - 1, mx1 + 1, my0 - 1, WOOD)
    c.vline(mx0 - 1, my0 - 1, my1 + 1, WOOD)
    c.hline(mx0 - 1, mx1 + 1, my1 + 1, CREAM)
    c.vline(mx1 + 1, my0, my1 + 1, CREAM)


def bevel_box(c, x0, y0, x1, y1, ring=3, light=0.7):
    """A chrome frame set into the panel."""
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            d = min(x - x0, x1 - x, (y - y0) * 2, (y1 - y) * 2)
            if d >= ring * 2 and min(x - x0, x1 - x) >= ring:
                continue
            top_left = (y - y0) * 2 < (x1 - x) and (x - x0) < (y1 - y) * 2
            t = (0.85 if top_left else 0.30)
            if x == x0 or y == y0:
                t = 0.10
            if x == x1 or y == y1:
                t = 0.95
            c.set(x, y, shade(R_METAL, t * light + 0.1, x, y))


def draw_panel(c):
    x0, y0, x1, y1 = PANEL
    for y in range(y0, y1 + 1):
        streak = noise(3.0, y * 1.7, 6)
        for x in range(x0, x1 + 1):
            s = 0.6 * streak + 0.4 * noise(x / 190.0, y * 1.7, 7)
            t = 0.52 + 0.30 * (s - 0.5) - 0.20 * (y - y0) / (y1 - y0)
            c.set(x, y, shade(R_PANEL, t, x, y))
    c.hline(x0, x1, y0, STEEL_L)
    c.hline(x0, x1, y0 + 1, STEEL)
    c.vline(x0, y0, y1, STEEL)
    c.hline(x0, x1, y1, BLACK)
    c.vline(x1, y0, y1, BLACK)
    c.hline(x0 + 1, x1 - 1, y0 - 1, BLACK)
    # engraved separator between the rows
    c.hline(x0 + 8, x1 - 8, SEPARATOR_Y, BLACK)
    c.hline(x0 + 8, x1 - 8, SEPARATOR_Y + 1, STEEL)
    for sx, sy in ((14, 99), (W - 15, 99), (14, 191), (W - 15, 191)):
        screw(c, sx, sy)


# A slotted screw head, drawn by hand like the lamps: H highlight, L light
# steel, M steel, D dark steel, S the slot, K shadow.
SCREW_HEAD = [
    '..LLLLL..',
    '.LHHLLLM.',
    'LSSSSSSSD',
    '.LMMMMMD.',
    '..DDDDDK.',
    '...KKKK..',
]


def screw(c, cx, cy):
    pens = {'H': HILITE, 'L': STEEL_L, 'M': STEEL, 'D': GUN, 'S': BLACK, 'K': BLACK}
    for j, row in enumerate(SCREW_HEAD):
        for i, ch in enumerate(row):
            if ch != '.':
                c.set(cx - 4 + i, cy - 2 + j, pens[ch])


def draw_vfd(c):
    x0, y0, x1, y1 = VFD_BOX
    bevel_box(c, x0, y0, x1, y1, 4)
    gx0, gy0, gx1, gy1 = x0 + 4, y0 + 3, x1 - 4, y1 - 3
    for y in range(gy0, gy1 + 1):
        for x in range(gx0, gx1 + 1):
            t = 0.0
            u = (x - gx0) - 3.2 * (y - gy0)
            if 0 < u < 90 and y - gy0 < 12:
                t = 0.25 * (1 - (y - gy0) / 12.0)
            c.set(x, y, shade([BLACK, GUN_D], t, x, y))
    c.text(x0 + 240, y0 + L['DIG_Y'] + L['DIG_H'] - 7, 'MHZ', VFD_DIM)
    c.text(x0 + 240, y0 + L['DIG_Y'] + L['DIG_H'] - 7, 'MHZ', VFD)


METER_Y_LEVEL = 126


def meter_pt(deg, pct):
    a = math.radians(deg)
    return (METER_CX + math.cos(a) * METER_RX * pct,
            METER_CY - math.sin(a) * METER_RY * pct)


def draw_line(c, x0, y0, x1, y1, col):
    n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
    for i in range(n + 1):
        t = i / float(n)
        c.set(int(round(x0 + (x1 - x0) * t)), int(round(y0 + (y1 - y0) * t)), col)


def draw_meter(c):
    x0, y0, x1, y1 = METER
    bevel_box(c, x0, y0, x1, y1, 4)
    fx0, fy0, fx1, fy1 = x0 + 4, y0 + 3, x1 - 4, y1 - 3
    for y in range(fy0, fy1 + 1):
        for x in range(fx0, fx1 + 1):
            t = 0.72 - 0.25 * ((x - (fx0 + fx1) / 2.0) / 80.0) ** 2
            t -= 0.35 * max(0, (5 - (y - fy0)) / 5.0)
            c.set(x, y, shade(R_GLOW, t, x, y))
    # scale arc
    steps = 160
    for i in range(steps + 1):
        deg = METER_DEG0 + (METER_DEG1 - METER_DEG0) * i / steps
        x, y = meter_pt(deg, 1.0)
        col = RED if i > steps * 0.78 else INK
        c.set(int(round(x)), int(round(y)), col)
        c.set(int(round(x)) + 1, int(round(y)), col)
    for i in range(11):
        deg = METER_DEG0 + (METER_DEG1 - METER_DEG0) * i / 10.0
        col = RED if i >= 8 else INK
        pct = 0.86 if i % 2 == 0 else 0.92
        xa, ya = meter_pt(deg, 1.0)
        xb, yb = meter_pt(deg, pct)
        draw_line(c, xa, ya, xb, yb, col)
        draw_line(c, xa + 1, ya, xb + 1, yb, col)
    for i, s in ((0, '0'), (2, '2'), (4, '4'), (6, '6'), (8, '8'), (10, '+')):
        deg = METER_DEG0 + (METER_DEG1 - METER_DEG0) * i / 10.0
        x, y = meter_pt(deg, 0.80)
        c.text(int(x - text_width(s) / 2), int(y - 3), s, RED if i >= 8 else INK)
    c.text(METER_CX - text_width('LEVEL') // 2, METER_Y_LEVEL, 'LEVEL', INK)
    # needle pivot housing
    hp = L['METER_PCT0'] / 100.0 - 0.03
    for x, y, nx, ny, r in ellipse_iter(METER_CX, METER_CY, METER_RX * hp, METER_RY * hp):
        if fy0 <= y <= fy1 and fx0 <= x <= fx1:
            t = 0.25 + 0.4 * max(0, -ny) * (1 - r) + (0.35 if r > 0.95 and ny < 0 else 0)
            c.set(x, y, shade(R_METAL, t, x, y))
    # glass sheen
    for y in range(fy0, fy1 + 1):
        for x in range(fx0, fx1 + 1):
            u = (x - fx0) - 3.2 * (y - fy0)
            if 20 < u < 50 and c.get(x, y) in (GLOW, CREAM, WOOD_L):
                if bayer(x, y) < 0.35:
                    c.set(x, y, HILITE if c.get(x, y) == CREAM else CREAM)


# A lamp lens, drawn by hand: B bezel, F face, R shaded rim, H highlight.
# Hires pixels are almost twice as tall as they are wide, so 10 x 6 pixels
# looks round.
LED_LENS = [
    '..BBBBBB..',
    '.BFHHFFFB.',
    'BFHFFFFFRB',
    'BFFFFFFRRB',
    '.BFFFRRRB.',
    '..BBBBBB..',
]


def led(c, cx, cy, state):
    """A round lamp lens, without dithering: at this size, mixed pixels
    look like noise rather than shading."""
    colours = {
        # face, rim, highlight
        'off': (GUN_D, BLACK, GUN),
        'green': (VFD, VFD_DIM, HILITE),
        'red': (RED, INK, AMBER),
        'amber': (AMBER, WOOD_L, CREAM),
    }
    face, rim, hilite = colours[state]
    pens = {'B': BLACK, 'F': face, 'R': rim, 'H': hilite}
    for j, row in enumerate(LED_LENS):
        for i, ch in enumerate(row):
            if ch != '.':
                c.set(cx - 5 + i, cy - 3 + j, pens[ch])


def knob(c, cx, cy, rx, ry, cap, knurls, shadow=True):
    if shadow:
        for x, y, nx, ny, r in ellipse_iter(cx + 4, cy + 3, rx, ry):
            if bayer(x, y) < 0.75 - 0.5 * r:
                c.set(x, y, BLACK)
    lx, ly, lz = -0.45, 0.60, 0.66
    for x, y, nx, ny, r in ellipse_iter(cx, cy, rx, ry):
        ang = math.atan2(-ny, nx)
        if r > cap:
            # knurled skirt: lit from the top left
            u = (r - cap) / (1 - cap)
            light = 0.5 + 0.42 * math.cos(ang - math.radians(125))
            ridge = 0.5 + 0.5 * math.sin(ang * knurls)
            t = light * (0.72 + 0.32 * ridge)
            if u > 0.85:
                t *= 0.55
            elif u < 0.15:
                t = t * 0.7 + 0.25
            c.set(x, y, shade(R_METAL, t, x, y))
        else:
            # bakelite dome cap with a specular highlight
            u = r / cap
            px, py = nx / cap, -ny / cap
            pz = math.sqrt(max(0.0, 1 - min(1.0, u * u) * 0.8))
            n = math.sqrt(px * px + py * py + pz * pz)
            diff = max(0.0, (px * lx + py * ly + pz * lz) / n)
            t = 0.04 + 0.30 * diff ** 2 + 0.75 * diff ** 18
            if u > 0.93:
                t = 0.02
            c.set(x, y, shade(R_METAL, t, x, y))


def knob_scale(c, cx, cy, rx, ry):
    for i in range(9):
        deg = 225 - i * 270 / 8.0
        a = math.radians(deg)
        long_tick = i in (0, 8)
        for pct in ((1.22, 1.40) if long_tick else (1.26, 1.36)):
            x = cx + math.cos(a) * rx * pct
            y = cy - math.sin(a) * ry * pct
            c.set(int(round(x)), int(round(y)), STEEL_L)
            c.set(int(round(x)) + 1, int(round(y)), STEEL_L)


def draw_key_bed(c):
    """The recessed channel the band keys sit in."""
    x0, y0 = KEYS_X - 2, KEYS_Y - 1
    x1, y1 = KEYS_X + KEYS_W + 1, KEYS_Y + KEYS_H
    c.rect(x0, y0, x1, y1, BLACK)
    c.hline(x0, x1, y1 + 1, STEEL)
    c.vline(x1 + 1, y0, y1 + 1, STEEL)


def render_key(c, x0, w, down):
    """An ivory push button of any width; shading is confined to the caps
    so that the middle can be tiled."""
    y0 = KEYS_Y
    top = y0 + (4 if down else 1)
    face_bot = top + KEY_FACE_H
    lip_bot = y0 + KEYS_H - 2
    x1 = x0 + w - 1
    inside = lambda x, y: rounded(x, y, x0, top, x1, face_bot, 3)
    for y in range(top, face_bot + 1):
        for x in range(x0, x1 + 1):
            if not inside(x, y):
                continue
            u = (y - top) / float(KEY_FACE_H)
            edge = max(0.0, 1.0 - min(x - x0, x1 - x) / float(KEY_CAP))
            t = 0.78 - 0.35 * u - 0.30 * edge ** 2
            if down:
                t -= 0.18
            if y == top:
                t = 0.98 if not down else 0.6
            c.set(x, y, shade(R_IVORY, t, x, y))
    for y in range(face_bot + 1, lip_bot + 1):
        for x in range(x0 + 1, x1):
            t = 0.35 - 0.25 * (y - face_bot) / max(1, lip_bot - face_bot)
            c.set(x, y, shade([INK, WOOD_D, WOOD, WOOD_L], t, x, y))


def draw_button(c, i, down):
    x0 = BTN_X + i * BTN_PITCH
    y0 = BTN_Y
    x1, y1 = x0 + BTN_W - 1, y0 + BTN_H - 1
    c.rect(x0 - 1, y0 - 1, x1 + 1, y1 + 1, BLACK)
    inside = lambda x, y: rounded(x, y, x0, y0, x1, y1, 4)
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if not inside(x, y):
                continue
            u = (y - y0) / float(BTN_H - 1)
            t = (0.80 - 0.45 * u) if not down else (0.30 + 0.25 * u)
            if not inside(x, y - 1):
                t = 0.95 if not down else 0.15
            if not inside(x, y + 1):
                t = 0.15 if not down else 0.6
            c.set(x, y, shade(R_METAL, t, x, y))
    s = BUTTONS[i]
    tx = x0 + (BTN_W - text_width(s)) // 2 + (1 if down else 0)
    ty = y0 + 3 + (1 if down else 0)
    col = RED if s == 'POWER' else BLACK
    m = c.text_mask(tx, ty, s)
    for (px, py) in m:
        if (px, py + 1) not in m:
            c.set(px, py + 1, STEEL_L if not down else GUN)
    c.text(tx, ty, s, col)


def draw_controls(c):
    draw_vfd(c)
    draw_meter(c)
    for y, label in zip(LED_YS, LED_LABELS):
        led(c, LED_X + 7, y + 5, 'off')
        c.engrave(LED_X + 16, y + 2, label)
    knob(c, TK[0], TK[1], TK[2], TK[3], 0.70, 30)
    c.engrave(TK[0] - text_width('TUNING') // 2, 140, 'TUNING')
    draw_key_bed(c)
    for i in range(len(BUTTONS)):
        draw_button(c, i, False)
    for i, cap in enumerate(KNOB_CAPTIONS):
        cx = KNOB_X + i * KNOB_W + L['KNOB_CX']
        cy = KNOB_Y + L['KNOB_CY']
        knob_scale(c, cx, cy, L['KNOB_RX'], L['KNOB_RY'])
        knob(c, cx, cy, L['KNOB_RX'], L['KNOB_RY'], 0.66, 18)
        c.engrave(KNOB_X + i * KNOB_W + 6, KNOB_Y + L['KNOB_VALUE_Y'], cap)


def render_background():
    c = Canvas(W, H)
    draw_wood(c)
    draw_speaker(c)
    draw_bezel(c)
    draw_dial_face(c)
    draw_panel(c)
    draw_controls(c)
    return c


# ---------------------------------------------------------------------------
# Alternate-state images

def render_sprites(bg):
    sprites = []   # (name, x, y, w, h, canvas)

    # Key slices: left cap, middle tile, right cap, for each state. The key
    # is drawn at x=2 so that the tile starts on a multiple of 4, keeping
    # the dither pattern continuous when tiles are repeated.
    for state in ('UP', 'DOWN'):
        c = Canvas(W, H)
        render_key(c, 2, 2 * KEY_CAP + KEY_TILE, state == 'DOWN')
        sprites.append(('KEY_%s_L' % state, 2, KEYS_Y, KEY_CAP, KEYS_H, c))
        sprites.append(('KEY_%s_M' % state, 2 + KEY_CAP, KEYS_Y, KEY_TILE, KEYS_H, c))
        sprites.append(('KEY_%s_R' % state, 2 + KEY_CAP + KEY_TILE, KEYS_Y, KEY_CAP, KEYS_H, c))
    for i in range(len(BUTTONS)):
        c = bg.copy()
        draw_button(c, i, True)
        sprites.append(('BTN%d' % i, BTN_X + i * BTN_PITCH - 1, BTN_Y - 1, BTN_W + 2, BTN_H + 2, c))
    for i, y in enumerate(LED_YS):
        for state in ('green', 'red', 'amber'):
            c = bg.copy()
            led(c, LED_X + 7, y + 5, state)
            sprites.append(('LED%d_%s' % (i, state.upper()), LED_X - 1, y, 16, 11, c))

    # Pack into rows of an atlas 640 pixels wide
    atlas_rows = []
    placed = []
    x = y = row_h = 0
    for name, sx, sy, w, h, c in sprites:
        if x + w > W:
            y += row_h
            x = row_h = 0
        placed.append((name, x, y, sx, sy, w, h, c))
        x += w + 1
        row_h = max(row_h, h)
    ah = y + row_h
    atlas = Canvas(W, ah)
    for name, ax, ay, sx, sy, w, h, c in placed:
        for yy in range(h):
            for xx in range(w):
                atlas.set(ax + xx, ay + yy, c.get(sx + xx, sy + yy))
    return atlas, [(n, ax, ay, sx, sy, w, h) for n, ax, ay, sx, sy, w, h, c in placed]


# ---------------------------------------------------------------------------
# Output

def to_planes_byterun1(c):
    bpr = (c.w + 15) // 16 * 2
    out = bytearray()
    for y in range(c.h):
        row = c.px[y]
        for p in range(DEPTH):
            bits = bytearray(bpr)
            for x in range(c.w):
                if (row[x] >> p) & 1:
                    bits[x >> 3] |= 0x80 >> (x & 7)
            out += byterun1(bits)
    return bytes(out)


def byterun1(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        j = i
        while j + 1 < n and data[j + 1] == data[i] and j - i < 127:
            j += 1
        if j - i >= 2:
            out.append((257 - (j - i + 1)) & 0xff)
            out.append(data[i])
            i = j + 1
        else:
            k = i
            while k < n and k - i < 128:
                if k + 2 < n and data[k] == data[k + 1] == data[k + 2]:
                    break
                k += 1
            out.append(k - i - 1)
            out += data[i:k]
            i = k
    return bytes(out)


def build_font():
    chars = [chr(i) for i in range(32, 127)]
    defined = [ch for ch in chars if ch in GLYPHS]
    offsets = {}
    x = 0
    for ch in defined:
        offsets[ch] = x
        x += len(GLYPHS[ch][0])
    modulo = (x + 15) // 16 * 2
    rows = [bytearray(modulo) for _ in range(FONT_H)]
    for ch in defined:
        for gy, row in enumerate(GLYPHS[ch]):
            for gx, p in enumerate(row):
                if p == '#':
                    bx = offsets[ch] + gx
                    rows[gy][bx >> 3] |= 0x80 >> (bx & 7)
    loc, space = [], []
    for ch in chars + ['?']:   # one extra entry for undefined characters
        key = ch.upper() if 'a' <= ch <= 'z' else ch
        if key not in GLYPHS:
            key = '?'
        w = len(GLYPHS[key][0])
        loc.append((offsets[key] << 16) | w)
        space.append(w + FONT_SPACING)
    space[0] = 4   # space character
    return b''.join(bytes(r) for r in rows), modulo, loc, space


def c_bytes(name, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append('\t' + ','.join(str(b) for b in data[i:i + 24]) + ',')
    return 'const UBYTE %s[%d] =\n{\n%s\n};\n' % (name, len(data), '\n'.join(lines))


def write_sources(bg, atlas, sprites, here):
    bg_data = to_planes_byterun1(bg)
    atlas_data = to_planes_byterun1(atlas)
    font_data, font_mod, font_loc, font_space = build_font()

    h = []
    h.append('/* Generated by make_art.py - do not edit. */')
    h.append('#ifndef PIRADIO_ART_H')
    h.append('#define PIRADIO_ART_H')
    h.append('')
    h.append('#include <exec/types.h>')
    h.append('')
    h.append('#define ART_W\t\t%d' % W)
    h.append('#define ART_H\t\t%d' % H)
    h.append('#define ART_DEPTH\t%d' % DEPTH)
    h.append('#define ATLAS_H\t\t%d' % atlas.h)
    h.append('')
    for k in sorted(L):
        h.append('#define %s\t%d' % (k, L[k]))
    h.append('')
    h.append('#define LED_COUNT\t%d' % len(LED_YS))
    h.append('#define FONT_H\t\t%d' % FONT_H)
    h.append('#define FONT_BASELINE\t%d' % FONT_BASELINE)
    h.append('#define FONT_MODULO\t%d' % font_mod)
    h.append('#define FONT_LO\t\t32')
    h.append('#define FONT_HI\t\t126')
    h.append('')
    h.append('struct ArtSprite { WORD ax, ay, x, y, w, h; };')
    h.append('')
    for i, s in enumerate(sprites):
        h.append('#define SPR_%s\t%d' % (s[0], i))
    h.append('#define SPR_COUNT\t%d' % len(sprites))
    h.append('')
    h.append('extern const UWORD art_palette[16];')
    h.append('extern const UBYTE art_background[%d];' % len(bg_data))
    h.append('extern const UBYTE art_atlas[%d];' % len(atlas_data))
    h.append('extern const struct ArtSprite art_sprites[SPR_COUNT];')
    h.append('extern const UBYTE art_font_data[%d];' % len(font_data))
    h.append('extern const ULONG art_font_loc[%d];' % len(font_loc))
    h.append('extern const WORD art_font_space[%d];' % len(font_space))
    h.append('extern const WORD art_digit_x[8];')
    h.append('extern const WORD art_led_y[%d];' % len(LED_YS))
    h.append('')
    h.append('#endif')

    c = []
    c.append('/* Generated by make_art.py - do not edit. */')
    c.append('#include "piradio_art.h"')
    c.append('')
    c.append('const UWORD art_palette[16] = { %s };' % ', '.join('0x%03X' % p for p in PALETTE))
    c.append('')
    c.append('const struct ArtSprite art_sprites[SPR_COUNT] =\n{')
    for name, ax, ay, sx, sy, w, hh in sprites:
        c.append('\t{ %d, %d, %d, %d, %d, %d },\t/* %s */' % (ax, ay, sx, sy, w, hh, name))
    c.append('};')
    c.append('')
    c.append('const WORD art_digit_x[8] = { %s };' % ', '.join(str(x) for x in DIGIT_X))
    c.append('const WORD art_led_y[%d] = { %s };' % (len(LED_YS), ', '.join(str(y) for y in LED_YS)))
    c.append('const ULONG art_font_loc[%d] = { %s };' % (len(font_loc), ', '.join('0x%08X' % v for v in font_loc)))
    c.append('const WORD art_font_space[%d] = { %s };' % (len(font_space), ', '.join(str(v) for v in font_space)))
    c.append('')
    c.append(c_bytes('art_font_data', font_data))
    c.append(c_bytes('art_atlas', atlas_data))
    c.append(c_bytes('art_background', bg_data))

    with open(os.path.join(here, 'piradio_art.h'), 'w', newline='\n') as f:
        f.write('\n'.join(h) + '\n')
    with open(os.path.join(here, 'piradio_art.c'), 'w', newline='\n') as f:
        f.write('\n'.join(c) + '\n')
    return len(bg_data), len(atlas_data)


# ---------------------------------------------------------------------------
# Preview: the background with a mock-up of the dynamic parts

def rgb(c12):
    return (((c12 >> 8) & 15) * 17, ((c12 >> 4) & 15) * 17, (c12 & 15) * 17)


def write_png(path, canvas, sx=2, sy=4):
    raw = bytearray()
    pal = [bytes(rgb(p)) for p in PALETTE]
    for y in range(canvas.h * sy):
        raw.append(0)
        row = canvas.px[y // sy]
        for x in range(canvas.w * sx):
            raw += pal[row[x // sx]]
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', canvas.w * sx, canvas.h * sy, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def mockup(bg, sprites):
    c = bg.copy()
    dx, dy = DIAL[0], DIAL[1]
    # scale for 87.5-108 MHz
    lo, hi = 87.5, 108.0
    def fx(f):
        return dx + L['SC_X0'] + int((f - lo) / (hi - lo) * L['SC_W'])
    c.hline(dx + L['SC_X0'], dx + L['SC_X0'] + L['SC_W'], dy + L['BASE_Y'], INK)
    f = 88.0
    while f <= hi + 1e-6:
        x = fx(f)
        if abs(f - round(f / 2) * 2) < 1e-6:
            c.rect(x, dy + L['TICK_Y0'], x + 1, dy + L['BASE_Y'], INK)
            s = '%d' % f
            c.text(x - text_width(s) // 2, dy + L['LABEL_Y'], s, INK)
        else:
            c.vline(x, dy + L['MINOR_Y0'], dy + L['BASE_Y'], INK)
        f += 0.4
    for f in (89.1, 93.5, 96.3, 101.1, 104.9):
        x = fx(f)
        for k in range(4):
            c.hline(x - k, x + k, dy + L['MARK_Y'] + k, VFD)
    s = 'FM BROADCAST'
    c.text(dx + (DIAL_W - text_width(s)) // 2, dy + L['TITLE_Y'], s, INK)
    c.text(dx + 10, dy + L['TITLE_Y'], 'WBFM', INK)
    s = 'STEP 100 KHZ'
    c.text(dx + DIAL_W - 10 - text_width(s), dy + L['TITLE_Y'], s, INK)
    x = fx(96.3)
    c.rect(x, dy + L['NEEDLE_Y0'], x + 1, dy + L['NEEDLE_Y1'], RED)
    c.vline(x + 2, dy + L['NEEDLE_Y0'] + 2, dy + L['NEEDLE_Y1'], WOOD_L)
    s = 'SCANNING UP'
    c.text(dx + (DIAL_W - text_width(s)) // 2, dy + L['MSG_TEXT_Y'], s, AMBER)
    # digits " 96.3000"
    segs = {'9': 0x6F, '6': 0x7D, '3': 0x4F, '0': 0x3F}
    for i, ch in enumerate('  963000'):
        m = segs.get(ch, 0)
        x0 = VFD_BOX[0] + DIGIT_X[i]
        y0 = VFD_BOX[1] + L['DIG_Y']
        dw, dh, t = L['DIG_W'], L['DIG_H'], L['DIG_T']
        h2 = dh // 2
        rects = [(x0 + 2, y0, x0 + dw - 3, y0 + t - 1), (x0 + dw - t, y0 + 2, x0 + dw - 1, y0 + h2 - 2),
                 (x0 + dw - t, y0 + h2 + 2, x0 + dw - 1, y0 + dh - 3), (x0 + 2, y0 + dh - t, x0 + dw - 3, y0 + dh - 1),
                 (x0, y0 + h2 + 2, x0 + t - 1, y0 + dh - 3), (x0, y0 + 2, x0 + t - 1, y0 + h2 - 2),
                 (x0 + 2, y0 + h2 - t // 2, x0 + dw - 3, y0 + h2 + t // 2 - 1)]
        for b, r in enumerate(rects):
            c.rect(r[0], r[1], r[2], r[3], VFD if (m >> b) & 1 else VFD_DIM)
    c.rect(VFD_BOX[0] + L['DOT_X'], VFD_BOX[1] + L['DIG_Y'] + L['DIG_H'] - 4, VFD_BOX[0] + L['DOT_X'] + 3, VFD_BOX[1] + L['DIG_Y'] + L['DIG_H'] - 1, VFD)
    # meter needle
    a = math.radians(METER_DEG0 + (METER_DEG1 - METER_DEG0) * 0.55)
    p0 = (METER_CX + math.cos(a) * METER_RX * 0.55, METER_CY - math.sin(a) * METER_RY * 0.55)
    p1 = (METER_CX + math.cos(a) * METER_RX * 0.94, METER_CY - math.sin(a) * METER_RY * 0.94)
    draw_line(c, p0[0], p0[1], p1[0], p1[1], BLACK)
    draw_line(c, p0[0] + 1, p0[1], p1[0] + 1, p1[1], BLACK)
    # lit lamps, pressed key and button
    by_name = {s[0]: s for s in sprites}
    for nm in ('LED0_GREEN', 'LED1_GREEN', 'LED2_GREEN', 'LED3_AMBER'):
        n, ax, ay, sx, sy, w, h = by_name[nm]
        for yy in range(h):
            for xx in range(w):
                c.set(sx + xx, sy + yy, ATLAS.get(ax + xx, ay + yy))
    labels = ['FM', 'AIR', 'MAR', 'WX', '2M', '70CM', 'PMR', 'CB']
    for i, s in enumerate(labels):
        x0, w = key_span(i, len(labels))
        render_key(c, x0, w, i == 0)
        y = KEYS_Y + (L['KEY_LABEL_Y_DOWN'] if i == 0 else L['KEY_LABEL_Y_UP'])
        c.text(x0 + (w - text_width(s)) // 2, y, s, RED if i == 0 else INK)
    for i, v in enumerate(('100%', 'OFF', 'AUTO')):
        cx = KNOB_X + i * KNOB_W
        c.text(cx + L['KNOB_VALUE_X1'] - text_width(v), KNOB_Y + L['KNOB_VALUE_Y'], v, AMBER)
    return c


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    BG = render_background()
    ATLAS, SPRITES = render_sprites(BG)
    nbg, nat = write_sources(BG, ATLAS, SPRITES, here)
    print('background %d bytes, atlas %d bytes (%d rows)' % (nbg, nat, ATLAS.h))
    if len(sys.argv) > 1:
        write_png(sys.argv[1], mockup(BG, SPRITES))
