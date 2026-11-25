/*
 * PiRadio - tune an RTL-SDR dongle on the Raspberry Pi from the Amiga.
 *
 * Talks to the piradio service on the Pi over A314. The radio is shown on
 * its own screen (panel.c), or with the WINDOW ToolType or argument in a
 * window on the Workbench (window.c). The sound is played by PiAudio, which
 * must be running as well.
 *
 * Only uses Kickstart/Workbench 1.3 functions.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <devices/inputevent.h>
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <libraries/dos.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include <proto/alib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/icon.h>
#include <proto/console.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../a314device/a314.h"
#include "piradio.h"

#define SERVICE_NAME	"piradio"
#define PORT_NAME	"PiRadio"

/* Timing, in ticks of 0.1 s */
#define RETRY_TICKS		30	/* pause before reconnecting */
#define CONNECT_TIMEOUT_TICKS	50
#define RX_TIMEOUT_TICKS	80	/* the service sends HB every 2 s */
#define SHUTDOWN_TIMEOUT_TICKS	20
#define HOLD_TICKS		6	/* the Pi's state doesn't undo the user's
					   changes for this long */

/* Values waiting to be sent to the Pi */
#define OUT_FREQ	1
#define OUT_SQL		2
#define OUT_GAIN	4
#define OUT_VOL		8

#define RAW_ESC		0x45
#define RAW_UP		0x4C
#define RAW_DOWN	0x4D
#define RAW_RIGHT	0x4E
#define RAW_LEFT	0x4F
#define RAW_F1		0x50
#define RAW_F10		0x59
#define RAW_HELP	0x5F

#define SHIFT		(IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)
#define ALT		(IEQUALIFIER_LALT | IEQUALIFIER_RALT)

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *IconBase = NULL;

/* Set by the startup code when started from Workbench */
extern struct WBStartup *_WBenchMsg;

/* The name the vbcc inline for RawKeyConvert() expects */
struct Device *ConsoleDevice = NULL;
static struct IOStdReq console_req;

static const struct Display *display = &panel_display;

/* What is known about the radio */
struct Band bands[MAX_BANDS];
int nbands = 0, cur_band = 0;
char regions[MAX_REGIONS][32];		/* names of the band lists */
int nregions = 0, cur_region = 0;
WORD gains[MAX_GAINS];
int ngains = 0;
LONG active[MAX_ACTIVE];
int nactive = 0;
LONG freq = 0;
int sql = 0, gain = -1, vol = 8, scan_dir = 0;
int run_status = -1, pipe_status = -1, level = 0;
BOOL have_state = FALSE;
char msg[80] = "";
char entry[12];
int entry_len = -1;
int blink = 0;
int changed = CH_ALL;
BOOL quit = FALSE;
BOOL ui_dragging = FALSE;

static int hold_ticks = 0;
static BOOL awaiting_band = FALSE;	/* a band was chosen here; the Pi's
					   answer is still to come */

/* The A314 connection */
static struct MsgPort *mp = NULL;
static struct A314_IORequest *cmsg = NULL, *rmsg = NULL, *wmsg = NULL, *xmsg = NULL;
static BOOL a314_open = FALSE;
static BOOL pending_connect = FALSE, pending_read = FALSE;
static BOOL pending_write = FALSE, pending_reset = FALSE;
static ULONG socket;
static int socket_seq = 0;
int conn_state = CS_DOWN;
static int conn_ticks = 0;
static int rx_idle_ticks = 0;
static UBYTE arbuf[256];
static UBYTE awbuf[256];
static char linebuf[512];
static int linelen = 0;
static char cmdq[160];		/* commands waiting to be sent; with the values
				   below they fit in one 252-byte write */
static int cmdq_len = 0;
static int out_dirty = 0;

static struct MsgPort *timer_mp = NULL;
static struct timerequest *timer_req = NULL;
static BOOL timer_open = FALSE, timer_pending = FALSE;

/* Shows a message in the shell, or in a requester when started from
   Workbench (where there is no shell). */
void report(const char *text)
{
	struct IntuiText body = { 0, 1, JAM2, 16, 8, NULL, (UBYTE *)text, NULL };
	struct IntuiText ok = { 0, 1, JAM2, 6, 3, NULL, (UBYTE *)"OK", NULL };

	if (!_WBenchMsg || !IntuitionBase)
		printf("%s\n", text);
	else
		AutoRequest(display->window(), &body, NULL, &ok, 0, 0, 8 * strlen(text) + 48, 54);
}

