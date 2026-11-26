/*
 * PiRadio - the radio on its own screen, drawn as a radio set.
 *
 * The cabinet and the controls are pictures made by make_art.py, stored in
 * piradio_art.c. Each part that changes (the dial, the display, the meter,
 * the lamps, the keys and the knobs) is put together off screen on top of
 * a copy of its background and then copied onto the screen in one go, so
 * that nothing flickers.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/inputevent.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <string.h>

#include "piradio.h"
#include "piradio_art.h"

#define SCR_W		ART_W
#define SCR_DEPTH	ART_DEPTH

/* Parts to redraw */
#define D_DIAL		0x0002
#define D_NEEDLE	0x0004
#define D_MSG		0x0008
#define D_VFD		0x0010
#define D_METER		0x0020
#define D_LEDS		0x0040
#define D_KEYS		0x0080
#define D_BUTTONS	0x0100
#define D_KNOB0		0x0200	/* then D_KNOB1, D_KNOB2 */
#define D_TKNOB		0x1000

/* Buttons, in the order of the artwork */
#define B_SEEK_DN	0
#define B_SEEK_UP	1
#define B_SCAN		2
#define B_SWEEP		3
#define B_REGION	4
#define B_HELP		5
#define B_WB		6
#define B_POWER		7

/* Knobs */
#define K_VOL		0
#define K_SQL		1
#define K_GAIN		2

static const int knob_sens[3] = { 4, 2, 3 };	/* pixels of mouse movement a step */

#define DRAG_NONE	0
#define DRAG_DIAL	1
#define DRAG_TUNE	2
#define DRAG_KNOB	3
#define DRAG_BUTTON	4

#define METER_MAX	40

static struct Screen *screen = NULL;
static struct Window *win = NULL;
static struct RastPort *wrp;

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };

struct Surface
{
	struct BitMap bm;
	struct RastPort rp;
	int w, h;
};

static struct Surface bg_surf;		/* the whole background */
static struct Surface atlas_surf;	/* pressed keys and buttons, lit lamps */
static struct Surface dial_surf;	/* the dial with the band's scale */
static struct Surface scratch;		/* for putting parts together */

static struct TextFont *panel_font = NULL;
static struct TextFont *topaz_font = NULL;

/* Screen height: all of the artwork on PAL, the top NTSC_H lines (which
   hold all of the controls) on NTSC */
static int scr_h = NTSC_H;

static int dirty = 0;
static int needle_x = -1;
static int tune_rot = 0;	/* the tuning knob's angle */
static int shown_level = 0;	/* the meter's needle, which moves smoothly */
static BOOL help_shown = FALSE;

static int drag_mode = DRAG_NONE;
static int drag_item = 0;
static int drag_x0, drag_y0, drag_v0, drag_acc, drag_last_angle;
static BOOL drag_moved = FALSE;
static int pressed_button = -1;

/* ---------------------------------------------------------------------- */
/* Integer trigonometry (256 units a turn, values times 1024)             */

static const WORD qsin[65] =
{
	0, 25, 50, 75, 100, 125, 150, 175, 200, 224, 249, 273, 297, 321, 345, 369,
	392, 415, 438, 460, 483, 505, 526, 548, 569, 590, 610, 630, 650, 669, 688, 706,
	724, 742, 759, 775, 792, 807, 822, 837, 851, 865, 878, 891, 903, 915, 926, 936,
	946, 955, 964, 972, 980, 987, 993, 999, 1004, 1009, 1013, 1016, 1019, 1021, 1023, 1024,
	1024
};

static const UBYTE atab[33] =
{
	0, 1, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19,
	20, 21, 22, 23, 24, 25, 25, 26, 27, 28, 29, 29, 30, 31, 31, 32
};

static LONG isin(int a)
{
	a &= 255;
	if (a < 64)
		return qsin[a];
	if (a < 128)
		return qsin[128 - a];
	if (a < 192)
		return -qsin[a - 128];
	return -qsin[256 - a];
}

static LONG icos(int a)
{
	return isin(a + 64);
}

/* Angle of vector (x, y), with y pointing up, in 0..255 */
static int iatan2(LONG y, LONG x)
{
	LONG ax = x < 0 ? -x : x;
	LONG ay = y < 0 ? -y : y;
	int a;

	if (ax == 0 && ay == 0)
		return 0;
	if (ay <= ax)
		a = atab[(ay * 32 + ax / 2) / ax];
	else
		a = 64 - atab[(ax * 32 + ay / 2) / ay];
	if (x < 0)
		a = 128 - a;
	if (y < 0)
		a = 256 - a;
	return a & 255;
}

static LONG isqrt(LONG v)
{
	LONG r = 0, b = 1L << 30;
	while (b > v)
		b >>= 2;
	while (b)
	{
		if (v >= r + b)
		{
			v -= r + b;
			r = (r >> 1) + b;
		}
		else
			r >>= 1;
		b >>= 2;
	}
	return r;
}

/* ---------------------------------------------------------------------- */
/* Drawing primitives                                                     */

/*
 * The off-screen surfaces have no layer, so the system doesn't clip what is
 * drawn into them, and drawing outside them would damage chip memory.
 * Everything drawn into a surface is checked against its size here.
 */
static int rp_width(struct RastPort *rp)
{
	return rp->BitMap->BytesPerRow * 8;
}

static int rp_height(struct RastPort *rp)
{
	return rp->BitMap->Rows;
}

static void fill(struct RastPort *rp, int pen, int x0, int y0, int x1, int y1)
{
	if (rp->Layer == NULL)
	{
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > rp_width(rp) - 1) x1 = rp_width(rp) - 1;
		if (y1 > rp_height(rp) - 1) y1 = rp_height(rp) - 1;
	}
	if (x1 < x0 || y1 < y0)
		return;
	SetAPen(rp, pen);
	RectFill(rp, x0, y0, x1, y1);
}

