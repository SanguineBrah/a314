/*
 * PiRadio - the radio in a window on the Workbench (the WINDOW ToolType).
 *
 * Made with a314gui, so that it looks like a tool of the system: on
 * Workbench 2.04 and later with gadtools.library in the screen's font
 * (cycle gadgets for the band and region, sliders for volume, squelch and
 * gain); on 1.3 with plain gadgets. The dial and the level meter are drawn
 * here, the dial off screen first so that it never flickers.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/intuition.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <string.h>

#include "piradio.h"
#include "../prefs/a314gui.h"

/* Gadgets and menu items */
#define ID_FREQ		1
#define ID_STATION	2
#define ID_DIAL		3
#define ID_LEVEL_LABEL	4
#define ID_METER	5
#define ID_STATUS	6
#define ID_BAND		7
#define ID_REGION	8
#define ID_SEEK_DOWN	9
#define ID_SEEK_UP	10
#define ID_SCAN		11
#define ID_SWEEP	12
#define ID_VOL		13
#define ID_SQL		14
#define ID_GAIN		15
#define ID_VOL_VALUE	16
#define ID_SQL_VALUE	17
#define ID_GAIN_VALUE	18
#define ID_MSG		19
#define ID_QUIT		30

static const struct GuiItem items[] =
{
	{ GUI_TEXT, ID_FREQ, 10, 0, 17, 0, "Frequency", 0 },
	{ GUI_TEXT, ID_STATION, 28, 0, 32, 0, NULL, 0 },
	{ GUI_AREA, ID_DIAL, 0, 1, 60, 2, NULL, 0 },
	{ GUI_LABEL, ID_LEVEL_LABEL, 0, 3, 9, 0, NULL, 0 },
	{ GUI_AREA, ID_METER, 10, 3, 17, 1, NULL, 0 },
	{ GUI_TEXT, ID_STATUS, 28, 3, 32, 0, NULL, 0 },
	{ GUI_CYCLE, ID_BAND, 10, 4, 23, 0, "_Band", 0 },
	{ GUI_CYCLE, ID_REGION, 42, 4, 18, 0, "R_egion", 0 },
	{ GUI_BUTTON, ID_SEEK_DOWN, 0, 5, 14, 0, "<< Seek", 0 },
	{ GUI_BUTTON, ID_SEEK_UP, 15, 5, 14, 0, "Seek >>", 0 },
	{ GUI_BUTTON, ID_SCAN, 31, 5, 14, 0, "Scan", 0 },
	{ GUI_BUTTON, ID_SWEEP, 46, 5, 14, 0, "Sweep", 0 },
	{ GUI_SLIDER, ID_VOL, 10, 6, 37, 0, "Volume", 0 },
	{ GUI_TEXT, ID_VOL_VALUE, 48, 6, 12, 0, NULL, 0 },
	{ GUI_SLIDER, ID_SQL, 10, 7, 37, 0, "Squelch", 0 },
	{ GUI_TEXT, ID_SQL_VALUE, 48, 7, 12, 0, NULL, 0 },
	{ GUI_SLIDER, ID_GAIN, 10, 8, 37, 0, "RF gain", 0 },
	{ GUI_TEXT, ID_GAIN_VALUE, 48, 8, 12, 0, NULL, 0 },
	{ GUI_TEXT, ID_MSG, 0, 9, 60, 0, NULL, 0 },
};

static const struct GuiMenu menus[] =
{
	{ GUI_MENU_TITLE, 0, 0, "Project" },
	{ GUI_MENU_ITEM, 'Q', ID_QUIT, "Quit" },
	{ GUI_MENU_END, 0, 0, NULL }
};

static struct Gui *gui = NULL;
static struct RastPort *rp;
static int dx, dy, dw, dh;		/* the dial's area */
static int mx, my, mw, mh;		/* the meter's */

/* The dial is drawn here first */
static struct BitMap dial_bm;
static struct RastPort dial_rp;
static BOOL have_dial_bm = FALSE;
static struct TextFont *font = NULL;
static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };

/* The choices of the band and region cycle gadgets; two sets, so that a
   new list is always a new array */
