/*
 * PiRadio - tune an RTL-SDR dongle on the Raspberry Pi from the Workbench.
 *
 * Opens a window on the Workbench screen with a tuning dial, band buttons
 * and sliders, and talks to the piradio service on the Pi over A314. The
 * sound is played by PiAudio, which must be running as well.
 *
 * Only uses Kickstart/Workbench 1.3 functions.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <devices/inputevent.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/dos.h>

#include <proto/alib.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/console.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../a314device/a314.h"

#define SERVICE_NAME	"piradio"
#define PORT_NAME	"PiRadio"

#define MAX_BANDS	10
#define MAX_GAINS	40
#define MAX_ACTIVE	200
#define SQL_MAX		200

/* Timing, in ticks of 1/10 s */
#define RETRY_TICKS		30	/* pause before reconnecting */
#define CONNECT_TIMEOUT_TICKS	50
#define RX_TIMEOUT_TICKS	80	/* the service sends HB every 2 s */
#define SHUTDOWN_TIMEOUT_TICKS	20

/* Connection states */
#define CS_DOWN		0
#define CS_CONNECTING	1
#define CS_UP		2
#define CS_CLOSING	3

/* Parts of the window to redraw */
#define D_READOUT	0x01
#define D_DIAL		0x02
#define D_METER		0x04
#define D_LAMPS		0x08
#define D_MSG		0x10
#define D_VALUES	0x20
#define D_SLIDERS	0x40
#define D_BANDS		0x80
#define D_ALL		0xFF

/* Settings changed here that still have to be sent to the Pi */
#define OUT_FREQ	1
#define OUT_SQL		2
#define OUT_GAIN	4
#define OUT_VOL		8

/* Layout, relative to the inside of the window borders */
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

/* Gadget IDs */
#define GID_BAND	0	/* 0 .. MAX_BANDS-1 */
#define GID_SEEK_DOWN	20
#define GID_SEEK_UP	21
#define GID_SCAN	22
#define GID_SWEEP	23
#define GID_VOL		30
#define GID_SQL		31
#define GID_GAIN	32

/* Raw key codes */
#define RAW_ESC		0x45
#define RAW_UP		0x4C
#define RAW_DOWN	0x4D
#define RAW_RIGHT	0x4E
#define RAW_LEFT	0x4F
#define RAW_F1		0x50
#define RAW_F10		0x59

struct Band
{
	LONG lo, hi, step;
	char mode[6];
	char shortname[8];
	char name[32];
};

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;

/* The name the vbcc inline for RawKeyConvert() expects */
struct Device *ConsoleDevice = NULL;
static struct IOStdReq console_req;

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };
static struct TextFont *font = NULL;
static struct Window *win = NULL;
static struct RastPort *rp;
static int ox, oy;		/* top left corner inside the window borders */

static struct MsgPort *mp = NULL;
static struct A314_IORequest *cmsg = NULL, *rmsg = NULL, *wmsg = NULL, *xmsg = NULL;
static BOOL a314_open = FALSE;
static BOOL pending_connect = FALSE, pending_read = FALSE;
static BOOL pending_write = FALSE, pending_reset = FALSE;
static ULONG socket;
static int socket_seq = 0;
static int conn_state = CS_DOWN;
static int conn_ticks = 0;
static int rx_idle_ticks = 0;
static UBYTE arbuf[256];
static UBYTE awbuf[256];
static char linebuf[512];
static int linelen = 0;
static char cmdq[160];		/* commands waiting to be sent */
static int cmdq_len = 0;
static int out_dirty = 0;

static struct MsgPort *timer_mp = NULL;
static struct timerequest *timer_req = NULL;
static BOOL timer_open = FALSE, timer_pending = FALSE;

/* What is known about the radio */
static struct Band bands[MAX_BANDS];
static int nbands = 0;
static WORD gains[MAX_GAINS];
static int ngains = 0;
static LONG active[MAX_ACTIVE];		/* active channels of the current band */
static int nactive = 0;
static int cur_band = 0;
static LONG freq = 0;
static int sql = 0, gain = -1, vol = 8, scan_dir = 0;
static int run_status = -1, pipe_status = -1, level = 0;
static BOOL have_state = FALSE;
static char msg[80] = "";

/* User interface state */
static int dirty = D_ALL;
static BOOL quit = FALSE;
static BOOL dragging_dial = FALSE;
static struct Gadget *active_slider = NULL;
static char entry[12];		/* frequency being typed, in MHz */
static int entry_len = -1;	/* -1 when not typing */
static int blink = 0;

/* ---------------------------------------------------------------------- */
/* Gadgets                                                                */