static BOOL point_ok(struct RastPort *rp, int x, int y)
{
	return rp->Layer != NULL ||
		(x >= 0 && y >= 0 && x < rp_width(rp) && y < rp_height(rp));
}

static void line(struct RastPort *rp, int pen, int x0, int y0, int x1, int y1)
{
	if (!point_ok(rp, x0, y0) || !point_ok(rp, x1, y1))
		return;
	SetAPen(rp, pen);
	Move(rp, x0, y0);
	Draw(rp, x1, y1);
}

static int text_w(struct RastPort *rp, const char *s)
{
	return TextLength(rp, (STRPTR)s, strlen(s));
}

static void text_at(struct RastPort *rp, int pen, int x, int y, const char *s)
{
	if (rp->Layer == NULL)
	{
		int w = text_w(rp, s);
		if (x < 0)
			x = 0;
		if (x + w > rp_width(rp))
			x = rp_width(rp) - w;
		if (x < 0 || y < 0 || y + rp->TxHeight > rp_height(rp))
			return;
	}
	SetAPen(rp, pen);
	SetDrMd(rp, JAM1);
	Move(rp, x, y + rp->TxBaseline);
	Text(rp, (STRPTR)s, strlen(s));
}

static void text_c(struct RastPort *rp, int pen, int cx, int y, const char *s)
{
	text_at(rp, pen, cx - text_w(rp, s) / 2, y, s);
}

static void ellipse(struct RastPort *rp, int pen, int cx, int cy, int rx, int ry)
{
	int dy;
	if (rx <= 0 || ry <= 0)
		return;
	for (dy = -ry; dy <= ry; dy++)
	{
		LONG w = (LONG)rx * isqrt(((LONG)ry * ry - (LONG)dy * dy) * 256) / (ry * 16);
		fill(rp, pen, cx - w, cy + dy, cx + w, cy + dy);
	}
}

/* Point at angle a on an ellipse, scaled by pct percent */
static int ex(int cx, int rx, int a, int pct)
{
	return cx + (int)(icos(a) * rx * pct / (1024L * 100));
}

static int ey(int cy, int ry, int a, int pct)
{
	return cy - (int)(isin(a) * ry * pct / (1024L * 100));
}

static BOOL alloc_surface(struct Surface *s, int w, int h)
{
	int i;
	InitBitMap(&s->bm, SCR_DEPTH, w, h);
	for (i = 0; i < SCR_DEPTH; i++)
		s->bm.Planes[i] = NULL;
	s->w = w;
	s->h = h;
	for (i = 0; i < SCR_DEPTH; i++)
	{
		s->bm.Planes[i] = AllocRaster(w, h);
		if (!s->bm.Planes[i])
			return FALSE;
	}
	InitRastPort(&s->rp);
	s->rp.BitMap = &s->bm;
	SetFont(&s->rp, panel_font);
	return TRUE;
}

static void free_surface(struct Surface *s)
{
	int i;
	for (i = 0; i < SCR_DEPTH; i++)
	{
		if (s->bm.Planes[i])
			FreeRaster(s->bm.Planes[i], s->w, s->h);
		s->bm.Planes[i] = NULL;
	}
}

static void blit_scratch(int x, int y, int w, int h)
{
	BltBitMapRastPort(&scratch.bm, 0, 0, wrp, x, y, w, h, 0xC0);
}

/* ---------------------------------------------------------------------- */
/* Artwork                                                                */

/* Unpacks a ByteRun1-compressed picture made by make_art.py */
static BOOL unpack_art(const UBYTE *src, ULONG srclen, struct BitMap *bm)
{
	const UBYTE *end = src + srclen;
	int bpr = bm->BytesPerRow;
	int row, p;

	for (row = 0; row < bm->Rows; row++)
	{
		for (p = 0; p < bm->Depth; p++)
		{
			UBYTE *dst = bm->Planes[p] + (LONG)row * bpr;
			int n = 0;
			while (n < bpr)
			{
				int c, k;
				if (src >= end)
					return FALSE;
				c = (BYTE)*src++;
				if (c >= 0)
				{
					k = c + 1;
					if (n + k > bpr || src + k > end)
						return FALSE;
					memcpy(dst + n, src, k);
					src += k;
					n += k;
				}
				else if (c != -128)
				{
					k = 1 - c;
					if (n + k > bpr || src >= end)
						return FALSE;
					memset(dst + n, *src++, k);
					n += k;
				}
			}
		}
	}
	return TRUE;
}

static WORD font_kern[FONT_HI - FONT_LO + 2];

/* Makes a TextFont of the panel font in piradio_art.c (not added to the
   system's font list, so it is freed with FreeMem) */
static struct TextFont *make_font(void)
{
	struct TextFont *tf = (struct TextFont *)AllocMem(sizeof(struct TextFont), MEMF_PUBLIC | MEMF_CLEAR);
	UBYTE *data;

	if (!tf)
		return NULL;
	data = (UBYTE *)AllocMem(sizeof(art_font_data), MEMF_CHIP);
	if (!data)
	{
		FreeMem(tf, sizeof(struct TextFont));
		return NULL;
	}
	CopyMem((APTR)art_font_data, data, sizeof(art_font_data));

