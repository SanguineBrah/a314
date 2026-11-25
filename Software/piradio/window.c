/*
 * PiRadio - the radio in a window on the Workbench (the WINDOW ToolType).
 *
 * Uses the Workbench's own colours and font: a dial, band buttons and
 * sliders. To avoid flicker, nothing is cleared before it is redrawn: text
 * is drawn with its background, and the dial is drawn off screen and then
 * copied into the window in one go.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <string.h>

#include "piradio.h"

/* Layout, inside the window borders */
#define INNER_W		600
#define INNER_H		146
#define DIAL_X		8
#define DIAL_Y		16
#define DIAL_W		584
#define DIAL_H		40
#define METER_X		56
#define METER_Y		62
#define METER_W		200
#define BAND_Y		76
#define BAND_W		56
#define BUTTON_Y	96
#define BUTTON_W	72
#define BUTTON_H	14
#define SLIDER_Y	116
#define SLIDER_W	112
#define MSG_Y		138
#define READOUT_CHARS	43	/* 344 pixels */
#define MSG_CHARS	75	/* 600 pixels */

#define GID_BAND	0	/* 0 .. MAX_BANDS-1 */
#define GID_SEEK_DOWN	20
#define GID_SEEK_UP	21
#define GID_SCAN	22
#define GID_SWEEP	23
#define GID_REGION	24
#define GID_VOL		30
#define GID_SQL		31
#define GID_GAIN	32

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };
static struct TextFont *font = NULL;
static struct Window *win = NULL;
static struct RastPort *rp;
static int ox, oy;		/* top left corner inside the window borders */

/* The dial is drawn here first */
static struct BitMap dial_bm;
static struct RastPort dial_rp;
static BOOL have_dial_bm = FALSE;

static BOOL dragging_dial = FALSE;
static struct Gadget *active_slider = NULL;

/* What is on screen, so that only changes are drawn */
static int shown_lamp[4] = { -1, -1, -1, -1 };
static int shown_meter = -1;
static int shown_band = -1;
static char built_bands[MAX_BANDS][8];
static int nbuilt = -1;

/* ---------------------------------------------------------------------- */
/* Gadgets                                                                */