static SHORT band_xy[] = { 0,0, BAND_W-1,0, BAND_W-1,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border band_border = { 0, 0, 1, 0, JAM1, 5, band_xy, NULL };
static SHORT button_xy[] = { 0,0, BUTTON_W-1,0, BUTTON_W-1,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border button_border = { 0, 0, 1, 0, JAM1, 5, button_xy, NULL };

static struct Gadget band_gadgets[MAX_BANDS];
static struct IntuiText band_texts[MAX_BANDS];
static int nband_gadgets = 0;

static const char *button_labels[4] = { "<< SEEK", "SEEK >>", "SCAN", "SWEEP" };
static struct Gadget buttons[4];
static struct IntuiText button_texts[4];

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

	for (i = 0; i < 4; i++)
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

/* Number of positions of a slider, and its value at each position */
static int slider_steps(int i)
{
	if (i == 0)
		return 17;
	if (i == 1)
		return SQL_MAX + 1;
	return ngains + 1;
}

static int slider_value(int i)
{
	if (i == 0)
		return vol;
	if (i == 1)
		return sql;
	if (gain < 0)
		return 0;
	{
		/* The position of the nearest gain step */
		int j, best = 0;
		LONG bestd = 0x7fffffffL;
		for (j = 0; j < ngains; j++)
		{
			LONG d = gains[j] > gain ? gains[j] - gain : gain - gains[j];
			if (d < bestd)
			{
				bestd = d;
				best = j + 1;
			}
		}
		return best;
	}
}

static void update_slider(int i)
{
	int n = slider_steps(i);
	UWORD pot = n > 1 ? (UWORD)((ULONG)slider_value(i) * MAXPOT / (n - 1)) : 0;
	UWORD body = n > 1 ? (UWORD)(MAXBODY / n) : MAXBODY;
	if (slider_info[i].HorizPot == pot && slider_info[i].HorizBody == body)
		return;
	ModifyProp(&sliders[i], win, NULL, slider_info[i].Flags, pot, 0, body, MAXBODY);
}

/* Reads a slider moved by the user. Returns TRUE if the value changed. */
static BOOL read_slider(struct Gadget *g)
{
	int i = g->GadgetID - GID_VOL;
	int n = slider_steps(i);
	int pos = n > 1 ? (int)(((ULONG)slider_info[i].HorizPot * (n - 1) + MAXPOT / 2) / MAXPOT) : 0;

	if (i == 0 && pos != vol)
	{
		vol = pos;
		out_dirty |= OUT_VOL;
	}
	else if (i == 1 && pos != sql)
	{
		sql = pos;
		out_dirty |= OUT_SQL;
	}
	else if (i == 2)
	{
		int g2 = pos == 0 ? -1 : gains[pos - 1];
		if (g2 == gain)
			return FALSE;
		gain = g2;
		out_dirty |= OUT_GAIN;
	}
	else
		return FALSE;
	dirty |= D_VALUES;
	return TRUE;
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

	/* The band buttons go before the sliders, so that changing a slider
	   (which redraws the gadgets after it) doesn't redraw them. */
	for (pos = 0, g = win->FirstGadget; g && g != &sliders[0]; g = g->NextGadget)
		pos++;

	nband_gadgets = nbands;
	for (i = 0; i < nbands; i++)
	{
		g = &band_gadgets[i];
		init_text(&band_texts[i], bands[i].shortname, BAND_W);
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
	RefreshGadgets(win->FirstGadget, win, NULL);
	dirty |= D_BANDS;
}

/* ---------------------------------------------------------------------- */
/* Drawing                                                                */

static void print_at(int x, int y, const char *s, int maxlen)
{
	int n = strlen(s);
	if (n > maxlen)
		n = maxlen;
	Move(rp, ox + x, oy + y + font->tf_Baseline);
	Text(rp, (STRPTR)s, n);
}

static void clear(int x, int y, int w, int h)
{
	SetAPen(rp, 0);
	RectFill(rp, ox + x, oy + y, ox + x + w - 1, oy + y + h - 1);
}

/* Formats hz as MHz with 4 decimals, e.g. "118.1000" */
static void format_mhz(char *buf, LONG hz)
{
	sprintf(buf, "%ld.%04ld", hz / 1000000L, (hz % 1000000L) / 100);
}

/* a * mul / div for 0 <= a <= div and mul < 1024, without overflowing */
static LONG scale(LONG a, LONG mul, LONG div)
{
	while (div > 0x1FFFFFL)
	{
		a >>= 1;
		div >>= 1;
	}
	return div > 0 ? a * mul / div : 0;
}

static int freq_to_x(LONG f)
{
	struct Band *b = &bands[cur_band];
	if (f < b->lo)
		f = b->lo;
	if (f > b->hi)
		f = b->hi;
	return DIAL_X + 2 + (int)scale(f - b->lo, DIAL_W - 5, b->hi - b->lo);
}

/* The frequency of the channel nearest to dial position x */
static LONG x_to_freq(int x)
{
	struct Band *b = &bands[cur_band];
	LONG span = b->hi - b->lo, w = DIAL_W - 5, f;

	x -= DIAL_X + 2;
	if (x < 0)
		x = 0;
	if (x > w)
		x = w;
	f = (span / w) * x + ((span % w) * x) / w;
	f = (f + b->step / 2) / b->step * b->step;
	return b->lo + (f > span ? span / b->step * b->step : f);
}

static void draw_readout(void)
{
	char buf[80];
	struct Band *b = &bands[cur_band];

	clear(0, 2, 344, 10);
	SetAPen(rp, 1);
	if (entry_len >= 0)
		sprintf(buf, "ENTER MHz: %s_", entry);
	else if (nbands && have_state)
	{
		char mhz[16];
		format_mhz(mhz, freq);
		sprintf(buf, "%s MHz  %-4s %s", mhz, b->mode, b->name);
	}
	else
		strcpy(buf, "NOT CONNECTED");
	print_at(8, 3, buf, 42);
}

static void draw_lamp(int x, const char *label, BOOL on)
{
	SetAPen(rp, on ? 3 : 0);
	RectFill(rp, ox + x + 1, oy + 4, ox + x + 8, oy + 9);
	SetAPen(rp, 1);
	Move(rp, ox + x, oy + 3);
	Draw(rp, ox + x + 9, oy + 3);
	Draw(rp, ox + x + 9, oy + 10);
	Draw(rp, ox + x, oy + 10);
	Draw(rp, ox + x, oy + 3);
	print_at(x + 12, 3, label, 6);
}

static void draw_lamps(void)
{
	BOOL busy = run_status == 0 || run_status == 3;
	BOOL up = conn_state == CS_UP;

	clear(344, 2, 256, 10);
	draw_lamp(344, "TUNER", up && (run_status == 1 || (busy && (blink & 4))));
	draw_lamp(408, "SIGNAL", up && level >= 2);
	draw_lamp(472, "AUDIO", up && pipe_status == 1);
	draw_lamp(536, "SCAN", up && scan_dir != 0);
}

/* A round step for the dial's labels: 1, 2 or 5 times a power of ten,
   giving at most 7 labels across the band. */
static LONG label_step(LONG span)
{
	LONG s = 1;
	for (;;)
	{
		if (span / s <= 7)
			return s;
		if (span / (s * 2) <= 7)
			return s * 2;
		if (span / (s * 5) <= 7)
			return s * 5;
		if (s > 100000000L)
			return s * 10;
		s *= 10;
	}
}

static void draw_dial(void)
{
	struct Band *b = &bands[cur_band];
	LONG step, minor, f, first;
	int i, x;

	clear(DIAL_X, DIAL_Y, DIAL_W, DIAL_H);
	SetAPen(rp, 1);
	Move(rp, ox + DIAL_X, oy + DIAL_Y);
	Draw(rp, ox + DIAL_X + DIAL_W - 1, oy + DIAL_Y);
	Draw(rp, ox + DIAL_X + DIAL_W - 1, oy + DIAL_Y + DIAL_H - 1);
	Draw(rp, ox + DIAL_X, oy + DIAL_Y + DIAL_H - 1);
	Draw(rp, ox + DIAL_X, oy + DIAL_Y);
	if (!nbands || !have_state)
		return;

	/* Scale: long ticks with labels, five short ticks between them */
	step = label_step(b->hi - b->lo);
	minor = step >= 5 ? step / 5 : step;
	first = (b->lo + step - 1) / step * step;
	for (f = first - step; f <= b->hi; f += minor)
	{
		if (f < b->lo)
			continue;
		x = ox + freq_to_x(f);
		Move(rp, x, oy + DIAL_Y + 1);
		Draw(rp, x, oy + DIAL_Y + ((f - first) % step == 0 ? 8 : 4));
		if ((f - first) % step == 0)
		{
			char buf[16];
			int n, dec = step >= 1000000L ? 0 : step >= 100000L ? 1 : step >= 10000L ? 2 : 3;
			format_mhz(buf, f);
			n = strlen(buf) - (dec ? 4 - dec : 5);
			x -= 4 * n;
			if (x < ox + DIAL_X + 2)
				x = ox + DIAL_X + 2;
			if (x + 8 * n > ox + DIAL_X + DIAL_W - 2)
				x = ox + DIAL_X + DIAL_W - 2 - 8 * n;
			Move(rp, x, oy + DIAL_Y + 11 + font->tf_Baseline);
			Text(rp, (STRPTR)buf, n);
		}
	}

	/* Active channels found by the last sweep */
	SetAPen(rp, 3);
	for (i = 0; i < nactive; i++)
	{
		x = ox + freq_to_x(b->lo + active[i] * b->step);
		RectFill(rp, x - 1, oy + DIAL_Y + DIAL_H - 6, x + 1, oy + DIAL_Y + DIAL_H - 3);
	}

	/* Needle */
	x = ox + freq_to_x(freq);
	RectFill(rp, x, oy + DIAL_Y + 1, x + 1, oy + DIAL_Y + DIAL_H - 2);
}

static void draw_meter(void)
{
	int w = level * METER_W / 99;
	SetAPen(rp, 1);
	print_at(8, METER_Y - 1, "LEVEL", 5);
	Move(rp, ox + METER_X - 1, oy + METER_Y - 1);
	Draw(rp, ox + METER_X + METER_W, oy + METER_Y - 1);
	Draw(rp, ox + METER_X + METER_W, oy + METER_Y + 6);
	Draw(rp, ox + METER_X - 1, oy + METER_Y + 6);
	Draw(rp, ox + METER_X - 1, oy + METER_Y - 1);
	if (w > 0)
	{
		SetAPen(rp, 3);
		RectFill(rp, ox + METER_X, oy + METER_Y, ox + METER_X + w - 1, oy + METER_Y + 5);
	}
	if (w < METER_W)
		clear(METER_X + w, METER_Y, METER_W - w, 6);
}

static void draw_values(void)
{
	char buf[16];
	int i;

	for (i = 0; i < 3; i++)
	{
		int x = 8 + i * 196;
		clear(x, SLIDER_Y, 32, 10);
		clear(x + 156, SLIDER_Y, 32, 10);
		SetAPen(rp, 1);
		print_at(x, SLIDER_Y + 1, slider_labels[i], 4);
		if (i == 0)
			sprintf(buf, "%d%%", vol * 100 / 8);
		else if (i == 1)
			sprintf(buf, sql ? "%d" : "OFF", sql);
		else if (gain < 0)
			strcpy(buf, "AUTO");
		else
			sprintf(buf, "%d", (gain + 5) / 10);
		print_at(x + 156, SLIDER_Y + 1, buf, 4);
	}
}

static void draw_bands(void)
{
	int i;
	for (i = 0; i < nband_gadgets; i++)
	{
		SetAPen(rp, i == cur_band && have_state ? 3 : 0);
		RectFill(rp, ox + 8 + i * (BAND_W + 2), oy + BAND_Y + BUTTON_H + 1,
			ox + 8 + i * (BAND_W + 2) + BAND_W - 1, oy + BAND_Y + BUTTON_H + 2);
	}
}

static void draw_msg(void)
{
	clear(0, MSG_Y, INNER_W, 8);
	SetAPen(rp, 1);
	print_at(8, MSG_Y, msg, 73);
}

static void refresh(void)
{
	int i;

	if (dirty)
		SetDrMd(rp, JAM1);	/* in case gadget drawing changed it */
	if (dirty & D_READOUT)
		draw_readout();
	if (dirty & D_LAMPS)
		draw_lamps();
	if (dirty & D_DIAL)
		draw_dial();
	if (dirty & D_METER)
		draw_meter();
	if (dirty & D_SLIDERS)
		for (i = 0; i < 3; i++)
			if (&sliders[i] != active_slider)
				update_slider(i);
	if (dirty & (D_VALUES | D_SLIDERS))
		draw_values();
	if (dirty & D_BANDS)
		draw_bands();
	if (dirty & D_MSG)
		draw_msg();
	dirty = 0;
}

static void set_msg(const char *s)
{
	strncpy(msg, s, sizeof(msg) - 1);
	msg[sizeof(msg) - 1] = 0;
	dirty |= D_MSG;
}

/* ---------------------------------------------------------------------- */
/* A314 connection                                                        */

static void start_a314_cmd(struct A314_IORequest *ior, UWORD command, char *buffer, int length)
{
	ior->a314_Request.io_Command = command;
	ior->a314_Request.io_Error = 0;
	ior->a314_Socket = socket;
	ior->a314_Buffer = (STRPTR)buffer;
	ior->a314_Length = length;
	SendIO((struct IORequest *)ior);
}

static void start_read(void)
{
	start_a314_cmd(rmsg, A314_READ, (char *)arbuf, 255);
	pending_read = TRUE;
}

static void start_reset(void)
{
	/* Completes at once, and makes the device return any pending request */
	if (!pending_reset)
	{
		start_a314_cmd(xmsg, A314_RESET, NULL, 0);
		pending_reset = TRUE;
	}
}

static void start_connect(void)
{
	/* A new socket number for every attempt */
	socket = ((ULONG)time(NULL) << 4) + (socket_seq++ & 15);
	start_a314_cmd(cmsg, A314_CONNECT, SERVICE_NAME, strlen(SERVICE_NAME));
	pending_connect = TRUE;
	conn_state = CS_CONNECTING;
	conn_ticks = 0;
	set_msg("CONNECTING TO THE PI...");
}

static BOOL a314_idle(void)
{
	return !pending_connect && !pending_read && !pending_write && !pending_reset;
}

/* Drops the connection; connection_tick() tries again after a pause.
   Everything learnt from the Pi is sent again on reconnecting. */
static void connection_lost(const char *why)
{
	if (conn_state == CS_UP || conn_state == CS_CONNECTING)
		start_reset();
	conn_state = CS_CLOSING;
	conn_ticks = 0;
	have_state = FALSE;
	run_status = pipe_status = -1;
	level = scan_dir = nactive = 0;
	cmdq_len = out_dirty = linelen = 0;
	set_msg(why);
	dirty |= D_ALL;
}

/* Queues a command for the Pi. Commands are only lost if the queue is
   full, which can only happen while the Pi isn't reading them; the
   connection is then soon dropped and everything is sent again. */
static void queue_cmd(const char *s)
{
	int n = strlen(s);
	if (conn_state == CS_UP && cmdq_len + n + 1 <= sizeof(cmdq))
	{
		memcpy(cmdq + cmdq_len, s, n);
		cmdq_len += n;
		cmdq[cmdq_len++] = '\n';
	}
}

static void send_pending(void)
{
	int len;

	if (pending_write || conn_state != CS_UP || (cmdq_len == 0 && out_dirty == 0))
		return;

	memcpy(awbuf, cmdq, cmdq_len);
	len = cmdq_len;
	cmdq_len = 0;
	if (out_dirty & OUT_FREQ)
		len += sprintf((char *)awbuf + len, "FREQ %ld\n", freq);
	if (out_dirty & OUT_SQL)
		len += sprintf((char *)awbuf + len, "SQL %d\n", sql);
	if (out_dirty & OUT_GAIN)
		len += sprintf((char *)awbuf + len, "GAIN %d\n", gain);
	if (out_dirty & OUT_VOL)
		len += sprintf((char *)awbuf + len, "VOL %d\n", vol);
	out_dirty = 0;

	start_a314_cmd(wmsg, A314_WRITE, (char *)awbuf, len);
	pending_write = TRUE;
}

/* Splits off the next space-separated word of *pp. */
static char *word(char **pp)
{
	char *p = *pp, *s;
	while (*p == ' ')
		p++;
	if (!*p)
		return NULL;
	s = p;
	while (*p && *p != ' ')
		p++;
	if (*p)
		*p++ = 0;
	*pp = p;
	return s;
}

static LONG number(char **pp)
{
	char *w = word(pp);
	return w ? strtol(w, NULL, 10) : 0;
}

static void copy_word(char *dst, char *src, int size)
{
	strncpy(dst, src ? src : "", size - 1);
	dst[size - 1] = 0;
}

static void handle_line(char *p)
{
	char *cmd = word(&p);
	if (!cmd)
		return;

	if (strcmp(cmd, "BANDS") == 0)
	{
		nbands = (int)number(&p);
		if (nbands < 0 || nbands > MAX_BANDS)
			nbands = nbands < 0 ? 0 : MAX_BANDS;
		if (cur_band >= nbands)
			cur_band = 0;
	}
	else if (strcmp(cmd, "B") == 0)
	{
		int i = (int)number(&p);
		struct Band *b;
		if (i < 0 || i >= MAX_BANDS)
			return;
		b = &bands[i];
		b->lo = number(&p);
		b->hi = number(&p);
		b->step = number(&p);
		if (b->step < 1)
			b->step = 1;
		if (b->lo < 0 || b->hi <= b->lo)
			b->hi = b->lo + b->step;
		copy_word(b->mode, word(&p), sizeof(b->mode));
		copy_word(b->shortname, word(&p), sizeof(b->shortname));
		while (*p == ' ')
			p++;
		copy_word(b->name, p, sizeof(b->name));
		if (i == nbands - 1)
			rebuild_band_buttons();
		dirty |= D_ALL;
	}
	else if (strcmp(cmd, "GAINS") == 0)
	{
		int i, n = (int)number(&p);
		ngains = n < 0 ? 0 : n > MAX_GAINS ? MAX_GAINS : n;
		for (i = 0; i < ngains; i++)
			gains[i] = (WORD)number(&p);
		dirty |= D_SLIDERS;
	}
	else if (strcmp(cmd, "ST") == 0)
	{
		int b = (int)number(&p);
		LONG f = number(&p);
		int s = (int)number(&p), g = (int)number(&p), v = (int)number(&p), sc = (int)number(&p);

		if (b < 0 || b >= nbands)
			return;
		if (b != cur_band)
		{
			cur_band = b;
			nactive = 0;	/* the Pi resends them with ACT */
		}
		/* Don't undo changes that haven't reached the Pi yet */
		if (!out_dirty && !pending_write && cmdq_len == 0 && !dragging_dial)
		{
			freq = f;
			if (active_slider == NULL)
			{
				sql = s < 0 ? 0 : s > SQL_MAX ? SQL_MAX : s;
				gain = g < -1 ? -1 : g;
				vol = v < 0 ? 0 : v > 16 ? 16 : v;
			}
		}
		scan_dir = sc;
		have_state = TRUE;
		dirty |= D_READOUT | D_DIAL | D_LAMPS | D_SLIDERS | D_BANDS;
	}
	else if (strcmp(cmd, "RUN") == 0)
	{
		run_status = (int)number(&p);
		dirty |= D_LAMPS;
	}
	else if (strcmp(cmd, "PIPE") == 0)
	{
		pipe_status = (int)number(&p);
		dirty |= D_LAMPS;
	}
	else if (strcmp(cmd, "LVL") == 0)
	{
		BOOL was_open = level >= 2;
		level = (int)number(&p);
		level = level < 0 ? 0 : level > 99 ? 99 : level;
		dirty |= D_METER;
		if ((level >= 2) != was_open)
			dirty |= D_LAMPS;
	}
	else if (strcmp(cmd, "ACTCLR") == 0)
	{
		if (number(&p) == cur_band)
		{
			nactive = 0;
			dirty |= D_DIAL;
		}
	}
	else if (strcmp(cmd, "ACT") == 0)
	{
		char *w;
		if (number(&p) != cur_band)
			return;
		while ((w = word(&p)) != NULL && nactive < MAX_ACTIVE)
			active[nactive++] = strtol(w, NULL, 10);
		dirty |= D_DIAL;
	}
	else if (strcmp(cmd, "MSG") == 0)
	{
		while (*p == ' ')
			p++;
		set_msg(p);
	}
	/* HB (heartbeat) needs nothing: any data resets the idle timer */
}

static void handle_read_completed(void)
{
	int i;

	pending_read = FALSE;
	if (conn_state != CS_UP)
		return;

	if (rmsg->a314_Request.io_Error != A314_READ_OK)
	{
		connection_lost("THE CONNECTION TO THE PI WAS LOST - RECONNECTING...");
		return;
	}
	rx_idle_ticks = 0;
	for (i = 0; i < rmsg->a314_Length; i++)
	{
		char c = arbuf[i];
		if (c == '\n')
		{
			linebuf[linelen] = 0;
			handle_line(linebuf);
			linelen = 0;
		}
		else if (linelen < sizeof(linebuf) - 1)
			linebuf[linelen++] = c;
	}
	if (conn_state == CS_UP)
		start_read();
}

static void handle_connect_completed(void)
{
	BYTE err = cmsg->a314_Request.io_Error;

	pending_connect = FALSE;
	if (conn_state != CS_CONNECTING)
		return;

	if (err == A314_CONNECT_OK)
	{
		conn_state = CS_UP;
		rx_idle_ticks = 0;
		linelen = 0;
		start_read();
		queue_cmd("HELLO");
		set_msg("CONNECTED, WAITING FOR THE PI...");
	}
	else
	{
		/* The device has already closed the socket */
		conn_state = CS_DOWN;
		conn_ticks = 0;
		set_msg(err == A314_CONNECT_UNKNOWN_SERVICE ?
			"THE PIRADIO SERVICE IS NOT INSTALLED ON THE PI - RETRYING..." :
			"CANNOT CONNECT TO THE PI - RETRYING...");
	}
	dirty |= D_ALL;
}

static void handle_a314_port(void)
{
	struct Message *m;

	while ((m = GetMsg(mp)) != NULL)
	{
		if (m == (struct Message *)rmsg)
			handle_read_completed();
		else if (m == (struct Message *)cmsg)
			handle_connect_completed();
		else if (m == (struct Message *)wmsg)
		{
			pending_write = FALSE;
			if (wmsg->a314_Request.io_Error != A314_WRITE_OK && conn_state == CS_UP)
				connection_lost("THE CONNECTION TO THE PI WAS LOST - RECONNECTING...");
		}
		else if (m == (struct Message *)xmsg)
			pending_reset = FALSE;
	}

	if (conn_state == CS_CLOSING && a314_idle())
	{
		conn_state = CS_DOWN;
		conn_ticks = 0;
	}
}

/* Called 10 times a second */
static void connection_tick(void)
{
	if (!a314_open)
		return;
	conn_ticks++;
	if (conn_state == CS_DOWN && conn_ticks >= RETRY_TICKS && a314_idle())
		start_connect();
	else if (conn_state == CS_CONNECTING && conn_ticks >= CONNECT_TIMEOUT_TICKS)
		connection_lost("NO ANSWER FROM THE PI - RETRYING...");
	else if (conn_state == CS_UP && ++rx_idle_ticks >= RX_TIMEOUT_TICKS)
		connection_lost("THE PI STOPPED RESPONDING - RECONNECTING...");
}

/* ---------------------------------------------------------------------- */
/* User input                                                             */

static void tune_to(LONG f)
{
	struct Band *b = &bands[cur_band];
	if (!nbands || !have_state)
		return;
	if (f < b->lo)
		f = b->lo;
	if (f > b->hi)
		f = b->hi;
	if (f != freq)
	{
		freq = f;
		out_dirty |= OUT_FREQ;
		dirty |= D_READOUT | D_DIAL;
	}
}

static void select_band(int i)
{
	char buf[16];
	if (i >= 0 && i < nbands && have_state)
	{
		sprintf(buf, "BAND %d", i);
		queue_cmd(buf);
	}
}

static void change(int *value, int delta, int lo, int hi, int out)
{
	int v = *value + delta;
	v = v < lo ? lo : v > hi ? hi : v;
	if (v != *value)
	{
		*value = v;
		out_dirty |= out;
		dirty |= D_SLIDERS;
	}
}

static void change_gain(int delta)
{
	int pos = slider_value(2) + delta;
	if (pos < 0 || pos > ngains)
		return;
	gain = pos == 0 ? -1 : gains[pos - 1];
	out_dirty |= OUT_GAIN;
	dirty |= D_SLIDERS;
}

static void handle_gadget(struct Gadget *g, BOOL up)
{
	int id = g->GadgetID;

	if (id >= GID_VOL)
	{
		read_slider(g);
		active_slider = up ? NULL : g;
		if (up)
			dirty |= D_SLIDERS;
		return;
	}
	if (!up)
		return;
	if (id < MAX_BANDS)
		select_band(id);
	else if (id == GID_SEEK_DOWN)
		queue_cmd("SEEK -1");
	else if (id == GID_SEEK_UP)
		queue_cmd("SEEK 1");
	else if (id == GID_SCAN)
		queue_cmd(scan_dir ? "SCAN 0" : "SCAN 1");
	else if (id == GID_SWEEP)
		queue_cmd("SWEEP");
}

static BOOL in_dial(int x, int y)
{
	x -= ox;
	y -= oy;
	return x >= DIAL_X && x < DIAL_X + DIAL_W && y >= DIAL_Y && y < DIAL_Y + DIAL_H;
}

static void handle_char(char c, UWORD qual)
{
	BOOL shift = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;

	if (entry_len >= 0)
	{
		if (((c >= '0' && c <= '9') || c == '.') && entry_len < sizeof(entry) - 1)
		{
			entry[entry_len++] = c;
			entry[entry_len] = 0;
		}
		else if (c == 8 && entry_len > 0)
			entry[--entry_len] = 0;
		else if (c == '\r')
		{
			/* MHz with up to six decimals, to Hz */
			LONG mhz = strtol(entry, NULL, 10), hz = mhz * 1000000L;
			char *dot = strchr(entry, '.');
			LONG unit = 100000L;
			if (mhz > 2000)
				hz = 0;
			else if (dot)
				for (dot++; *dot >= '0' && *dot <= '9' && unit > 0; dot++, unit /= 10)
					hz += (*dot - '0') * unit;
			entry_len = -1;
			if (hz > 0)
			{
				freq = hz;
				out_dirty |= OUT_FREQ;
			}
		}
		dirty |= D_READOUT;
		return;
	}

	if ((c >= '0' && c <= '9') || c == '.')
	{
		entry[0] = c;
		entry[1] = 0;
		entry_len = 1;
		dirty |= D_READOUT;
	}
	else if (c == ' ')
		queue_cmd(scan_dir ? "SCAN 0" : shift ? "SCAN -1" : "SCAN 1");
	else if (c == 'r' || c == 'R')
		queue_cmd("SWEEP");
	else if (c == 'v' || c == '-')
		change(&vol, -1, 0, 16, OUT_VOL);
	else if (c == 'V' || c == '+')
		change(&vol, 1, 0, 16, OUT_VOL);
	else if (c == 's')
		change(&sql, -5, 0, SQL_MAX, OUT_SQL);
	else if (c == 'S')
		change(&sql, 5, 0, SQL_MAX, OUT_SQL);
	else if (c == 'g')
		change_gain(-1);
	else if (c == 'G')
		change_gain(1);
	else if (c == 'a' || c == 'A')
	{
		gain = -1;
		out_dirty |= OUT_GAIN;
		dirty |= D_SLIDERS;
	}
	else if (c == 'q' || c == 'Q')
		quit = TRUE;
}

static void handle_rawkey(UWORD code, UWORD qual, APTR prev)
{
	struct InputEvent ie;
	char buf[8];
	int n;

	if (code & IECODE_UP_PREFIX)
		return;

	if (code == RAW_LEFT || code == RAW_RIGHT)
	{
		LONG steps = qual & (IEQUALIFIER_LALT | IEQUALIFIER_RALT) ? 100 :
			qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT) ? 10 : 1;
		if (nbands && have_state)
			tune_to(freq + (code == RAW_RIGHT ? steps : -steps) * bands[cur_band].step);
		return;
	}
	if (code == RAW_UP || code == RAW_DOWN)
	{
		queue_cmd(code == RAW_UP ? "SEEK 1" : "SEEK -1");
		return;
	}
	if (code >= RAW_F1 && code <= RAW_F10)
	{
		select_band(code - RAW_F1);
		return;
	}
	if (code == RAW_ESC)
	{
		if (entry_len >= 0)
		{
			entry_len = -1;
			dirty |= D_READOUT;
		}
		else
			quit = TRUE;
		return;
	}

	/* Everything else is translated with the user's keymap */
	if (!ConsoleDevice)
		return;
	memset(&ie, 0, sizeof(ie));
	ie.ie_Class = IECLASS_RAWKEY;
	ie.ie_Code = code;
	ie.ie_Qualifier = qual;
	ie.ie_EventAddress = prev;
	n = RawKeyConvert(&ie, (STRPTR)buf, sizeof(buf), NULL);
	if (n == 1)
		handle_char(buf[0], qual);
}

static void handle_window(void)
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
			if (code == SELECTDOWN && in_dial(mx, my) && nbands && have_state)
			{
				dragging_dial = TRUE;
				ReportMouse(TRUE, win);
				tune_to(x_to_freq(mx - ox));
			}
			else if (code == SELECTUP && dragging_dial)
			{
				dragging_dial = FALSE;
				ReportMouse(FALSE, win);
			}
			break;
		case IDCMP_MOUSEMOVE:
			if (dragging_dial)
				tune_to(x_to_freq(mx - ox));
			else if (active_slider)
				read_slider(active_slider);
			break;
		case IDCMP_INACTIVEWINDOW:
			if (dragging_dial)
			{
				dragging_dial = FALSE;
				ReportMouse(FALSE, win);
			}
			break;
		case IDCMP_RAWKEY:
			handle_rawkey(code, qual, prev);
			break;
		}
	}
}