	tf->tf_Message.mn_Node.ln_Type = NT_FONT;
	tf->tf_Message.mn_Node.ln_Name = (char *)"piradio.font";
	tf->tf_YSize = FONT_H;
	tf->tf_Style = FS_NORMAL;
	tf->tf_Flags = FPF_PROPORTIONAL | FPF_DESIGNED;
	tf->tf_XSize = 7;
	tf->tf_Baseline = FONT_BASELINE;
	tf->tf_BoldSmear = 1;
	tf->tf_LoChar = FONT_LO;
	tf->tf_HiChar = FONT_HI;
	tf->tf_CharData = (APTR)data;
	tf->tf_Modulo = FONT_MODULO;
	tf->tf_CharLoc = (APTR)art_font_loc;
	tf->tf_CharSpace = (APTR)art_font_space;
	tf->tf_CharKern = (APTR)font_kern;
	return tf;
}

static void free_font(struct TextFont *tf)
{
	if (tf)
	{
		FreeMem(tf->tf_CharData, sizeof(art_font_data));
		FreeMem(tf, sizeof(struct TextFont));
	}
}

/* Copies part of the background into the scratch surface at 0,0 */
static void restore_to_scratch(int x, int y, int w, int h)
{
	BltBitMap(&bg_surf.bm, x, y, &scratch.bm, 0, 0, w, h, 0xC0, 0xFF, NULL);
}

/* Draws a picture from the atlas into the scratch surface, which holds the
   screen area starting at (sx, sy) */
static void sprite_to_scratch(int id, int sx, int sy)
{
	const struct ArtSprite *s = &art_sprites[id];
	BltBitMap(&atlas_surf.bm, s->ax, s->ay, &scratch.bm, s->x - sx, s->y - sy, s->w, s->h, 0xC0, 0xFF, NULL);
}

static void sprite_to_window(int id)
{
	const struct ArtSprite *s = &art_sprites[id];
	BltBitMapRastPort(&atlas_surf.bm, s->ax, s->ay, wrp, s->x, s->y, s->w, s->h, 0xC0);
}

static void restore_window(int x, int y, int w, int h)
{
	BltBitMapRastPort(&bg_surf.bm, x, y, wrp, x, y, w, h, 0xC0);
}

/* ---------------------------------------------------------------------- */
/* The dial                                                               */

static LONG span_div(LONG span)
{
	LONG div = 1;
	while (span / div > 2000000L)
		div *= 10;
	return div;
}

/* Frequency to x within the dial surface */
static int freq_to_x(LONG f)
{
	struct Band *b = &bands[cur_band];
	LONG span = b->hi - b->lo, d = f - b->lo, div;

	if (span <= 0)
		return SC_X0;
	if (d < 0)
		d = 0;
	if (d > span)
		d = span;
	div = span_div(span);
	return SC_X0 + (int)((d / div) * SC_W / (span / div));
}

static LONG x_to_freq(int x)
{
	struct Band *b = &bands[cur_band];
	LONG span = b->hi - b->lo, d = x - SC_X0, div = span_div(span);

	if (d < 0)
		d = 0;
	if (d > SC_W)
		d = SC_W;
	return b->lo + (d * (span / div) / SC_W) * div;
}

static void format_label(char *buf, LONG f, LONG step)
{
	LONG mhz = f / 1000000L, frac = f % 1000000L;
	if (step % 1000000L == 0)
		sprintf(buf, "%ld", mhz);
	else if (step % 100000L == 0)
		sprintf(buf, "%ld.%01ld", mhz, frac / 100000L);
	else if (step % 10000L == 0)
		sprintf(buf, "%ld.%02ld", mhz, frac / 10000L);
	else
		sprintf(buf, "%ld.%03ld", mhz, frac / 1000L);
}

static void format_step(char *buf, LONG step)
{
	LONG k = step / 1000, r = step % 1000;
	if (step < 1000)
		sprintf(buf, "STEP %ld HZ", step);
	else if (r == 0)
		sprintf(buf, "STEP %ld KHZ", k);
	else if (r % 100 == 0)
		sprintf(buf, "STEP %ld.%ld KHZ", k, r / 100);
	else if (r % 10 == 0)
		sprintf(buf, "STEP %ld.%02ld KHZ", k, r / 10);
	else
		sprintf(buf, "STEP %ld.%03ld KHZ", k, r);
}

static const char *mode_name(const char *m)
{
	if (strcmp(m, "wbfm") == 0) return "WBFM";
	if (strcmp(m, "fm") == 0) return "NFM";
	if (strcmp(m, "am") == 0) return "AM";
	if (strcmp(m, "usb") == 0) return "USB";
	if (strcmp(m, "lsb") == 0) return "LSB";
	return "RAW";
}

static const LONG tick_steps[] =
{
	1000, 2000, 5000, 10000, 20000, 25000, 50000, 100000, 200000, 250000,
	500000, 1000000, 2000000, 5000000, 10000000, 20000000, 50000000,
	100000000, 200000000, 500000000, 0
};

/* The dial surface holds the glowing face with the current band's scale
   printed on it; the needle is drawn over it on the screen. */