static SHORT band_xy[] = { 0,0, BAND_W-1,0, BAND_W-1,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border band_border = { 0, 0, 1, 0, JAM1, 5, band_xy, NULL };
static SHORT button_xy[] = { 0,0, BUTTON_W-1,0, BUTTON_W-1,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border button_border = { 0, 0, 1, 0, JAM1, 5, button_xy, NULL };

static struct Gadget band_gadgets[MAX_BANDS];
static struct IntuiText band_texts[MAX_BANDS];
static int nband_gadgets = 0;

#define NBUTTONS	5
static const char *button_labels[NBUTTONS] = { "<< SEEK", "SEEK >>", "SCAN", "SWEEP", "REGION" };
static struct Gadget buttons[NBUTTONS];
static struct IntuiText button_texts[NBUTTONS];

static const char *slider_labels[3] = { "VOL", "SQL", "GAIN" };
static struct Gadget sliders[3];
static struct PropInfo slider_info[3];
static struct Image slider_knobs[3];

static void init_text(struct IntuiText *t, const char *s, int width)
{
	memset(t, 0, sizeof(*t));
	t->FrontPen = 1;
	t->DrawMode = JAM1;
	t->LeftEdge = (width - 8 * (int)strlen(s)) / 2;
	t->TopEdge = 3;
	t->ITextFont = &topaz8;	/* not the screen's font, which may be wider */
	t->IText = (UBYTE *)s;
}

static void init_gadgets(void)
{
	int i;

	for (i = 0; i < NBUTTONS; i++)
	{
		struct Gadget *g = &buttons[i];
		init_text(&button_texts[i], button_labels[i], BUTTON_W);
		memset(g, 0, sizeof(*g));
		g->LeftEdge = ox + 8 + i * (BUTTON_W + 8);
		g->TopEdge = oy + BUTTON_Y;
		g->Width = BUTTON_W;
		g->Height = BUTTON_H;
		g->Flags = GFLG_GADGHCOMP;
		g->Activation = GACT_RELVERIFY;
		g->GadgetType = GTYP_BOOLGADGET;
		g->GadgetRender = &button_border;
		g->GadgetText = &button_texts[i];
		g->GadgetID = GID_SEEK_DOWN + i;
	}

	for (i = 0; i < 3; i++)
	{
		struct Gadget *g = &sliders[i];
		memset(g, 0, sizeof(*g));
		g->LeftEdge = ox + 40 + i * 196;
		g->TopEdge = oy + SLIDER_Y;
		g->Width = SLIDER_W;
		g->Height = 10;
		g->Flags = GFLG_GADGHCOMP;
		g->Activation = GACT_IMMEDIATE | GACT_RELVERIFY | GACT_FOLLOWMOUSE;
		g->GadgetType = GTYP_PROPGADGET;
		g->GadgetRender = &slider_knobs[i];
		g->SpecialInfo = &slider_info[i];
		g->GadgetID = GID_VOL + i;
		slider_info[i].Flags = AUTOKNOB | FREEHORIZ | PROPNEWLOOK;
		slider_info[i].HorizBody = MAXBODY / 8;
		slider_info[i].VertBody = MAXBODY;
	}
}

/* Number of positions of a slider, and its position now */
static int slider_steps(int i)
{
	return i == 0 ? 17 : i == 1 ? SQL_MAX / SQL_STEP + 1 : ngains + 1;
}

static int slider_pos(int i)
{
	return i == 0 ? vol : i == 1 ? sql / SQL_STEP : gain_index();
}

/* Moves a slider to its value. Intuition redraws the slider (and the ones
   after it), so this is only done when its position really changes. */
static void update_slider(int i)
{
	int n = slider_steps(i);
	UWORD pot = n > 1 ? (UWORD)((ULONG)slider_pos(i) * MAXPOT / (n - 1)) : 0;
	UWORD body = n > 1 ? (UWORD)(MAXBODY / n) : MAXBODY;
	if (slider_info[i].HorizPot == pot && slider_info[i].HorizBody == body)
		return;
	ModifyProp(&sliders[i], win, NULL, slider_info[i].Flags, pot, 0, body, MAXBODY);
}

/* Takes the value of a slider the user is moving */
static void read_slider(struct Gadget *g)
{
	int i = g->GadgetID - GID_VOL;
	int n = slider_steps(i);
	int pos = n > 1 ? (int)(((ULONG)slider_info[i].HorizPot * (n - 1) + MAXPOT / 2) / MAXPOT) : 0;

	if (i == 0)
		set_vol(pos);
	else if (i == 1)
		set_sql(pos * SQL_STEP);
	else
		set_gain_index(pos);
}

static BOOL band_list_changed(void)
{
	int i;
	if (nbands != nbuilt)
		return TRUE;
	for (i = 0; i < nbands; i++)
		if (strcmp(built_bands[i], bands[i].shortname) != 0)
			return TRUE;
	return FALSE;
}

/* Replaces the band buttons with one per band the Pi has sent. */
static void rebuild_band_buttons(void)
{
	struct Gadget *g;
	int i, pos;

	for (i = 0; i < nband_gadgets; i++)
		RemoveGadget(win, &band_gadgets[i]);
	SetAPen(rp, 0);
	RectFill(rp, ox, oy + BAND_Y, ox + INNER_W - 1, oy + BAND_Y + BUTTON_H + 2);

	/* The band buttons go before the sliders, so that moving a slider
	   (which redraws the gadgets after it) doesn't redraw them. */
	for (pos = 0, g = win->FirstGadget; g && g != &sliders[0]; g = g->NextGadget)
		pos++;

	nband_gadgets = nbands;
	for (i = 0; i < nbands; i++)
	{
		strcpy(built_bands[i], bands[i].shortname);
		g = &band_gadgets[i];
		init_text(&band_texts[i], built_bands[i], BAND_W);
		memset(g, 0, sizeof(*g));
		g->LeftEdge = ox + 8 + i * (BAND_W + 2);
		g->TopEdge = oy + BAND_Y;
		g->Width = BAND_W;
		g->Height = BUTTON_H;
		g->Flags = GFLG_GADGHCOMP;
		g->Activation = GACT_RELVERIFY;
		g->GadgetType = GTYP_BOOLGADGET;
		g->GadgetRender = &band_border;
		g->GadgetText = &band_texts[i];
		g->GadgetID = GID_BAND + i;
		AddGadget(win, g, pos + i);
	}
	nbuilt = nbands;
	if (nbands)
		RefreshGadgets(&band_gadgets[0], win, NULL);
	shown_band = -1;
}

/* ---------------------------------------------------------------------- */
/* Drawing                                                                */

/* Text with its background, padded with spaces to n characters, so that
   it replaces what was there without clearing it first */
static void text_field(struct RastPort *r, int x, int y, int pen, const char *s, int n)
{
	static const char spaces[] = "                                                                                ";
	int len = strlen(s);

	if (len > n)
		len = n;
	SetAPen(r, pen);
	SetBPen(r, 0);
	SetDrMd(r, JAM2);
	Move(r, x, y + font->tf_Baseline);
	Text(r, (STRPTR)s, len);
	if (n > len)
		Text(r, (STRPTR)spaces, n - len < sizeof(spaces) - 1 ? n - len : sizeof(spaces) - 1);
	SetDrMd(r, JAM1);
}

static void draw_readout(void)
{
	char buf[64], f[16];

	if (entry_len >= 0)
		sprintf(buf, "%s%c MHz", entry, (blink & 4) ? '_' : ' ');
	else if (have_state && nbands)
	{
		format_mhz(f, freq);
		sprintf(buf, "%9s MHz  %s %s", f, bands[cur_band].mode, bands[cur_band].name);
	}
	else
		buf[0] = 0;
	text_field(rp, ox, oy + 2, entry_len >= 0 ? 3 : 1, buf, READOUT_CHARS);
}

static int lamp_state(int i)
{
	switch (i)
	{
	case 0:		/* TUNER: on when receiving, flashing when starting */
		return run_status == 1 || ((run_status == 0 || run_status == 3) && (blink & 2));
	case 1:		/* SIGNAL */
		return run_status == 1 && level >= 2;
	case 2:		/* AUDIO: the PiAudio pipe is open */
		return pipe_status == 1;
	default:
		return scan_dir != 0;
	}
}

static void draw_lamps(void)
{
	static const char *labels[4] = { "TUNER", "SIGNAL", "AUDIO", "SCAN" };
	int i;

	for (i = 0; i < 4; i++)
	{
		int x = ox + 344 + 8 + i * 64, on = lamp_state(i);
		if (on == shown_lamp[i])
			continue;
		if (shown_lamp[i] < 0)
		{
			SetAPen(rp, 1);
			Move(rp, x, oy + 3);
			Draw(rp, x + 9, oy + 3);
			Draw(rp, x + 9, oy + 10);
			Draw(rp, x, oy + 10);
			Draw(rp, x, oy + 3);
			text_field(rp, x + 14, oy + 2, 1, labels[i], 6);
		}
		SetAPen(rp, on ? 3 : 0);
		RectFill(rp, x + 1, oy + 4, x + 8, oy + 9);
		shown_lamp[i] = on;
	}
}

static int freq_to_x(LONG f)
{
	struct Band *b = &bands[cur_band];
	if (f < b->lo)
		f = b->lo;
	if (f > b->hi)
		f = b->hi;
	return 2 + (int)scale_long(f - b->lo, DIAL_W - 5, b->hi - b->lo);
}

/* The frequency at dial position x (relative to the dial) */
static LONG x_to_freq(int x)
{
	struct Band *b = &bands[cur_band];
	LONG span = b->hi - b->lo, w = DIAL_W - 5;

	x -= 2;
	if (x < 0)
		x = 0;
	if (x > w)
		x = w;
	return b->lo + (span / w) * x + ((span % w) * x) / w;
}

/* A step between labelled ticks that gives about 8 labels */
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

static void dial_line(struct RastPort *r, int pen, int x0, int y0, int x1, int y1)
{
	/* The off-screen dial has no clipping */
	if (x0 < 0 || x1 >= DIAL_W || y0 < 0 || y1 >= DIAL_H)
		return;
	SetAPen(r, pen);
	Move(r, x0, y0);
	Draw(r, x1, y1);
}

static void dial_fill(struct RastPort *r, int pen, int x0, int y0, int x1, int y1)
{
	if (x0 < 0)
		x0 = 0;
	if (x1 > DIAL_W - 1)
		x1 = DIAL_W - 1;
	if (y0 < 0)
		y0 = 0;
	if (y1 > DIAL_H - 1)
		y1 = DIAL_H - 1;
	if (x1 < x0 || y1 < y0)
		return;
	SetAPen(r, pen);
	RectFill(r, x0, y0, x1, y1);
}

/* Draws the dial: off screen if there was memory for it, so that it is
   never seen half drawn */
static void draw_dial(void)
{
	struct RastPort *r = have_dial_bm ? &dial_rp : rp;
	int dx = have_dial_bm ? 0 : ox + DIAL_X, dy = have_dial_bm ? 0 : oy + DIAL_Y;
	struct Band *b = &bands[cur_band];
	LONG step, minor, f, first;
	char buf[16];
	int i, x;

	SetDrMd(r, JAM1);
	SetAPen(r, 0);
	RectFill(r, dx, dy, dx + DIAL_W - 1, dy + DIAL_H - 1);
	SetAPen(r, 1);
	Move(r, dx, dy);
	Draw(r, dx + DIAL_W - 1, dy);
	Draw(r, dx + DIAL_W - 1, dy + DIAL_H - 1);
	Draw(r, dx, dy + DIAL_H - 1);
	Draw(r, dx, dy);

	if (nbands && have_state)
	{
		/* Scale: long ticks with labels, five short ticks between them */
		step = label_step(b->hi - b->lo);
		minor = step >= 5 ? step / 5 : step;
		first = (b->lo + minor - 1) / minor * minor;
		for (f = first; f <= b->hi; f += minor)
		{
			x = freq_to_x(f);
			if (f % step == 0)
			{
				int n;
				dial_line(r, 1, dx + x, dy + DIAL_H - 12, dx + x, dy + DIAL_H - 3);
				/* The label, in MHz with as many decimals as needed */
				if (step >= 1000000L)
					sprintf(buf, "%ld", f / 1000000L);
				else if (step >= 100000L)
					sprintf(buf, "%ld.%ld", f / 1000000L, (f % 1000000L) / 100000L);
				else if (step >= 10000L)
					sprintf(buf, "%ld.%02ld", f / 1000000L, (f % 1000000L) / 10000L);
				else
					sprintf(buf, "%ld.%03ld", f / 1000000L, (f % 1000000L) / 1000L);
				n = strlen(buf);
				x -= n * 4;
				if (x < 2)
					x = 2;
				if (x + n * 8 > DIAL_W - 2)
					x = DIAL_W - 2 - n * 8;
				SetAPen(r, 1);
				Move(r, dx + x, dy + 3 + font->tf_Baseline);
				Text(r, (STRPTR)buf, n);
			}
			else
				dial_line(r, 1, dx + x, dy + DIAL_H - 7, dx + x, dy + DIAL_H - 3);
		}

		/* Active channels found by the last sweep */
		for (i = 0; i < nactive; i++)
		{
			x = freq_to_x(b->lo + active[i] * b->step);
			dial_fill(r, 2, dx + x - 1, dy + DIAL_H - 16, dx + x + 1, dy + DIAL_H - 13);
		}

		/* The pointer */
		x = freq_to_x(freq);
		dial_fill(r, 3, dx + x, dy + 1, dx + x + 1, dy + DIAL_H - 2);
	}

	if (have_dial_bm)
		BltBitMapRastPort(&dial_bm, 0, 0, rp, ox + DIAL_X, oy + DIAL_Y, DIAL_W, DIAL_H, 0xC0);
}

static void draw_meter(void)
{
	int w = level * 2;

	if (w > METER_W)
		w = METER_W;
	if (w == shown_meter)
		return;
	if (shown_meter < 0)
		text_field(rp, ox, oy + METER_Y - 1, 1, "LEVEL", 6);
	/* Only the part that changed */
	if (w > shown_meter)
	{
		SetAPen(rp, 3);
		RectFill(rp, ox + METER_X + (shown_meter < 0 ? 0 : shown_meter), oy + METER_Y,
			ox + METER_X + w - 1, oy + METER_Y + 5);
	}
	else
	{
		SetAPen(rp, 0);
		RectFill(rp, ox + METER_X + w, oy + METER_Y, ox + METER_X + shown_meter - 1, oy + METER_Y + 5);
	}
	shown_meter = w;
}

static void draw_value(int i)
{
	char buf[12];

	if (i == 0)
		sprintf(buf, "%d%%", vol * 100 / 8);
	else if (i == 1)
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
	text_field(rp, ox + 40 + i * 196 + SLIDER_W + 8, oy + SLIDER_Y + 1, 1, buf, 4);
}

/* The current band's button is underlined, and the region named */
static void draw_bands(void)
{
	char buf[48];

	if (band_list_changed())
		rebuild_band_buttons();
	if (cur_band != shown_band)
	{
		if (shown_band >= 0 && shown_band < nbands)
		{
			SetAPen(rp, 0);
			RectFill(rp, ox + 8 + shown_band * (BAND_W + 2), oy + BAND_Y + BUTTON_H + 1,
				ox + 8 + shown_band * (BAND_W + 2) + BAND_W - 1, oy + BAND_Y + BUTTON_H + 2);
		}
		if (cur_band < nbands && have_state)
		{
			SetAPen(rp, 3);
			RectFill(rp, ox + 8 + cur_band * (BAND_W + 2), oy + BAND_Y + BUTTON_H + 1,
				ox + 8 + cur_band * (BAND_W + 2) + BAND_W - 1, oy + BAND_Y + BUTTON_H + 2);
			shown_band = cur_band;
		}
		else
			shown_band = -1;
	}
	if (nregions > 0 && have_state)
		sprintf(buf, "REGION: %s", regions[cur_region]);
	else
		buf[0] = 0;
	text_field(rp, ox + 272, oy + METER_Y - 1, 1, buf, 41);
}

static void draw_msg(void)
{
	text_field(rp, ox, oy + MSG_Y, 1, msg, MSG_CHARS);
}

static void refresh(void)
{
	int c = changed, i;

	changed = 0;
	if (!c)
		return;
	if (c & (CH_BANDS | CH_DIAL | CH_FREQ))
		draw_readout();
	if (c & CH_LAMPS)
		draw_lamps();
	if (c & (CH_DIAL | CH_FREQ))
		draw_dial();
	if (c & CH_LEVEL)
		draw_meter();
	for (i = 0; i < 3; i++)
	{
		if (c & (CH_VOL << i))
		{
			if (&sliders[i] != active_slider)
				update_slider(i);
			draw_value(i);
		}
	}
	if (c & CH_BANDS)
		draw_bands();
	if (c & CH_MSG)
		draw_msg();
}

/* ---------------------------------------------------------------------- */
/* Input                                                                  */

static void handle_gadget(struct Gadget *g, BOOL up)
{
	int id = g->GadgetID;

	if (id >= GID_VOL)
	{
		read_slider(g);
		active_slider = up ? NULL : g;
		ui_dragging = active_slider != NULL || dragging_dial;
		if (up)
			changed |= CH_VOL << (id - GID_VOL);
		return;
	}
	if (!up)
		return;
	if (id < MAX_BANDS)
		select_band(id);
	else if (id == GID_SEEK_DOWN)
		do_seek(-1);
	else if (id == GID_SEEK_UP)
		do_seek(1);
	else if (id == GID_SCAN)
		toggle_scan(1);
	else if (id == GID_SWEEP)
		do_sweep();
	else if (id == GID_REGION)
		next_region();
}

static BOOL in_dial(int x, int y)
{
	x -= ox;
	y -= oy;
	return x >= DIAL_X && x < DIAL_X + DIAL_W && y >= DIAL_Y && y < DIAL_Y + DIAL_H;
}

static void stop_dragging(void)
{
	if (dragging_dial)
	{
		dragging_dial = FALSE;
		ReportMouse(FALSE, win);
	}
	ui_dragging = active_slider != NULL;
}

static void handle(void)
{
	struct IntuiMessage *im;

	while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL)
	{
		ULONG cls = im->Class;
		UWORD code = im->Code, qual = im->Qualifier;
		int mx = im->MouseX, my = im->MouseY;
		APTR addr = im->IAddress;
		/* For dead keys, RawKeyConvert() needs the previous keys */
		APTR prev = cls == IDCMP_RAWKEY && addr ? *(APTR *)addr : NULL;
		ReplyMsg((struct Message *)im);

		switch (cls)
		{
		case IDCMP_CLOSEWINDOW:
			quit = TRUE;
			break;
		case IDCMP_GADGETDOWN:
			handle_gadget((struct Gadget *)addr, FALSE);
			break;
		case IDCMP_GADGETUP:
			handle_gadget((struct Gadget *)addr, TRUE);
			break;
		case IDCMP_MOUSEBUTTONS:
			if (code == SELECTDOWN && in_dial(mx, my) && can_tune())
			{
				dragging_dial = ui_dragging = TRUE;
				ReportMouse(TRUE, win);
				tune_to(x_to_freq(mx - ox - DIAL_X));
			}
			else if (code == SELECTUP)
				stop_dragging();
			break;
		case IDCMP_MOUSEMOVE:
			if (dragging_dial)
				tune_to(x_to_freq(mx - ox - DIAL_X));
			else if (active_slider)
				read_slider(active_slider);
			break;
		case IDCMP_INACTIVEWINDOW:
			stop_dragging();
			break;
		case IDCMP_RAWKEY:
			handle_rawkey(code, qual, prev);
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
			FreeRaster(dial_bm.Planes[i], DIAL_W, DIAL_H);
			dial_bm.Planes[i] = NULL;
		}
	have_dial_bm = FALSE;
}

static void alloc_dial_bm(void)
{
	int i, depth = rp->BitMap->Depth;

	if (depth > 8)
		return;
	InitBitMap(&dial_bm, depth, DIAL_W, DIAL_H);
	for (i = 0; i < 8; i++)
		dial_bm.Planes[i] = NULL;
	for (i = 0; i < depth; i++)
		if (!(dial_bm.Planes[i] = AllocRaster(DIAL_W, DIAL_H)))
		{
			free_dial_bm();
			return;	/* then it is drawn straight into the window */
		}
	InitRastPort(&dial_rp);
	dial_rp.BitMap = &dial_bm;
	SetFont(&dial_rp, font);
	have_dial_bm = TRUE;
}

static void close_window(void)
{
	if (win)
		CloseWindow(win);
	win = NULL;
	free_dial_bm();
	if (font)
		CloseFont(font);
	font = NULL;
}

static BOOL open_window(void)
{
	struct Screen wb;
	struct NewWindow nw;
	int i, bt;

	font = OpenFont(&topaz8);
	if (!font || !GetScreenData((APTR)&wb, sizeof(wb), WBENCHSCREEN, NULL))
	{
		report("Unable to open the window");
		return FALSE;
	}

	/* Expected size of the title bar. The gadgets are placed using the
	   real border sizes once the window is open. */
	bt = wb.WBorTop + wb.Font->ta_YSize + 1;

	memset(&nw, 0, sizeof(nw));
	nw.Width = INNER_W + wb.WBorLeft + wb.WBorRight;
	nw.Height = INNER_H + bt + wb.WBorBottom;
	nw.LeftEdge = (wb.Width - nw.Width) / 2;
	nw.TopEdge = wb.BarHeight + 2;
	if (nw.LeftEdge < 0 || nw.TopEdge + nw.Height > wb.Height)
		nw.LeftEdge = nw.TopEdge = 0;
	nw.DetailPen = 0;
	nw.BlockPen = 1;
	nw.IDCMPFlags = IDCMP_CLOSEWINDOW | IDCMP_GADGETDOWN | IDCMP_GADGETUP |
		IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY | IDCMP_INACTIVEWINDOW;
	nw.Flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
		WFLG_SMART_REFRESH | WFLG_NOCAREREFRESH | WFLG_ACTIVATE | WFLG_RMBTRAP;
	nw.Title = (UBYTE *)"PiRadio";
	nw.Type = WBENCHSCREEN;

	win = OpenWindow(&nw);
	if (!win)
	{
		close_window();
		report("Unable to open the window");
		return FALSE;
	}
	rp = win->RPort;
	SetFont(rp, font);
	SetDrMd(rp, JAM1);
	alloc_dial_bm();

	ox = win->BorderLeft;
	oy = win->BorderTop;
	init_gadgets();
	for (i = 0; i < NBUTTONS; i++)
		AddGadget(win, &buttons[i], -1);
	for (i = 0; i < 3; i++)
		AddGadget(win, &sliders[i], -1);
	RefreshGadgets(win->FirstGadget, win, NULL);
	for (i = 0; i < 3; i++)
		text_field(rp, ox + 40 + i * 196 - 8 * (int)strlen(slider_labels[i]) - 8,
			oy + SLIDER_Y + 1, 1, slider_labels[i], strlen(slider_labels[i]));
	changed = CH_ALL;
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

const struct Display window_display =
{
	open_window, close_window, signals, handle, refresh, tick, NULL, NULL, window
};