/* ---------------------------------------------------------------------- */
/* Timer, 10 ticks a second                                               */

static void start_timer(void)
{
	timer_req->tr_node.io_Command = TR_ADDREQUEST;
	timer_req->tr_time.tv_secs = 0;
	timer_req->tr_time.tv_micro = 100000;
	SendIO((struct IORequest *)timer_req);
	timer_pending = TRUE;
}

static void tick(void)
{
	connection_tick();
	blink++;
	if (run_status == 0 || run_status == 3)
		dirty |= D_LAMPS;
}

/* ---------------------------------------------------------------------- */

static BOOL open_window(void)
{
	struct Screen wb;
	struct NewWindow nw;
	int i, bl, bt, br, bb;

	if (!GetScreenData((APTR)&wb, sizeof(wb), WBENCHSCREEN, NULL))
		return FALSE;

	/* Expected sizes of the window's borders. The gadgets are placed using
	   the real sizes once the window is open. */
	bl = wb.WBorLeft;
	br = wb.WBorRight;
	bb = wb.WBorBottom;
	bt = wb.WBorTop + wb.Font->ta_YSize + 1;

	memset(&nw, 0, sizeof(nw));
	nw.Width = INNER_W + bl + br;
	nw.Height = INNER_H + bt + bb;
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
		return FALSE;
	rp = win->RPort;
	SetFont(rp, font);
	SetDrMd(rp, JAM1);

	ox = win->BorderLeft;
	oy = win->BorderTop;
	init_gadgets();
	for (i = 0; i < 4; i++)
		AddGadget(win, &buttons[i], -1);
	for (i = 0; i < 3; i++)
		AddGadget(win, &sliders[i], -1);
	RefreshGadgets(win->FirstGadget, win, NULL);
	return TRUE;
}