static void build_dial(void)
{
	struct RastPort *rp = &dial_surf.rp;
	struct Band *b = &bands[cur_band];
	LONG span, div, step = 0, minor, m, f;
	char buf[40];
	int i, px;

	BltBitMap(&bg_surf.bm, DIAL_X, DIAL_Y, &dial_surf.bm, 0, 0, DIAL_W, DIAL_H, 0xC0, 0xFF, NULL);
	if (nbands == 0 || !have_state)
		return;

	text_c(rp, C_INK, DIAL_W / 2, TITLE_Y, b->name);
	text_at(rp, C_INK, 10, TITLE_Y, mode_name(b->mode));
	format_step(buf, b->step);
	text_at(rp, C_INK, DIAL_W - 10 - text_w(rp, buf), TITLE_Y, buf);

	/* The scale: labelled ticks at least 50 pixels apart */
	span = b->hi - b->lo;
	div = span_div(span);
	for (i = 0; tick_steps[i]; i++)
	{
		if (tick_steps[i] < div)
			continue;
		px = (int)((tick_steps[i] / div) * SC_W / (span / div));
		if (px >= 50)
		{
			step = tick_steps[i];
			break;
		}
	}
	if (step == 0)
		step = span;
	m = step;
	while (m % 10 == 0)
		m /= 10;
	minor = (m == 2) ? step / 4 : step / 5;
	if (minor < div || (minor / div) * SC_W / (span / div) < 4)
		minor = step;

	line(rp, C_INK, SC_X0, BASE_Y, SC_X0 + SC_W, BASE_Y);
	for (f = ((b->lo + minor - 1) / minor) * minor; f <= b->hi; f += minor)
	{
		int x = freq_to_x(f);
		if (f % step == 0)
		{
			fill(rp, C_INK, x, TICK_Y0, x + 1, BASE_Y);
			format_label(buf, f, step);
			text_c(rp, C_INK, x + 1, LABEL_Y, buf);
		}
		else
			line(rp, C_INK, x, MINOR_Y0, x, BASE_Y);
	}

	/* Markers for the active channels found by the last sweep */
	for (i = 0; i < nactive; i++)
	{
		int x = freq_to_x(b->lo + active[i] * b->step);
		int k;
		for (k = 0; k < 4; k++)
			fill(rp, C_VFD, x - k, MARK_Y + k, x + k, MARK_Y + k);
	}
}

static const char *help_lines[14] =
{
	"CURSOR L/R  Tune one step",
	"  + SHIFT x10, + ALT x100",
	"CURSOR U/D  Seek next signal",
	"SPACE       Scan (SHIFT: down)",
	"R           Sweep band for signals",
	"F1-F10      Select band",
	"W           Back to Workbench",
	"0-9 . RET   Enter frequency in MHz",
	"v / V       Volume down / up",
	"s / S       Squelch down / up",
	"g / G  a    Gain down / up, auto",
	"HELP / ?    Show or hide this help",
	"ESC / Q     Quit",
	"Mouse: drag the dial and knobs"
};

static void draw_help(void)
{
	int i;
	/* The plain face, in topaz for the mixed-case text. The lines fit
	   above the message strip. */
	restore_window(DIAL_X, DIAL_Y, DIAL_W, MSG_Y0 - 1);
	SetFont(wrp, topaz_font);
	for (i = 0; i < 14; i++)
	{
		int col = i / 7, row = i % 7;
		text_at(wrp, C_INK, DIAL_X + 16 + col * 292, DIAL_Y + 1 + row * HELP_PITCH, help_lines[i]);
	}
	SetFont(wrp, panel_font);
}

static void draw_needle(void)
{
	int x;
	if (help_shown)
		return;
	if (needle_x >= 0)
		BltBitMapRastPort(&dial_surf.bm, needle_x - 1, NEEDLE_Y0, wrp,
			DIAL_X + needle_x - 1, DIAL_Y + NEEDLE_Y0, 5, NEEDLE_Y1 - NEEDLE_Y0 + 1, 0xC0);
	needle_x = -1;
	if (nbands == 0 || !have_state)
		return;
	x = freq_to_x(freq);
	fill(wrp, C_WOOD_L, DIAL_X + x + 2, DIAL_Y + NEEDLE_Y0 + 2, DIAL_X + x + 2, DIAL_Y + NEEDLE_Y1);
	fill(wrp, C_RED, DIAL_X + x, DIAL_Y + NEEDLE_Y0, DIAL_X + x + 1, DIAL_Y + NEEDLE_Y1);
	fill(wrp, C_HILITE, DIAL_X + x, DIAL_Y + NEEDLE_Y0, DIAL_X + x, DIAL_Y + NEEDLE_Y0 + 1);
	needle_x = x;
}

/* The message strip: a message from the Pi, or else the region */
static void draw_msg(void)
{
	char buf[80];
	const char *s = msg;

	if (help_shown)
		return;
	if (!*s)
	{
		if (nregions > 1 && have_state)
			sprintf(buf, "REGION: %.30s  -  PRESS HELP FOR KEYS", regions[cur_region]);
		else
			strcpy(buf, "PRESS HELP FOR KEYBOARD SHORTCUTS");
		s = buf;
	}
	BltBitMapRastPort(&dial_surf.bm, MSG_X0, MSG_Y0, wrp, DIAL_X + MSG_X0, DIAL_Y + MSG_Y0,
		MSG_X1 - MSG_X0 + 1, MSG_Y1 - MSG_Y0 + 1, 0xC0);
	text_c(wrp, *msg ? C_AMBER : C_STEEL, DIAL_X + DIAL_W / 2, DIAL_Y + MSG_TEXT_Y, s);
}

static void show_dial(void)
{
	BltBitMapRastPort(&dial_surf.bm, 0, 0, wrp, DIAL_X, DIAL_Y, DIAL_W, DIAL_H, 0xC0);
	needle_x = -1;
	if (help_shown)
	{
		draw_help();
		return;
	}
	draw_needle();
	draw_msg();
}

/* ---------------------------------------------------------------------- */
/* The frequency display                                                  */