/* Formats hz as MHz with 4 decimals, e.g. "118.1000" */
void format_mhz(char *buf, LONG hz)
{
	sprintf(buf, "%ld.%04ld", hz / 1000000L, (hz % 1000000L) / 100);
}

/* a * mul / div for 0 <= a <= div and mul < 1024, without overflowing */
LONG scale_long(LONG a, LONG mul, LONG div)
{
	while (div > 0x1FFFFFL)
	{
		a >>= 1;
		div >>= 1;
	}
	return div > 0 ? a * mul / div : 0;
}

static void set_msg(const char *s)
{
	strncpy(msg, s, sizeof(msg) - 1);
	msg[sizeof(msg) - 1] = 0;
	changed |= CH_MSG;
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
	have_state = awaiting_band = FALSE;
	run_status = pipe_status = -1;
	level = scan_dir = nactive = 0;
	cmdq_len = out_dirty = linelen = hold_ticks = 0;
	set_msg(why);
	changed |= CH_ALL;
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

static char *rest(char *p)
{
	while (*p == ' ')
		p++;
	return p;
}

/* Takes the Pi's state, unless it would undo a change the user has just
   made that hasn't reached the Pi yet */
static void handle_state(char *p)
{
	int b = (int)number(&p);
	LONG f = number(&p);
	int s = (int)number(&p), g = (int)number(&p), v = (int)number(&p), sc = (int)number(&p);
	BOOL new_band;

	if (b < 0 || b >= nbands)
		return;
	new_band = b != cur_band || !have_state;
	if (b != cur_band)
	{
		cur_band = b;
		nactive = 0;	/* the Pi sends them again */
		changed |= CH_DIAL | CH_BANDS;
	}
	if (awaiting_band && b == cur_band)
	{
		awaiting_band = FALSE;
		new_band = TRUE;
	}
	if (new_band || (hold_ticks == 0 && !out_dirty && !pending_write && cmdq_len == 0 && !ui_dragging))
	{
		s = s < 0 ? 0 : s > SQL_MAX ? SQL_MAX : s;
		g = g < -1 ? -1 : g;
		v = v < 0 ? 0 : v > 16 ? 16 : v;
		if (f != freq)
			changed |= CH_FREQ;
		if (s != sql)
			changed |= CH_SQL;
		if (g != gain)
			changed |= CH_GAIN;
		if (v != vol)
			changed |= CH_VOL;
		freq = f;
		sql = s;
		gain = g;
		vol = v;
	}
	sc = sc < -1 ? -1 : sc > 1 ? 1 : sc;
	if (sc != scan_dir)
	{
		scan_dir = sc;
		changed |= CH_SCAN | CH_LAMPS;
	}
	if (!have_state)
	{
		have_state = TRUE;
		changed |= CH_ALL;
	}
}

static void handle_line(char *p)
{
	char *cmd = word(&p);
	if (!cmd)
		return;

	if (strcmp(cmd, "REGIONS") == 0)
	{
		nregions = (int)number(&p);
		if (nregions < 0 || nregions > MAX_REGIONS)
			nregions = nregions < 0 ? 0 : MAX_REGIONS;
	}
	else if (strcmp(cmd, "R") == 0)
	{
		int i = (int)number(&p);
		if (i >= 0 && i < MAX_REGIONS)
			copy_word(regions[i], rest(p), sizeof(regions[i]));
	}
	else if (strcmp(cmd, "REG") == 0)
	{
		cur_region = (int)number(&p);
		if (cur_region < 0 || cur_region >= nregions)
			cur_region = 0;
		changed |= CH_BANDS;
	}
	else if (strcmp(cmd, "BANDS") == 0)
	{
		nactive = 0;	/* a new band list, perhaps for another region */
		nbands = (int)number(&p);
		if (nbands < 0 || nbands > MAX_BANDS)
			nbands = nbands < 0 ? 0 : MAX_BANDS;
		if (cur_band >= nbands)
			cur_band = 0;
		changed |= CH_BANDS | CH_DIAL;
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
		if (b->lo < 0 || b->hi <= b->lo || b->hi > 2000000000L)
			b->hi = b->lo + b->step;
		copy_word(b->mode, word(&p), sizeof(b->mode));
		copy_word(b->shortname, word(&p), sizeof(b->shortname));
		copy_word(b->name, rest(p), sizeof(b->name));
		changed |= CH_BANDS | CH_DIAL;
	}
	else if (strcmp(cmd, "GAINS") == 0)
	{
		int i, n = (int)number(&p);
		ngains = n < 0 ? 0 : n > MAX_GAINS ? MAX_GAINS : n;
		for (i = 0; i < ngains; i++)
			gains[i] = (WORD)number(&p);
		changed |= CH_GAIN;
	}
	else if (strcmp(cmd, "ST") == 0)
		handle_state(p);
	else if (strcmp(cmd, "RUN") == 0)
	{
		run_status = (int)number(&p);
		changed |= CH_LAMPS;
	}
	else if (strcmp(cmd, "PIPE") == 0)
	{
		pipe_status = (int)number(&p);
		changed |= CH_LAMPS;
	}
	else if (strcmp(cmd, "LVL") == 0)
	{
		int l = (int)number(&p);
		l = l < 0 ? 0 : l > 99 ? 99 : l;
		if (l != level)
		{
			if ((l >= 2) != (level >= 2))
				changed |= CH_LAMPS;
			level = l;
			changed |= CH_LEVEL;
		}
	}
	else if (strcmp(cmd, "ACTCLR") == 0)
	{
		if (number(&p) == cur_band)
		{
			nactive = 0;
			changed |= CH_DIAL;
		}
	}
	else if (strcmp(cmd, "ACT") == 0)
	{
		char *w;
		if (number(&p) != cur_band)
			return;
		while ((w = word(&p)) != NULL && nactive < MAX_ACTIVE)
			active[nactive++] = strtol(w, NULL, 10);
		changed |= CH_DIAL;
	}
	else if (strcmp(cmd, "MSG") == 0)
	{
		p = rest(p);
		if (strcmp(p, msg) != 0)
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
/* Actions, for both displays                                             */

BOOL can_tune(void)
{
	return nbands > 0 && have_state && !awaiting_band && conn_state == CS_UP;
}

static void stop_scan_here(void)
{
	/* The Pi stops scanning by itself on any tuning command */
	if (scan_dir)
	{
		scan_dir = 0;
		changed |= CH_SCAN | CH_LAMPS;
	}
}

void tune_to(LONG f)
{
	struct Band *b = &bands[cur_band];

	if (!can_tune())
		return;
	/* The nearest channel of the band */
	if (f < b->lo)
		f = b->lo;
	if (f > b->hi)
		f = b->hi;
	f = b->lo + (f - b->lo + b->step / 2) / b->step * b->step;
	if (f > b->hi)
		f -= b->step;
	if (f == freq)
		return;
	if (scan_dir)
	{
		queue_cmd("SCAN 0");
		stop_scan_here();
	}
	freq = f;
	out_dirty |= OUT_FREQ;
	hold_ticks = HOLD_TICKS;
	changed |= CH_FREQ;
}

void tune_steps(LONG n)
{
	if (can_tune())
		tune_to(freq + n * bands[cur_band].step);
}

void select_band(int i)
{
	char buf[16];

	if (i < 0 || i >= nbands || i == cur_band || !can_tune())
		return;
	sprintf(buf, "BAND %d", i);
	queue_cmd(buf);
	/* Show the new band at once; the Pi's answer sets the frequency */
	cur_band = i;
	freq = bands[i].lo;
	nactive = 0;
	out_dirty &= ~OUT_FREQ;
	awaiting_band = TRUE;
	stop_scan_here();
	changed |= CH_BANDS | CH_DIAL | CH_FREQ;
}

void next_region(void)
{
	char buf[16];

	if (nregions > 1 && can_tune())
	{
		sprintf(buf, "REGION %d", (cur_region + 1) % nregions);
		queue_cmd(buf);
		stop_scan_here();
	}
}

void do_seek(int dir)
{
	if (can_tune())
	{
		queue_cmd(dir > 0 ? "SEEK 1" : "SEEK -1");
		stop_scan_here();
	}
}

void toggle_scan(int dir)
{
	if (!can_tune())
		return;
	if (scan_dir)
	{
		queue_cmd("SCAN 0");
		scan_dir = 0;
	}
	else
	{
		queue_cmd(dir > 0 ? "SCAN 1" : "SCAN -1");
		scan_dir = dir > 0 ? 1 : -1;
	}
	changed |= CH_SCAN | CH_LAMPS;
}

void do_sweep(void)
{
	if (can_tune())
	{
		queue_cmd("SWEEP");
		stop_scan_here();
	}
}

void set_vol(int v)
{
	v = v < 0 ? 0 : v > 16 ? 16 : v;
	if (v != vol && have_state)
	{
		vol = v;
		out_dirty |= OUT_VOL;
		hold_ticks = HOLD_TICKS;
		changed |= CH_VOL;
	}
}

void set_sql(int s)
{
	s = s < 0 ? 0 : s > SQL_MAX ? SQL_MAX : s;
	if (s != sql && have_state)
	{
		sql = s;
		out_dirty |= OUT_SQL;
		hold_ticks = HOLD_TICKS;
		changed |= CH_SQL;
	}
}

int gain_index(void)
{
	int j, best = 0;
	LONG bestd = 0x7fffffffL;

	if (gain < 0)
		return 0;
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

void set_gain_index(int i)
{
	int g;

	i = i < 0 ? 0 : i > ngains ? ngains : i;
	g = i == 0 ? -1 : gains[i - 1];
	if (g != gain && have_state)
	{
		gain = g;
		out_dirty |= OUT_GAIN;
		hold_ticks = HOLD_TICKS;
		changed |= CH_GAIN;
	}
}

static void cancel_entry(void)
{
	if (entry_len >= 0)
	{
		entry_len = -1;
		changed |= CH_FREQ;
	}
}

/* Tunes to the frequency typed, in MHz with up to six decimals */
static void commit_entry(void)
{
	LONG mhz = strtol(entry, NULL, 10), hz = mhz * 1000000L, unit = 100000L;
	char *dot = strchr(entry, '.');
	char buf[24];

	cancel_entry();
	if (mhz > 2000 || !can_tune())
		return;
	if (dot)
		for (dot++; *dot >= '0' && *dot <= '9' && unit > 0; dot++, unit /= 10)
			hz += (*dot - '0') * unit;
	if (hz <= 0)
		return;
	if (hz >= bands[cur_band].lo && hz <= bands[cur_band].hi)
		tune_to(hz);
	else
	{
		/* Perhaps in another band: the Pi finds it */
		sprintf(buf, "FREQ %ld", hz);
		queue_cmd(buf);
		stop_scan_here();
	}
}

static void handle_char(char c, UWORD qual)
{
	if (entry_len >= 0)
	{
		if (((c >= '0' && c <= '9') || (c == '.' && !strchr(entry, '.'))) &&
			entry_len < sizeof(entry) - 1)
		{
			entry[entry_len++] = c;
			entry[entry_len] = 0;
		}
		else if (c == 8 && entry_len > 0)
			entry[--entry_len] = 0;
		else if (c == '\r')
		{
			commit_entry();
			return;
		}
		else if (c == 27)
			cancel_entry();
		changed |= CH_FREQ;
		return;
	}

	switch (c)
	{
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9': case '.':
		entry[0] = c;
		entry[1] = 0;
		entry_len = 1;
		changed |= CH_FREQ;
		break;
	case ' ': toggle_scan(qual & SHIFT ? -1 : 1); break;
	case 'r': case 'R': do_sweep(); break;
	case 'v': case '-': set_vol(vol - 1); break;
	case 'V': case '+': case '=': set_vol(vol + 1); break;
	case 's': set_sql(sql - SQL_STEP); break;
	case 'S': set_sql(sql + SQL_STEP); break;
	case 'g': set_gain_index(gain_index() - 1); break;
	case 'G': set_gain_index(gain_index() + 1); break;
	case 'a': case 'A': set_gain_index(0); break;
	case 'w': case 'W':
		if (display->to_back)
			display->to_back();
		break;
	case '?':
		if (display->help)
			display->help();
		break;
	case 27: case 'q': case 'Q': quit = TRUE; break;
	}
}

/* Only IDCMP_RAWKEY is used: before V36, a window that also asks for
   IDCMP_VANILLAKEY never hears the cursor, function or Help keys. Other
   keys are translated with the user's keymap. */
void handle_rawkey(UWORD code, UWORD qual, APTR prev)
{
	LONG steps = qual & ALT ? 100 : qual & SHIFT ? 10 : 1;
	struct InputEvent ie;
	char buf[8];

	if (code & IECODE_UP_PREFIX)
		return;

	/* Only keys with an action end typing a frequency, so that qualifier
	   keys (needed for the digits on some keymaps) don't */
	if (code == RAW_LEFT || code == RAW_RIGHT || code == RAW_UP || code == RAW_DOWN ||
		(code >= RAW_F1 && code <= RAW_F10))
		cancel_entry();

	if (code == RAW_LEFT || code == RAW_RIGHT)
		tune_steps(code == RAW_RIGHT ? steps : -steps);
	else if (code == RAW_UP || code == RAW_DOWN)
		do_seek(code == RAW_UP ? 1 : -1);
	else if (code >= RAW_F1 && code <= RAW_F10)
		select_band(code - RAW_F1);
	else if (code == RAW_HELP)
	{
		if (display->help)
			display->help();
	}
	else if (ConsoleDevice)
	{
		memset(&ie, 0, sizeof(ie));
		ie.ie_Class = IECLASS_RAWKEY;
		ie.ie_Code = code;
		ie.ie_Qualifier = qual;
		ie.ie_EventAddress = prev;
		if (RawKeyConvert(&ie, (STRPTR)buf, sizeof(buf), NULL) == 1)
			handle_char(buf[0], qual);
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
	if (hold_ticks > 0)
		hold_ticks--;
	blink++;
	if ((run_status == 0 || run_status == 3) && (blink & 1) == 0)
		changed |= CH_LAMPS;	/* the tuner lamp flashes */
	display->tick();
}

/* ---------------------------------------------------------------------- */

/* The WINDOW ToolType (from Workbench) or argument (from the shell) */
static BOOL want_window(int argc, char **argv)
{
	BOOL found = FALSE;

	if (!_WBenchMsg)
		return argc > 1 && (strcmp(argv[1], "WINDOW") == 0 || strcmp(argv[1], "window") == 0);

	IconBase = OpenLibrary((STRPTR)"icon.library", 33);
	if (IconBase)
	{
		struct WBArg *arg = &_WBenchMsg->sm_ArgList[_WBenchMsg->sm_NumArgs - 1];
		BPTR old = CurrentDir(arg->wa_Lock);
		struct DiskObject *dob = GetDiskObject((STRPTR)arg->wa_Name);
		if (dob)
		{
			found = FindToolType((STRPTR *)dob->do_ToolTypes, (STRPTR)"WINDOW") != NULL;
			FreeDiskObject(dob);
		}
		CurrentDir(old);
		CloseLibrary(IconBase);
		IconBase = NULL;
	}
	return found;
}

int main(int argc, char **argv)
{
	ULONG portsig, timersig;
	BOOL a314_stuck = FALSE, display_open = FALSE;
	int rc = 0;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
	GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 0);
	if (!IntuitionBase || !GfxBase)
	{
		rc = 20;
		goto cleanup;
	}
	if (want_window(argc, argv))
		display = &window_display;

	/* Only one copy at a time: the message port is also the marker */
	Forbid();
	if (!FindPort((STRPTR)PORT_NAME))
		mp = CreatePort((STRPTR)PORT_NAME, 0);
	Permit();
	if (!mp)
	{
		report("PiRadio is already running");
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
		report("Not enough memory");
		rc = 20;
		goto cleanup;
	}
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)timer_req, 0) != 0)
	{
		report("Unable to open timer.device");
		rc = 20;
		goto cleanup;
	}
	timer_open = TRUE;

	/* Without a314.device the radio still opens, so that the problem is
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

	if (!display->open())
	{
		/* The display has said why */
		rc = 20;
		goto cleanup;
	}
	display_open = TRUE;

	start_timer();
	if (a314_open)
		start_connect();
	else
		set_msg("A314.DEVICE NOT FOUND - IS THE A314 SOFTWARE INSTALLED?");

	portsig = 1L << mp->mp_SigBit;
	timersig = 1L << timer_mp->mp_SigBit;

	while (!quit)
	{
		ULONG sig, dsig = display->signals();

		send_pending();
		display->refresh();

		sig = Wait(dsig | portsig | timersig | SIGBREAKF_CTRL_C);
		if (sig & portsig)
			handle_a314_port();
		if (sig & dsig)
			display->handle();
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

	if (display_open)
		display->close();
	display_open = FALSE;
	if (ConsoleDevice)
		CloseDevice((struct IORequest *)&console_req);

	if (a314_stuck)
	{
		/* The device still owns some requests. Freeing them could crash
		   the machine later, so leave them, and the port, allocated. The
		   port no longer signals this task, which is about to exit. */
		report("a314.device did not return all requests");
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