int main(void)
{
	ULONG winsig, portsig, timersig;
	BOOL a314_stuck = FALSE;
	int rc = 0;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
	GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 0);
	if (!IntuitionBase || !GfxBase)
	{
		rc = 20;
		goto cleanup;
	}

	/* Only one copy at a time: the message port is also the marker */
	Forbid();
	if (!FindPort((STRPTR)PORT_NAME))
		mp = CreatePort((STRPTR)PORT_NAME, 0);
	Permit();
	if (!mp)
	{
		printf("PiRadio is already running\n");
		rc = 5;
		goto cleanup;
	}
	cmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
	rmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
	wmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
	xmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
	timer_mp = CreatePort(NULL, 0);
	if (timer_mp)
		timer_req = (struct timerequest *)CreateExtIO(timer_mp, sizeof(struct timerequest));
	if (!cmsg || !rmsg || !wmsg || !xmsg || !timer_req)
	{
		printf("Not enough memory\n");
		rc = 20;
		goto cleanup;
	}
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)timer_req, 0) != 0)
	{
		printf("Unable to open timer.device\n");
		rc = 20;
		goto cleanup;
	}
	timer_open = TRUE;

	/* Without a314.device the window still opens, so that the problem is
	   visible when PiRadio is started from an icon */
	if (OpenDevice((STRPTR)A314_NAME, 0, (struct IORequest *)cmsg, 0) == 0)
	{
		a314_open = TRUE;
		memcpy(rmsg, cmsg, sizeof(struct A314_IORequest));
		memcpy(wmsg, cmsg, sizeof(struct A314_IORequest));
		memcpy(xmsg, cmsg, sizeof(struct A314_IORequest));
	}

	/* Unit -1 opens console.device only for RawKeyConvert() */
	if (OpenDevice((STRPTR)"console.device", -1, (struct IORequest *)&console_req, 0) == 0)
		ConsoleDevice = console_req.io_Device;

	font = OpenFont(&topaz8);
	if (!font || !open_window())
	{
		printf("Unable to open the window\n");
		rc = 20;
		goto cleanup;
	}

	start_timer();
	if (a314_open)
		start_connect();
	else
		set_msg("A314.DEVICE NOT FOUND - IS THE A314 SOFTWARE INSTALLED?");

	winsig = 1L << win->UserPort->mp_SigBit;
	portsig = 1L << mp->mp_SigBit;
	timersig = 1L << timer_mp->mp_SigBit;

	while (!quit)
	{
		ULONG sig;

		send_pending();
		refresh();

		sig = Wait(winsig | portsig | timersig | SIGBREAKF_CTRL_C);
		if (sig & portsig)
			handle_a314_port();
		if (sig & winsig)
			handle_window();
		if ((sig & timersig) && GetMsg(timer_mp))
		{
			timer_pending = FALSE;
			tick();
			start_timer();
		}
		if (sig & SIGBREAKF_CTRL_C)
			quit = TRUE;
	}