static const UBYTE seg_digits[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

static void draw_digit(struct RastPort *rp, int x, int y, int mask, int on, int off)
{
	int h2 = DIG_H / 2, t = DIG_T;
	fill(rp, (mask & 0x01) ? on : off, x + 2, y, x + DIG_W - 3, y + t - 1);
	fill(rp, (mask & 0x02) ? on : off, x + DIG_W - t, y + 2, x + DIG_W - 1, y + h2 - 2);
	fill(rp, (mask & 0x04) ? on : off, x + DIG_W - t, y + h2 + 2, x + DIG_W - 1, y + DIG_H - 3);
	fill(rp, (mask & 0x08) ? on : off, x + 2, y + DIG_H - t, x + DIG_W - 3, y + DIG_H - 1);
	fill(rp, (mask & 0x10) ? on : off, x, y + h2 + 2, x + t - 1, y + DIG_H - 3);
	fill(rp, (mask & 0x20) ? on : off, x, y + 2, x + t - 1, y + h2 - 2);
	fill(rp, (mask & 0x40) ? on : off, x + 2, y + h2 - t / 2, x + DIG_W - 3, y + h2 + t / 2 - 1);
}

static void draw_vfd(void)
{
	struct RastPort *rp = &scratch.rp;
	int cells[8];
	int i, on = C_VFD;
	BOOL dp = TRUE;

	restore_to_scratch(VFD_X, VFD_Y, VFD_W, VFD_H);
	for (i = 0; i < 8; i++)
		cells[i] = -1;

	if (entry_len >= 0)
	{
		/* What is being typed: the MHz before the point, the rest after */
		char *dot = strchr(entry, '.');
		int ilen = dot ? (int)(dot - entry) : entry_len;
		int n = ilen < 4 ? ilen : 4;
		int k;
		for (k = 0; k < n; k++)
			cells[4 - n + k] = entry[ilen - n + k] - '0';
		if (dot)
			for (k = 0; k < 4 && dot[1 + k]; k++)
				cells[4 + k] = dot[1 + k] - '0';
		dp = dot != NULL;
		on = C_AMBER;
	}
	else if (have_state && nbands)
	{
		LONG mhz = freq / 1000000L;
		LONG frac = (freq % 1000000L) / 100;
		for (i = 3; i >= 0; i--)
		{
			cells[i] = (int)(mhz % 10);
			mhz /= 10;
			if (mhz == 0)
				break;
		}
		for (i = 7; i >= 4; i--)
		{
			cells[i] = (int)(frac % 10);
			frac /= 10;
		}
	}

	for (i = 0; i < 8; i++)
		draw_digit(rp, art_digit_x[i], DIG_Y, cells[i] >= 0 && cells[i] <= 9 ? seg_digits[cells[i]] : 0, on, C_VFD_DIM);
	fill(rp, dp ? on : C_VFD_DIM, DOT_X, DIG_Y + DIG_H - 4, DOT_X + 3, DIG_Y + DIG_H - 1);

	blit_scratch(VFD_X, VFD_Y, VFD_W, VFD_H);
}

/* ---------------------------------------------------------------------- */
/* The level meter and the lamps                                          */

static void draw_meter(void)
{
	struct RastPort *rp = &scratch.rp;
	int lv, a, x0, y0, x1, y1;

	restore_to_scratch(METER_X, METER_Y, METER_W, METER_H);

	lv = shown_level > METER_MAX ? METER_MAX : shown_level;
	a = METER_A0 - lv * (METER_A0 - METER_A1) / METER_MAX;
	x0 = ex(METER_CX, METER_RX, a, METER_PCT0);
	y0 = ey(METER_CY, METER_RY, a, METER_PCT0);
	x1 = ex(METER_CX, METER_RX, a, METER_PCT1);
	y1 = ey(METER_CY, METER_RY, a, METER_PCT1);
	line(rp, C_BLACK, x0, y0, x1, y1);
	line(rp, C_BLACK, x0 + 1, y0, x1 + 1, y1);
	fill(rp, C_RED, x1, y1, x1 + 1, y1);

	blit_scratch(METER_X, METER_Y, METER_W, METER_H);
}

#define LAMP_OFF	-1
#define LAMP_GREEN	0
#define LAMP_RED	1
#define LAMP_AMBER	2

static int lamp_state(int i)
{
	switch (i)
	{
	case 0:	/* TUNER */
		if (run_status == 1)
			return LAMP_GREEN;
		if (run_status == 2)
			return LAMP_RED;
		if (run_status == 0 || run_status == 3)
			return (blink & 2) ? LAMP_AMBER : LAMP_OFF;
		return LAMP_OFF;
	case 1:	/* SIGNAL */
		return (run_status == 1 && level >= 2) ? LAMP_GREEN : LAMP_OFF;
	case 2:	/* AUDIO */
		if (pipe_status == 1)
			return LAMP_GREEN;
		if (pipe_status == 0)
			return LAMP_RED;
		return LAMP_OFF;
	default:	/* SCAN */
		return scan_dir ? LAMP_AMBER : LAMP_OFF;
	}
}

static int lamp_shown[LED_COUNT] = { LAMP_OFF, LAMP_OFF, LAMP_OFF, LAMP_OFF };

static void draw_leds(void)
{
	int i;
	for (i = 0; i < LED_COUNT; i++)
	{
		int st = lamp_state(i);
		if (st == lamp_shown[i])
			continue;
		lamp_shown[i] = st;
		if (st == LAMP_OFF)
			restore_window(LED_X - 1, art_led_y[i], LED_W, LED_H);
		else
			sprite_to_window(SPR_LED0_GREEN + i * 3 + st);
	}
}

/* ---------------------------------------------------------------------- */
/* Band keys and buttons                                                  */

/* One key per band; the keys share the width of their channel */
static int key_count(void)
{
	return nbands < KEY_COUNT ? nbands : KEY_COUNT;
}

/* Copies the first w columns of an atlas picture into the scratch surface */
static void key_slice(int id, int dx, int w)
{
	const struct ArtSprite *s = &art_sprites[id];
	BltBitMap(&atlas_surf.bm, s->ax, s->ay, &scratch.bm, dx, 0, w, s->h, 0xC0, 0xFF, NULL);
}

/* A key of any width, from its left end, middle pieces and right end */
static void draw_one_key(int x, int w, BOOL down)
{
	int l = down ? SPR_KEY_DOWN_L : SPR_KEY_UP_L;
	int mx = x + KEY_CAP, mend = x + w - KEY_CAP;

	key_slice(l, x, KEY_CAP);
	while (mx < mend)
	{
		int n = mend - mx;
		if (n > KEY_TILE)
			n = KEY_TILE;
		key_slice(l + 1, mx, n);
		mx += n;
	}
	key_slice(l + 2, mend, KEY_CAP);
}

static void draw_keys(void)
{
	struct RastPort *rp = &scratch.rp;
	int i, n = key_count();

	restore_to_scratch(KEYS_X, KEYS_Y, KEYS_W, KEYS_H);
	if (have_state)
	{
		for (i = 0; i < n; i++)
		{
			BOOL down = (i == cur_band);
			int x0 = i * KEYS_W / n;
			int w = (i + 1) * KEYS_W / n - KEY_GAP - x0;
			draw_one_key(x0, w, down);
			text_c(rp, down ? C_RED : C_INK, x0 + w / 2, down ? KEY_LABEL_Y_DOWN : KEY_LABEL_Y_UP,
				bands[i].shortname);
		}
	}
	blit_scratch(KEYS_X, KEYS_Y, KEYS_W, KEYS_H);
}

static void draw_buttons(void)
{
	int x = BTN_X - 1, y = BTN_Y - 1, w = BTN_PITCH * BTN_COUNT, h = BTN_H + 2;

	restore_to_scratch(x, y, w, h);
	if (pressed_button >= 0 && pressed_button < BTN_COUNT)
		sprite_to_scratch(SPR_BTN0 + pressed_button, x, y);
	blit_scratch(x, y, w, h);
}

/* ---------------------------------------------------------------------- */
/* Knobs                                                                  */

static int knob_units(int k)
{
	if (k == K_VOL)
		return vol;
	if (k == K_SQL)
		return sql / SQL_STEP;
	return gain_index();
}

static int knob_max(int k)
{
	if (k == K_VOL)
		return 16;
	if (k == K_SQL)
		return SQL_MAX / SQL_STEP;
	return ngains;
}

static void set_knob_units(int k, int u)
{
	if (k == K_VOL)
		set_vol(u);
	else if (k == K_SQL)
		set_sql(u * SQL_STEP);
	else
		set_gain_index(u);
}

static void draw_knob(int k)
{
	struct RastPort *rp = &scratch.rp;
	char buf[16];
	int u = knob_units(k), umax = knob_max(k);
	int a, x0, y0, x1, y1;

	restore_to_scratch(KNOB_X + k * KNOB_W, KNOB_Y, KNOB_W, KNOB_H);

	if (u < 0) u = 0;
	if (umax <= 0) umax = 1;
	if (u > umax) u = umax;
	/* 270 degrees, from lower left (225) to lower right (-45) */
	a = 160 - u * 192 / umax;

	x0 = ex(KNOB_CX, KNOB_RX, a, 12);
	y0 = ey(KNOB_CY, KNOB_RY, a, 12);
	x1 = ex(KNOB_CX, KNOB_RX, a, 60);
	y1 = ey(KNOB_CY, KNOB_RY, a, 60);
	line(rp, C_BLACK, x0 + 1, y0 + 1, x1 + 1, y1 + 1);
	line(rp, C_HILITE, x0, y0, x1, y1);
	line(rp, C_STEEL_L, x0 + 1, y0, x1 + 1, y1);

	if (k == K_VOL)
		sprintf(buf, "%d%%", vol * 100 / 8);
	else if (k == K_SQL)
	{
		if (sql == 0)
			strcpy(buf, "OFF");
		else
			sprintf(buf, "%d", sql);
	}
	else if (gain < 0)
		strcpy(buf, "AUTO");
	else
		sprintf(buf, "%d.%d", gain / 10, gain % 10);
	text_at(rp, C_AMBER, KNOB_VALUE_X1 - text_w(rp, buf), KNOB_VALUE_Y, buf);

	blit_scratch(KNOB_X + k * KNOB_W, KNOB_Y, KNOB_W, KNOB_H);
}

static void draw_tuning_knob(void)
{
	struct RastPort *rp = &scratch.rp;
	int dx, dy;

	restore_to_scratch(TK_X, TK_Y, TK_W, TK_H);

	/* Finger dimple: a small hollow, lit from the top left */
	dx = ex(TK_CX, TK_RX, tune_rot, TK_DIMPLE_PCT);
	dy = ey(TK_CY, TK_RY, tune_rot, TK_DIMPLE_PCT);
	ellipse(rp, C_GUN, dx, dy, 6, 3);
	ellipse(rp, C_BLACK, dx - 1, dy, 5, 2);
	fill(rp, C_STEEL, dx + 1, dy + 3, dx + 4, dy + 3);

	blit_scratch(TK_X, TK_Y, TK_W, TK_H);
}

/* ---------------------------------------------------------------------- */

static void refresh(void)
{
	int c = changed, d;

	changed = 0;
	if (c & CH_FREQ)
		dirty |= D_NEEDLE | D_VFD;
	if (c & CH_DIAL)
		dirty |= D_DIAL | D_VFD;
	if (c & CH_BANDS)
		dirty |= D_KEYS | D_MSG;
	if (c & (CH_LAMPS | CH_SCAN))
		dirty |= D_LEDS;
	if (c & CH_MSG)
		dirty |= D_MSG;
	if (c & CH_VOL)
		dirty |= D_KNOB0;
	if (c & CH_SQL)
		dirty |= D_KNOB0 << 1;
	if (c & CH_GAIN)
		dirty |= D_KNOB0 << 2;
	/* CH_LEVEL: the meter follows the level in tick() */

	d = dirty;
	dirty = 0;
	if (d & D_DIAL)
	{
		build_dial();
		show_dial();
		d &= ~(D_NEEDLE | D_MSG);
	}
	if (d & D_NEEDLE)
		draw_needle();
	if (d & D_MSG)
		draw_msg();
	if (d & D_VFD)
		draw_vfd();
	if (d & D_METER)
		draw_meter();
	if (d & D_LEDS)
		draw_leds();
	if (d & D_KEYS)
		draw_keys();
	if (d & D_BUTTONS)
		draw_buttons();
	if (d & D_KNOB0)
		draw_knob(K_VOL);
	if (d & (D_KNOB0 << 1))
		draw_knob(K_SQL);
	if (d & (D_KNOB0 << 2))
		draw_knob(K_GAIN);
	if (d & D_TKNOB)
		draw_tuning_knob();
}

/* ---------------------------------------------------------------------- */
/* Input                                                                  */

static void turn_tuning_knob(int n)
{
	tune_steps(n);
	tune_rot = (tune_rot - n * 8) & 255;
	dirty |= D_TKNOB;
}

/* Puts the radio behind the Workbench; it carries on playing. It comes
   back with the screen depth gadget or Left-Amiga+M as usual. */
static void to_back(void)
{
	ScreenToBack(screen);
	WBenchToFront();
}

static void toggle_help(void)
{
	help_shown = !help_shown;
	if (help_shown)
		draw_help();
	else
		show_dial();
}

static BOOL in_rect(int x, int y, int x0, int y0, int w, int h)
{
	return x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
}

static int tune_knob_angle(int mx, int my)
{
	int dx = mx - (TK_X + TK_CX);
	int dy = (TK_Y + TK_CY) - my;
	return iatan2((LONG)dy * 2, dx);
}

static void press_button(int i, UWORD qual)
{
	switch (i)
	{
	case B_SEEK_DN: do_seek(-1); break;
	case B_SEEK_UP: do_seek(1); break;
	case B_SCAN: toggle_scan((qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) ? -1 : 1); break;
	case B_SWEEP: do_sweep(); break;
	case B_REGION: next_region(); break;
	case B_HELP: toggle_help(); break;
	case B_WB: to_back(); break;
	case B_POWER: quit = TRUE; break;
	}
}

static void mouse_down(int mx, int my, UWORD qual)
{
	int i;

	drag_moved = FALSE;
	drag_x0 = mx;
	drag_y0 = my;

	if (in_rect(mx, my, DIAL_X, DIAL_Y, DIAL_W, DIAL_H))
	{
		if (help_shown)
		{
			toggle_help();
			return;
		}
		drag_mode = DRAG_DIAL;
		tune_to(x_to_freq(mx - DIAL_X));
	}
	else if (in_rect(mx, my, TK_X, TK_Y, TK_W, TK_H - 9))
	{
		drag_mode = DRAG_TUNE;
		drag_acc = 0;
		drag_last_angle = tune_knob_angle(mx, my);
	}
	else if (in_rect(mx, my, KNOB_X, KNOB_Y, 3 * KNOB_W, KNOB_H))
	{
		drag_mode = DRAG_KNOB;
		drag_item = (mx - KNOB_X) / KNOB_W;
		drag_v0 = knob_units(drag_item);
	}
	else if (in_rect(mx, my, KEYS_X, KEYS_Y, KEYS_W, KEYS_H) && key_count() > 0)
		select_band((mx - KEYS_X) * key_count() / KEYS_W);
	else if (in_rect(mx, my, BTN_X, BTN_Y, BTN_PITCH * BTN_COUNT, BTN_H))
	{
		i = (mx - BTN_X) / BTN_PITCH;
		if (i < BTN_COUNT && (mx - BTN_X) % BTN_PITCH < BTN_W)
		{
			drag_mode = DRAG_BUTTON;
			pressed_button = i;
			dirty |= D_BUTTONS;
			press_button(i, qual);
		}
	}
	/* Only dragging the dial or a knob holds off the Pi's state (a button
	   is held down for a moment too, while the Pi answers it) */
	ui_dragging = drag_mode == DRAG_DIAL || drag_mode == DRAG_TUNE || drag_mode == DRAG_KNOB;
}

static void mouse_move(int mx, int my)
{
	int ddx = mx - drag_x0, ddy = my - drag_y0;

	if (ddx * ddx + ddy * ddy > 4)
		drag_moved = TRUE;

	switch (drag_mode)
	{
	case DRAG_DIAL:
		tune_to(x_to_freq(mx - DIAL_X));
		break;

	case DRAG_TUNE:
	{
		int a = tune_knob_angle(mx, my);
		int d = ((a - drag_last_angle + 128) & 255) - 128;
		drag_last_angle = a;
		drag_acc += d;
		/* Clockwise (a smaller angle) tunes up: 32 steps a turn */
		while (drag_acc <= -8)
		{
			turn_tuning_knob(1);
			drag_acc += 8;
		}
		while (drag_acc >= 8)
		{
			turn_tuning_knob(-1);
			drag_acc -= 8;
		}
		break;
	}

	case DRAG_KNOB:
		/* Up or right turns it up */
		set_knob_units(drag_item, drag_v0 + ((drag_y0 - my) + (mx - drag_x0) / 2) / knob_sens[drag_item]);
		break;
	}
}

static void mouse_up(int mx)
{
	/* A click on the tuning knob steps down or up, by side */
	if (drag_mode == DRAG_TUNE && !drag_moved)
		turn_tuning_knob(mx < TK_X + TK_CX ? -1 : 1);
	if (pressed_button >= 0)
	{
		pressed_button = -1;
		dirty |= D_BUTTONS;
	}
	drag_mode = DRAG_NONE;
	ui_dragging = FALSE;
}

static void handle(void)
{
	struct IntuiMessage *im;

	while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL)
	{
		ULONG cls = im->Class;
		UWORD code = im->Code, qual = im->Qualifier;
		int mx = im->MouseX, my = im->MouseY;
		/* For dead keys, RawKeyConvert() needs the previous keys */
		APTR prev = (cls == IDCMP_RAWKEY && im->IAddress) ? *(APTR *)im->IAddress : NULL;
		ReplyMsg((struct Message *)im);

		switch (cls)
		{
		case IDCMP_MOUSEBUTTONS:
			if (code == SELECTDOWN)
				mouse_down(mx, my, qual);
			else if (code == SELECTUP)
				mouse_up(mx);
			break;
		case IDCMP_MOUSEMOVE:
			if (drag_mode != DRAG_NONE)
				mouse_move(mx, my);
			break;
		case IDCMP_RAWKEY:
			if (help_shown && code == 0x45)	/* Esc closes the help first */
				toggle_help();
			else
				handle_rawkey(code, qual, prev);
			break;
		}
	}
}