static const char *band_labels[2][MAX_BANDS + 1], *region_labels[2][MAX_REGIONS + 1];
static int label_set = 0;
static char shown_bands[MAX_BANDS][32];
static int nshown_bands = -1, nshown_regions = -1;

static int shown_meter = -1;

/* ---------------------------------------------------------------------- */
/* The dial and the meter                                                 */

static int freq_to_x(LONG f)
{
	struct Band *b = &bands[cur_band];
	if (f < b->lo)
		f = b->lo;
	if (f > b->hi)
		f = b->hi;
	return 3 + (int)scale_long(f - b->lo, dw - 7, b->hi - b->lo);
}

/* The frequency at x in the dial */
static LONG x_to_freq(int x)
{
	struct Band *b = &bands[cur_band];
	LONG span = b->hi - b->lo, w = dw - 7;

	x -= 3;
	if (x < 0)
		x = 0;
	if (x > w)
		x = w;
	return b->lo + (span / w) * x + ((span % w) * x) / w;
}

/* A step between labelled ticks that gives at most 10 labels */
static LONG label_step(LONG span)
{
	LONG s = 1;
	for (;;)
	{
		if (span / s <= 10)
			return s;
		if (span / (s * 2) <= 10)
			return s * 2;
		if (span / (s * 5) <= 10)
			return s * 5;
		s *= 10;
	}
}

/* The next step of 1, 2, 5, 10, 20, 50, ... */
static LONG next_step(LONG s)
{
	LONG p = 1;
	while (s / p >= 10)
		p *= 10;
	if (s / p == 1)
		return 2 * p;
	if (s / p == 2)
		return 5 * p;
	return 10 * p;
}

/* The label of frequency f, with as many decimals as the step needs */
static void dial_label(char *buf, LONG f, LONG step)
{
	if (step >= 1000000L)
		sprintf(buf, "%ld", f / 1000000L);
	else if (step >= 100000L)
		sprintf(buf, "%ld.%ld", f / 1000000L, (f % 1000000L) / 100000L);
	else if (step >= 10000L)
		sprintf(buf, "%ld.%02ld", f / 1000000L, (f % 1000000L) / 10000L);
	else
		sprintf(buf, "%ld.%03ld", f / 1000000L, (f % 1000000L) / 1000L);
}

/* Where the label of f goes: its left edge. A label too near an end of
   the dial is moved inwards (*moved), and is then drawn only if there is
   room for it. */
static int label_x(struct RastPort *r, LONG f, LONG step, char *buf, int *w, BOOL *moved)
{
	int x;
	dial_label(buf, f, step);
	*w = TextLength(r, (STRPTR)buf, strlen(buf));
	x = freq_to_x(f) - *w / 2;
	*moved = TRUE;
	if (x < 2)
		x = 2;
	else if (x + *w > dw - 2)
		x = dw - 2 - *w;
	else
		*moved = FALSE;
	return x;
}

/* Whether the labels of this step, but for those moved inwards, fit
   without running into each other */
static BOOL labels_fit(struct RastPort *r, struct Band *b, LONG step)
{
	char buf[16];
	LONG f;
	int x, w, last_end = -100;
	BOOL moved;
	for (f = (b->lo + step - 1) / step * step; f <= b->hi; f += step)
	{
		x = label_x(r, f, step, buf, &w, &moved);
		if (moved)
			continue;
		if (x < last_end + 6)
			return FALSE;
		last_end = x + w;
	}
	return TRUE;
}

/* The labels: first those in place, then those at the ends that have
   room */
static void draw_labels(struct RastPort *r, int ox, int oy, struct Band *b, LONG step)
{
	char buf[16];
	LONG f;
	int pass, x, w, first = dw, last_end = -100;
	BOOL moved;
	SetAPen(r, 1);
	for (pass = 0; pass < 2; pass++)
		for (f = (b->lo + step - 1) / step * step; f <= b->hi; f += step)
		{
			x = label_x(r, f, step, buf, &w, &moved);
			if (moved != (pass == 1))
				continue;
			if (moved)
			{
				if (x + w + 6 > first && x < last_end + 6)
					continue;
				/* The one at the other end must not meet this one */
				if (x < first)
					first = x;
				else
					last_end = x + w;
			}
			else
			{
				if (x < first)
					first = x;
				last_end = x + w;
			}
			Move(r, ox + x, oy + 2 + r->TxBaseline);
			Text(r, (STRPTR)buf, strlen(buf));
		}
}