cleanup:
	/* Close the connection, and wait (but not forever) for the device to
	   return every request */
	if (a314_open)
	{
		int waited = 0;
		if (conn_state == CS_UP || conn_state == CS_CONNECTING)
			start_reset();
		conn_state = CS_CLOSING;
		while (!a314_idle() && waited < SHUTDOWN_TIMEOUT_TICKS && timer_open)
		{
			ULONG sig, psig = 1L << mp->mp_SigBit, tsig = 1L << timer_mp->mp_SigBit;
			if (!timer_pending)
				start_timer();
			sig = Wait(psig | tsig);
			if (sig & psig)
				handle_a314_port();
			if ((sig & tsig) && GetMsg(timer_mp))
			{
				timer_pending = FALSE;
				waited++;
			}
		}
		a314_stuck = !a314_idle();
	}

	if (timer_pending)
	{
		AbortIO((struct IORequest *)timer_req);
		WaitIO((struct IORequest *)timer_req);
	}
	if (timer_open)
		CloseDevice((struct IORequest *)timer_req);
	if (timer_req)
		DeleteExtIO((struct IORequest *)timer_req);
	if (timer_mp)
		DeletePort(timer_mp);

	if (win)
		CloseWindow(win);
	if (font)
		CloseFont(font);
	if (ConsoleDevice)
		CloseDevice((struct IORequest *)&console_req);

	if (a314_stuck)
	{
		/* The device still owns some requests. Freeing them could crash
		   the machine later, so leave them, and the port, allocated. The
		   port no longer signals this task, which is about to exit. */
		printf("a314.device did not return all requests\n");
		Forbid();
		mp->mp_Flags = PA_IGNORE;
		RemPort(mp);
		Permit();
	}
	else
	{
		if (a314_open)
			CloseDevice((struct IORequest *)cmsg);
		if (xmsg)
			DeleteExtIO((struct IORequest *)xmsg);
		if (wmsg)
			DeleteExtIO((struct IORequest *)wmsg);
		if (rmsg)
			DeleteExtIO((struct IORequest *)rmsg);
		if (cmsg)
			DeleteExtIO((struct IORequest *)cmsg);
		if (mp)
			DeletePort(mp);
	}

	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	return rc;
}