static void tick(void)
{
	/* The meter's needle: up quickly, down more slowly */
	int target = level, before = shown_level;

	if (target > shown_level)
		shown_level = target;
	else if (target < shown_level)
	{
		int d = (shown_level - target) / 3;
		shown_level -= d > 0 ? d : 1;
	}
	if ((shown_level > METER_MAX ? METER_MAX : shown_level) != (before > METER_MAX ? METER_MAX : before))
		dirty |= D_METER;
}

/* ---------------------------------------------------------------------- */

static void close_panel(void)
{
	if (win)
		CloseWindow(win);
	win = NULL;
	free_surface(&scratch);
	free_surface(&dial_surf);
	free_surface(&atlas_surf);
	free_surface(&bg_surf);
	free_font(panel_font);
	panel_font = NULL;
	if (screen)
		CloseScreen(screen);
	screen = NULL;
}

static BOOL open_panel(void)
{
	struct NewScreen ns;
	struct NewWindow nw;
	int i;

	if (GfxBase->DisplayFlags & PAL)
		scr_h = ART_H;

	memset(&ns, 0, sizeof(ns));
	ns.Width = SCR_W;
	ns.Height = scr_h;
	ns.Depth = SCR_DEPTH;
	ns.DetailPen = C_BLACK;
	ns.BlockPen = C_STEEL;
	ns.ViewModes = HIRES;
	ns.Type = CUSTOMSCREEN | SCREENQUIET;
	ns.Font = &topaz8;
	ns.DefaultTitle = (UBYTE *)"PiRadio";
	screen = OpenScreen(&ns);
	if (!screen)
	{
		report("Unable to open PiRadio's screen (not enough chip memory?)");
		return FALSE;
	}
	LoadRGB4(&screen->ViewPort, (UWORD *)art_palette, 16);
	/* The title bar goes behind the backdrop window, so that the artwork
	   covers the whole screen */
	ShowTitle(screen, FALSE);

	memset(&nw, 0, sizeof(nw));
	nw.Width = SCR_W;
	nw.Height = scr_h;
	nw.DetailPen = C_BLACK;
	nw.BlockPen = C_STEEL;
	nw.IDCMPFlags = IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY;
	nw.Flags = WFLG_BACKDROP | WFLG_BORDERLESS | WFLG_ACTIVATE | WFLG_RMBTRAP |
		WFLG_REPORTMOUSE | WFLG_SMART_REFRESH | WFLG_NOCAREREFRESH;
	nw.Screen = screen;
	nw.Type = CUSTOMSCREEN;
	win = OpenWindow(&nw);
	if (!win)
	{
		close_panel();
		report("Unable to open PiRadio's window");
		return FALSE;
	}
	wrp = win->RPort;
	topaz_font = wrp->Font;

	panel_font = make_font();
	if (!panel_font ||
		!alloc_surface(&bg_surf, ART_W, ART_H) ||
		!alloc_surface(&atlas_surf, ART_W, ATLAS_H) ||
		!alloc_surface(&dial_surf, DIAL_W, DIAL_H) ||
		!alloc_surface(&scratch, SCRATCH_W, SCRATCH_H))
	{
		close_panel();
		report("Not enough chip memory for PiRadio");
		return FALSE;
	}
	if (!unpack_art(art_background, sizeof(art_background), &bg_surf.bm) ||
		!unpack_art(art_atlas, sizeof(art_atlas), &atlas_surf.bm))
	{
		close_panel();
		report("PiRadio's pictures are damaged");
		return FALSE;
	}
	SetFont(wrp, panel_font);

	restore_window(0, 0, SCR_W, scr_h);
	for (i = 0; i < LED_COUNT; i++)
		lamp_shown[i] = LAMP_OFF;
	dirty = D_DIAL | D_VFD | D_METER | D_LEDS | D_KEYS | D_BUTTONS |
		D_KNOB0 | (D_KNOB0 << 1) | (D_KNOB0 << 2) | D_TKNOB;
	return TRUE;
}

static ULONG signals(void)
{
	return 1L << win->UserPort->mp_SigBit;
}

static struct Window *window(void)
{
	return win;
}

const struct Display panel_display =
{
	open_panel, close_panel, signals, handle, refresh, tick, toggle_help, to_back, window
};