static void dial_fill(struct RastPort *r, int pen, int ox, int oy, int x0, int y0, int x1, int y1)
{
	/* The off-screen dial has no clipping: keep inside it */
	if (x0 < 0)
		x0 = 0;
	if (x1 > dw - 1)
		x1 = dw - 1;
	if (y0 < 0)
		y0 = 0;
	if (y1 > dh - 1)
		y1 = dh - 1;
	if (x1 < x0 || y1 < y0)
		return;
	SetAPen(r, pen);
	RectFill(r, ox + x0, oy + y0, ox + x1, oy + y1);
}

static void draw_dial(void)
{
	struct RastPort *r = have_dial_bm ? &dial_rp : rp;
	int ox = have_dial_bm ? 0 : dx, oy = have_dial_bm ? 0 : dy;
	struct Band *b = &bands[cur_band];
	LONG step, minor, f;
	int i, x;

	SetDrMd(r, JAM1);
	dial_fill(r, 0, ox, oy, 0, 0, dw - 1, dh - 1);

	if (nbands && have_state)
	{
		int base = dh - 4;
		/* The scale: long ticks with labels, short ticks between them */
		step = label_step(b->hi - b->lo);
		/* Fewer labels where they would not all fit */
		while (step < b->hi - b->lo && !labels_fit(r, b, step))
			step = next_step(step);
		minor = step >= 5 ? step / 5 : step;
		for (f = (b->lo + minor - 1) / minor * minor; f <= b->hi; f += minor)
		{
			x = freq_to_x(f);
			if (f % step != 0)
			{
				dial_fill(r, 1, ox, oy, x, base - 3, x, base);
				continue;
			}
			dial_fill(r, 1, ox, oy, x, base - 7, x, base);
		}
		if (2 + r->TxHeight < base - 8)
			draw_labels(r, ox, oy, b, step);
		dial_fill(r, 1, ox, oy, 3, base, dw - 4, base);

		/* Active channels found by the last sweep */
		for (i = 0; i < nactive; i++)
		{
			x = freq_to_x(b->lo + active[i] * b->step);
			dial_fill(r, 2, ox, oy, x - 1, base + 1, x + 1, base + 2);
		}

		/* The pointer */
		x = freq_to_x(freq);
		dial_fill(r, 3, ox, oy, x, 0, x + 1, dh - 1);
	}

	if (have_dial_bm)
		BltBitMapRastPort(&dial_bm, 0, 0, rp, dx, dy, dw, dh, 0xC0);
}

static void draw_meter(void)
{
	int w = level * mw / 50;

	if (w > mw)
		w = mw;
	if (w == shown_meter)
		return;
	if (shown_meter < 0)
	{
		SetAPen(rp, 0);
		RectFill(rp, mx, my, mx + mw - 1, my + mh - 1);
		shown_meter = 0;
	}
	/* Only the part that changed */
	if (w > shown_meter)
	{
		SetAPen(rp, 3);
		RectFill(rp, mx + shown_meter, my, mx + w - 1, my + mh - 1);
	}
	else if (w < shown_meter)
	{
		SetAPen(rp, 0);
		RectFill(rp, mx + w, my, mx + shown_meter - 1, my + mh - 1);
	}
	shown_meter = w;
}

/* ---------------------------------------------------------------------- */
/* The rest of the window                                                 */

static void show_freq(void)
{
	char buf[32], f[16];

	if (entry_len >= 0)
		sprintf(buf, "%s%c MHz", entry, (blink & 4) ? '_' : ' ');
	else if (have_state && nbands)
	{
		format_mhz(f, freq);
		sprintf(buf, "%s MHz", f);
	}
	else
		buf[0] = 0;
	gui_set_text(gui, ID_FREQ, buf);
}

static void show_station(void)
{
	static const char *modes[][2] =
	{
		{ "wbfm", "wide FM" }, { "fm", "narrow FM" }, { "am", "AM" },
		{ "usb", "USB" }, { "lsb", "LSB" }, { "raw", "raw" }
	};
	char buf[64];
	const char *mode = "";
	int i;

	if (!have_state || !nbands)
	{
		gui_set_text(gui, ID_STATION, "");
		return;
	}
	for (i = 0; i < 6; i++)
		if (strcmp(bands[cur_band].mode, modes[i][0]) == 0)
			mode = modes[i][1];
	sprintf(buf, "%s (%s)", bands[cur_band].name, mode);
	gui_set_text(gui, ID_STATION, buf);
}

/* What the tuner is doing, in place of the panel's lamps */
static void show_status(void)
{
	char buf[64];

	if (run_status == 1)
		strcpy(buf, level >= 2 ? "Receiving a signal" : "Receiving");
	else if (run_status == 0)
		strcpy(buf, "Starting the tuner...");
	else if (run_status == 3)
		strcpy(buf, "Sweeping the band...");
	else if (run_status == 2)
		strcpy(buf, "Tuner error");
	else
		buf[0] = 0;
	if (scan_dir)
		strcat(buf, ", scanning");
	if (pipe_status == 0)
		strcat(buf, ", no sound");
	gui_set_text(gui, ID_STATUS, buf);
}

static void show_bands(void)
{
	int i, set;
	BOOL same = nbands == nshown_bands && nregions == nshown_regions;

	for (i = 0; same && i < nbands; i++)
		same = strcmp(shown_bands[i], bands[i].name) == 0;
	if (!same)
	{
		/* A new list: in the other set of arrays */
		set = label_set ^ 1;
		for (i = 0; i < nbands; i++)
		{
			strcpy(shown_bands[i], bands[i].name);
			band_labels[set][i] = bands[i].name;
		}
		band_labels[set][nbands] = NULL;
		for (i = 0; i < nregions; i++)
			region_labels[set][i] = regions[i];
		region_labels[set][nregions] = NULL;
		label_set = set;
		nshown_bands = nbands;
		nshown_regions = nregions;
	}
	gui_set_cycle(gui, ID_BAND, band_labels[label_set], nbands, cur_band);
	gui_set_cycle(gui, ID_REGION, region_labels[label_set], nregions, cur_region);
}

static void show_values(int c)
{
	char buf[16];

	if (c & CH_VOL)
	{
		gui_set_slider(gui, ID_VOL, vol, 16);
		sprintf(buf, "%d%%", vol * 100 / 8);
		gui_set_text(gui, ID_VOL_VALUE, buf);
	}
	if (c & CH_SQL)
	{
		gui_set_slider(gui, ID_SQL, sql / SQL_STEP, SQL_MAX / SQL_STEP);
		if (sql == 0)
			strcpy(buf, "Off");
		else
			sprintf(buf, "%d", sql);
		gui_set_text(gui, ID_SQL_VALUE, buf);
	}
	if (c & CH_GAIN)
	{
		/* Until the Pi has sent its gains there is nothing to choose; the
		   knob is then of an ordinary size rather than the whole slider */
		gui_set_slider(gui, ID_GAIN, gain_index(), ngains ? ngains : 10);
		gui_disable(gui, ID_GAIN, ngains == 0);
		if (gain < 0)
			strcpy(buf, "Automatic");
		else
			sprintf(buf, "%d.%d dB", gain / 10, gain % 10);
		gui_set_text(gui, ID_GAIN_VALUE, buf);
	}
}

static void refresh(void)
{
	int c = changed;

	changed = 0;
	if (!c)
		return;
	if (c & (CH_FREQ | CH_DIAL | CH_BANDS))
		show_freq();
	if (c & (CH_DIAL | CH_BANDS))
		show_station();
	if (c & (CH_FREQ | CH_DIAL))
		draw_dial();
	if (c & CH_LEVEL)
		draw_meter();
	if (c & (CH_LAMPS | CH_SCAN | CH_LEVEL))
		show_status();
	if (c & CH_BANDS)
		show_bands();
	if (c & (CH_VOL | CH_SQL | CH_GAIN))
		show_values(c);
	if (c & CH_MSG)
		gui_set_text(gui, ID_MSG, msg);
}

/* ---------------------------------------------------------------------- */
/* Input                                                                  */

static void handle(void)
{
	struct GuiEvent ev;

	while (gui_event(gui, &ev))
	{
		switch (ev.type)
		{
		case GUI_EV_CLOSE:
			quit = TRUE;
			break;
		case GUI_EV_KEY:
			handle_char((char)ev.value, 0);
			break;
		case GUI_EV_RAWKEY:
			handle_rawkey((UWORD)ev.value, (UWORD)(ev.value >> 16), NULL);
			break;
		case GUI_EV_MENU:
			if (ev.id == ID_QUIT)
				quit = TRUE;
			break;
		case GUI_EV_GADGET:
			switch (ev.id)
			{
			case ID_DIAL:
				tune_to(x_to_freq((int)(ev.value >> 16)));
				break;
			case ID_BAND:
				select_band((int)ev.value);
				break;
			case ID_REGION:
				select_region((int)ev.value);
				break;
			case ID_SEEK_DOWN:
				do_seek(-1);
				break;
			case ID_SEEK_UP:
				do_seek(1);
				break;
			case ID_SCAN:
				toggle_scan(1);
				break;
			case ID_SWEEP:
				do_sweep();
				break;
			case ID_VOL:
				set_vol((int)ev.value);
				break;
			case ID_SQL:
				set_sql((int)ev.value * SQL_STEP);
				break;
			case ID_GAIN:
				set_gain_index((int)ev.value);
				break;
			}
			/* What couldn't be done (e.g. while not connected) is shown
			   as it is again */
			changed |= CH_BANDS | CH_VOL | CH_SQL | CH_GAIN;
			break;
		}
	}
}

static void tick(void)
{
	if (entry_len >= 0 && (blink & 3) == 0)
		changed |= CH_FREQ;	/* the cursor blinks */
}

/* ---------------------------------------------------------------------- */

static void free_dial_bm(void)
{
	int i;
	for (i = 0; i < dial_bm.Depth; i++)
		if (dial_bm.Planes[i])
		{
			FreeRaster(dial_bm.Planes[i], dw, dh);
			dial_bm.Planes[i] = NULL;
		}
	have_dial_bm = FALSE;
}

static void alloc_dial_bm(void)
{
	int i, depth = rp->BitMap->Depth;

	if (depth > 8 || dw <= 0 || dh <= 0)
		return;
	InitBitMap(&dial_bm, depth, dw, dh);
	for (i = 0; i < 8; i++)
		dial_bm.Planes[i] = NULL;
	for (i = 0; i < depth; i++)
		if (!(dial_bm.Planes[i] = AllocRaster(dw, dh)))
		{
			free_dial_bm();
			return;	/* then it is drawn straight into the window */
		}
	InitRastPort(&dial_rp);
	dial_rp.BitMap = &dial_bm;
	if (font)
		SetFont(&dial_rp, font);
	have_dial_bm = TRUE;
}

static void close_window(void)
{
	gui_close(gui);
	gui = NULL;
	free_dial_bm();
	if (font)
		CloseFont(font);
	font = NULL;
}

static BOOL open_window(void)
{
	gui = gui_open("PiRadio", items, sizeof(items) / sizeof(items[0]), menus);
	if (!gui)
	{
		report("Unable to open the window");
		return FALSE;
	}
	rp = gui_window(gui)->RPort;
	font = OpenFont(&topaz8);	/* the dial's labels: small and plain */
	gui_area(gui, ID_DIAL, &dx, &dy, &dw, &dh);
	gui_area(gui, ID_METER, &mx, &my, &mw, &mh);
	alloc_dial_bm();
	if (font && !have_dial_bm)
		SetFont(rp, font);
	gui_set_text(gui, ID_LEVEL_LABEL, "Level");
	shown_meter = -1;
	nshown_bands = nshown_regions = -1;
	changed = CH_ALL;
	return TRUE;
}

static ULONG signals(void)
{
	return gui_signal(gui);
}

static struct Window *window(void)
{
	return gui_window(gui);
}

const struct Display window_display =
{
	open_window, close_window, signals, handle, refresh, tick, NULL, NULL, window
};
