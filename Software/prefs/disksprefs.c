/*
 * Disks - the A314 disk images (Prefs/A314/Disks).
 *
 * Shows and changes the disk images (ADF/HDF files on the Raspberry Pi)
 * of a314disk.device, the drives PD0: to PD3:, and of a314scsi.device:
 *
 * - Change... and Eject put an image in a drive, or take it out, at once.
 *   "At start" makes the drive's image the one it gets when the Amiga
 *   starts.
 * - The SCSI units' images are used from the next start of the Amiga.
 *
 * As in the Workbench's preferences editors, Save keeps the settings (the
 * start-up images and the SCSI units), Use keeps the drives as they are
 * now without saving, and Cancel puts the drives back as they were.
 *
 * The settings are kept on the Pi; the a314prefs service there reads and
 * changes them (see a314prefs.py). The images are chosen from the files in
 * the Pi user's home directory.
 *
 * Only uses Kickstart/Workbench 1.3 functions.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <workbench/startup.h>
#include <libraries/dos.h>

#include <proto/alib.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../a314device/a314.h"

#define SERVICE_NAME	"a314prefs"

#define DRIVES		4
#define SCSI_UNITS	4
#define PATH_LEN	250		/* relative to the Pi user's home */
#define MAX_FILES	200
#define NAME_LEN	120		/* the longest the Pi sends */

/* Main window layout, inside the borders */
#define INNER_W		600
#define INNER_H		182
#define ROW_H		14
#define DRIVE_Y		16
#define SCSI_Y		88
#define LABEL_X		8
#define NAME_X		64
#define NAME_W		288
#define CHANGE_X	360
#define CHANGE_W	80
#define EJECT_X		444
#define EJECT_W		64
#define CHECK_X		516
#define BUTTON_H	12
#define MSG_Y		150
#define BOTTOM_Y	166
#define BOTTOM_W	96

/* File browser layout */
#define FB_W		440
#define FB_H		170
#define FB_ROWS		10
#define FB_LIST_X	8
#define FB_LIST_Y	22
#define FB_LIST_W	400
#define FB_ROW_H	10

/* Gadget ids */
#define GID_CHANGE	10		/* + row: drives 0-3, SCSI 4-7 */
#define GID_EJECT	20
#define GID_CHECK	30
#define GID_SAVE	40
#define GID_USE		41
#define GID_CANCEL	42
#define GID_FB_SCROLL	50
#define GID_FB_PARENT	51
#define GID_FB_WRITE	52
#define GID_FB_OK	53
#define GID_FB_CANCEL	54

struct Image_
{
	char path[PATH_LEN + 1];	/* "" for none */
	BOOL rw;
};

struct FileEntry
{
	BOOL dir;
	ULONG size;
	char name[NAME_LEN + 1];
};

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;

/* Set by the startup code when started from Workbench */
extern struct WBStartup *_WBenchMsg;

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };
static struct TextFont *font = NULL;
static struct Window *win = NULL;
static struct RastPort *rp;
static int ox, oy;

/* The connection to the Pi */
static struct MsgPort *mp = NULL;
static struct A314_IORequest *cmsg = NULL, *rmsg = NULL, *wmsg = NULL;
static struct timerequest *treq = NULL;
static BOOL a314_open = FALSE, timer_open = FALSE, connected = FALSE;
static BOOL read_pending = FALSE, write_pending = FALSE;
static ULONG socket;
static UBYTE arbuf[256];
static char line[256];
static char awbuf[256];

/* What the drives and units have, and what they had at the start */
static struct Image_ drives[DRIVES], drives_orig[DRIVES];
static BOOL drives_known = FALSE;	/* the disk service answered */
static struct Image_ auto_images[DRIVES];
static BOOL at_start[DRIVES], at_start_changed[DRIVES];
static struct Image_ scsi[SCSI_UNITS];
static BOOL scsi_changed[SCSI_UNITS];

static struct FileEntry *files = NULL;
static int nfiles = 0;

static BOOL quit = FALSE;

/* Shows a message in the shell, or in a requester when started from
   Workbench (where there is no shell). */
static void report(const char *text)
{
	struct IntuiText body = { 0, 1, JAM2, 16, 8, NULL, (UBYTE *)text, NULL };
	struct IntuiText ok = { 0, 1, JAM2, 6, 3, NULL, (UBYTE *)"OK", NULL };

	if (!_WBenchMsg || !IntuitionBase)
		printf("%s\n", text);
	else
		AutoRequest(win, &body, NULL, &ok, 0, 0, 8 * strlen(text) + 48, 54);
}

static BOOL confirm(const char *line1, const char *line2)
{
	struct IntuiText t2 = { 0, 1, JAM2, 16, 16, &topaz8, (UBYTE *)line2, NULL };
	struct IntuiText t1 = { 0, 1, JAM2, 16, 6, &topaz8, (UBYTE *)line1, &t2 };
	struct IntuiText yes = { 0, 1, JAM2, 6, 3, &topaz8, (UBYTE *)"Yes", NULL };
	struct IntuiText no = { 0, 1, JAM2, 6, 3, &topaz8, (UBYTE *)"No", NULL };
	int w = 8 * (strlen(line1) > strlen(line2) ? strlen(line1) : strlen(line2)) + 48;
	return AutoRequest(win, &t1, &yes, &no, 0, 0, w < 200 ? 200 : w, 60);
}

/* ---------------------------------------------------------------------- */
/* Hex names                                                              */

static void to_hex(const char *s, char *out, int size)
{
	static const char digits[] = "0123456789abcdef";
	int n = 0;
	if (!*s)
	{
		strcpy(out, "-");
		return;
	}
	while (*s && n + 2 < size)
	{
		out[n++] = digits[(UBYTE)*s >> 4];
		out[n++] = digits[(UBYTE)*s & 15];
		s++;
	}
	out[n] = '\0';
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static void from_hex(const char *hex, char *out, int size)
{
	int n = 0;
	if (strcmp(hex, "-") == 0)
		hex = "";
	while (hex[0] && hex[1] && n < size - 1)
	{
		int a = hex_digit(hex[0]), b = hex_digit(hex[1]);
		if (a < 0 || b < 0)
			break;
		out[n++] = (char)((a << 4) | b);
		hex += 2;
	}
	out[n] = '\0';
}

/* ---------------------------------------------------------------------- */
/* The connection to the Pi: one request, then its answer lines           */

static void a314_cmd(struct A314_IORequest *ior, UWORD command, char *buffer, int length)
{
	ior->a314_Request.io_Command = command;
	ior->a314_Request.io_Error = 0;
	ior->a314_Socket = socket;
	ior->a314_Buffer = (STRPTR)buffer;
	ior->a314_Length = length;
	SendIO((struct IORequest *)ior);
}

/* Waits for ior for up to secs seconds; FALSE on a time-out */
static BOOL wait_io(struct IORequest *ior, ULONG secs)
{
	treq->tr_node.io_Command = TR_ADDREQUEST;
	treq->tr_time.tv_secs = secs;
	treq->tr_time.tv_micro = 0;
	SendIO((struct IORequest *)treq);
	for (;;)
	{
		if (CheckIO(ior))
		{
			WaitIO(ior);
			AbortIO((struct IORequest *)treq);
			WaitIO((struct IORequest *)treq);
			return TRUE;
		}
		if (CheckIO((struct IORequest *)treq))
		{
			WaitIO((struct IORequest *)treq);
			return FALSE;
		}
		Wait(1L << mp->mp_SigBit);
	}
}

/* Ends the connection. a314.device returns every request of the socket
   when it is reset, so all of them are collected here, also those whose
   wait timed out. */
static void disconnect(void)
{
	if (!connected)
		return;
	a314_cmd(cmsg, A314_RESET, NULL, 0);
	WaitIO((struct IORequest *)cmsg);
	if (read_pending)
		WaitIO((struct IORequest *)rmsg);
	if (write_pending)
		WaitIO((struct IORequest *)wmsg);
	read_pending = write_pending = FALSE;
	connected = FALSE;
}

static BOOL connect_pi(void)
{
	if (connected)
		return TRUE;
	socket = ((ULONG)time(NULL) << 4) ^ (ULONG)mp;
	a314_cmd(cmsg, A314_CONNECT, SERVICE_NAME, strlen(SERVICE_NAME));
	if (!wait_io((struct IORequest *)cmsg, 10))
	{
		a314_cmd(wmsg, A314_RESET, NULL, 0);
		WaitIO((struct IORequest *)wmsg);
		WaitIO((struct IORequest *)cmsg);
		return FALSE;
	}
	if (cmsg->a314_Request.io_Error != A314_CONNECT_OK)
		return FALSE;
	connected = TRUE;
	a314_cmd(rmsg, A314_READ, (char *)arbuf, sizeof(arbuf) - 1);
	read_pending = TRUE;
	return TRUE;
}

static BOOL send_cmd(const char *text)
{
	int n = strlen(text);
	if (!connected || n > 251)
		return FALSE;		/* (too long: checked by the callers) */
	memcpy(awbuf, text, n);
	a314_cmd(wmsg, A314_WRITE, awbuf, n);
	if (!wait_io((struct IORequest *)wmsg, 10))
	{
		write_pending = TRUE;	/* collected by disconnect() */
		disconnect();
		return FALSE;
	}
	if (wmsg->a314_Request.io_Error != A314_WRITE_OK)
	{
		disconnect();
		return FALSE;
	}
	return TRUE;
}

/* The next line from the Pi, in line; FALSE if there is none */
static BOOL next_line(void)
{
	int n;
	if (!read_pending)
		return FALSE;
	if (!wait_io((struct IORequest *)rmsg, 10))
	{
		disconnect();		/* the read is still pending: collected there */
		return FALSE;
	}
	if (rmsg->a314_Request.io_Error != A314_READ_OK)
	{
		read_pending = FALSE;
		disconnect();
		return FALSE;
	}
	n = rmsg->a314_Length;
	memcpy(line, arbuf, n);
	line[n] = '\0';
	a314_cmd(rmsg, A314_READ, (char *)arbuf, sizeof(arbuf) - 1);
	return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Talking to the a314prefs service                                       */

static void set_msg(const char *s);

/* Sends a command whose answer is OK or ERR; shows it; TRUE for OK */
static BOOL simple_cmd(const char *text)
{
	int tries;
	BOOL ok = FALSE;
	if (strlen(text) > 251)
	{
		set_msg("The path is too long");
		return FALSE;
	}
	/* Twice, in case the Pi service was restarted since the last command */
	for (tries = 0; tries < 2 && !ok; tries++)
		ok = connect_pi() && send_cmd(text) && next_line();
	if (!ok)
	{
		set_msg("No answer from the Pi");
		return FALSE;
	}
	if (strncmp(line, "OK", 2) == 0)
	{
		set_msg(line[2] ? line + 3 : "");
		return TRUE;
	}
	set_msg(strncmp(line, "ERR ", 4) == 0 ? line + 4 : line);
	return FALSE;
}

static BOOL same_image(struct Image_ *a, struct Image_ *b)
{
	return strcmp(a->path, b->path) == 0 && (!a->path[0] || a->rw == b->rw);
}

static BOOL load_state(void)
{
	char hex[2 * PATH_LEN + 8];
	int i;

	if (!connect_pi() || !send_cmd("DISKS"))
		return FALSE;
	for (i = 0; i < DRIVES; i++)
	{
		drives[i].path[0] = '\0';
		auto_images[i].path[0] = '\0';
	}
	drives_known = FALSE;
	for (;;)
	{
		int unit;
		char rw[4];
		if (!next_line())
			return FALSE;
		if (strcmp(line, "END") == 0)
			break;
		if (sscanf(line, "D %d %3s %500s", &unit, rw, hex) == 3 && unit >= 0 && unit < DRIVES)
		{
			if (rw[0] != '?')
				drives_known = TRUE;
			from_hex(hex, drives[unit].path, PATH_LEN + 1);
			drives[unit].rw = rw[0] == 'r' && rw[1] == 'w';
		}
		else if (sscanf(line, "A %d %3s %500s", &unit, rw, hex) == 3 && unit >= 0 && unit < DRIVES)
		{
			from_hex(hex, auto_images[unit].path, PATH_LEN + 1);
			auto_images[unit].rw = rw[0] == '1';
		}
	}
	for (i = 0; i < DRIVES; i++)
	{
		drives_orig[i] = drives[i];
		at_start[i] = drives[i].path[0] && same_image(&drives[i], &auto_images[i]);
		at_start_changed[i] = FALSE;
	}

	if (!send_cmd("SCSI"))
		return FALSE;
	for (i = 0; i < SCSI_UNITS; i++)
	{
		scsi[i].path[0] = '\0';
		scsi[i].rw = FALSE;
		scsi_changed[i] = FALSE;
	}
	for (;;)
	{
		int unit;
		char rw[4];
		if (!next_line())
			return FALSE;
		if (strcmp(line, "END") == 0)
			break;
		if (sscanf(line, "S %d %3s %500s", &unit, rw, hex) == 3 && unit >= 0 && unit < SCSI_UNITS)
		{
			from_hex(hex, scsi[unit].path, PATH_LEN + 1);
			scsi[unit].rw = rw[0] == '1';
		}
	}
	return TRUE;
}

static BOOL list_files(const char *dir)
{
	char cmd[2 * PATH_LEN + 16], hex[2 * PATH_LEN + 8];
	strcpy(cmd, "FILES ");
	to_hex(dir, cmd + 6, sizeof(cmd) - 6);
	nfiles = 0;
	if (strlen(cmd) > 251)
	{
		set_msg("The path is too long");
		return FALSE;
	}
	if (!(connect_pi() && send_cmd(cmd)) && !(connect_pi() && send_cmd(cmd)))
		return FALSE;
	for (;;)
	{
		char kind[4];
		ULONG size;
		if (!next_line())
			return FALSE;
		if (strcmp(line, "END") == 0)
			return TRUE;
		if (strncmp(line, "ERR", 3) == 0)
		{
			set_msg(line[3] ? line + 4 : "Error");
			return FALSE;
		}
		if (nfiles < MAX_FILES && sscanf(line, "F %3s %lu %500s", kind, &size, hex) == 3)
		{
			files[nfiles].dir = kind[0] == 'd';
			files[nfiles].size = size;
			from_hex(hex, files[nfiles].name, NAME_LEN + 1);
			nfiles++;
		}
	}
}

/* ---------------------------------------------------------------------- */
/* Drawing                                                                */

static void print_at(struct RastPort *r, int x, int y, const char *s, int maxlen)
{
	int n = strlen(s);
	if (n > maxlen)
		n = maxlen;
	Move(r, x, y + font->tf_Baseline);
	Text(r, (STRPTR)s, n);
}

static void clear(struct RastPort *r, int x, int y, int w, int h)
{
	SetAPen(r, 0);
	RectFill(r, x, y, x + w - 1, y + h - 1);
}

static void draw_box(struct RastPort *r, int x, int y, int w, int h)
{
	SetAPen(r, 1);
	Move(r, x, y);
	Draw(r, x + w - 1, y);
	Draw(r, x + w - 1, y + h - 1);
	Draw(r, x, y + h - 1);
	Draw(r, x, y);
}

/* The end of a path, if it doesn't fit */
static void fit_path(const char *path, char *out, int chars)
{
	int n = strlen(path);
	if (n <= chars)
		strcpy(out, path);
	else
	{
		strcpy(out, "...");
		strcpy(out + 3, path + n - (chars - 3));
	}
}

static void set_msg(const char *s)
{
	clear(rp, ox + 8, oy + MSG_Y, INNER_W - 16, 8);
	SetAPen(rp, 1);
	print_at(rp, ox + 8, oy + MSG_Y, s, (INNER_W - 16) / 8);
}

static void draw_check(int row)
{
	BOOL on = row < DRIVES ? at_start[row] : scsi[row - DRIVES].rw;
	int y = oy + (row < DRIVES ? DRIVE_Y + row * ROW_H : SCSI_Y + (row - DRIVES) * ROW_H) + 2;
	clear(rp, ox + CHECK_X + 1, y + 1, 14, 8);
	SetAPen(rp, 1);
	if (on)
		print_at(rp, ox + CHECK_X + 4, y + 1, "X", 1);
}

static void draw_row(int row)
{
	struct Image_ *img = row < DRIVES ? &drives[row] : &scsi[row - DRIVES];
	int y = oy + (row < DRIVES ? DRIVE_Y + row * ROW_H : SCSI_Y + (row - DRIVES) * ROW_H) + 2;
	char text[NAME_W / 8 + 1];

	clear(rp, ox + NAME_X + 1, y + 1, NAME_W - 2, 8);
	SetAPen(rp, 1);
	if (row < DRIVES && !drives_known)
		strcpy(text, "(not in use - mount PD0: first)");
	else if (!img->path[0])
		strcpy(text, row < DRIVES ? "(empty)" : "(none)");
	else
	{
		char name[NAME_W / 8 + 1];
		int room = NAME_W / 8 - 2 - (row < DRIVES && img->rw ? 11 : 0);
		fit_path(img->path, name, room);
		sprintf(text, "%s%s", name, row < DRIVES && img->rw ? " (writable)" : "");
	}
	print_at(rp, ox + NAME_X + 4, y + 1, text, NAME_W / 8 - 1);
	draw_check(row);
}

/* ---------------------------------------------------------------------- */
/* Gadgets                                                                */

static SHORT change_xy[] = { 0,0, CHANGE_W-1,0, CHANGE_W-1,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border change_border = { 0, 0, 1, 0, JAM1, 5, change_xy, NULL };
static SHORT eject_xy[] = { 0,0, EJECT_W-1,0, EJECT_W-1,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border eject_border = { 0, 0, 1, 0, JAM1, 5, eject_xy, NULL };
static SHORT check_xy[] = { 0,0, 15,0, 15,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border check_border = { 0, 0, 1, 0, JAM1, 5, check_xy, NULL };
static SHORT bottom_xy[] = { 0,0, BOTTOM_W-1,0, BOTTOM_W-1,13, 0,13, 0,0 };
static struct Border bottom_border = { 0, 0, 1, 0, JAM1, 5, bottom_xy, NULL };

#define ROWS (DRIVES + SCSI_UNITS)
static struct Gadget change_g[ROWS], eject_g[ROWS], check_g[ROWS], bottom_g[3];
static struct IntuiText change_t[ROWS], eject_t[ROWS], bottom_t[3];
static const char *bottom_labels[3] = { "Save", "Use", "Cancel" };

static void make_button(struct Gadget *g, struct IntuiText *t, const char *label, int x, int y,
	int w, int h, struct Border *border, int id)
{
	memset(g, 0, sizeof(*g));
	g->LeftEdge = x;
	g->TopEdge = y;
	g->Width = w;
	g->Height = h;
	g->Flags = GFLG_GADGHCOMP;
	g->Activation = GACT_RELVERIFY;
	g->GadgetType = GTYP_BOOLGADGET;
	g->GadgetRender = border;
	g->GadgetID = id;
	if (t)
	{
		memset(t, 0, sizeof(*t));
		t->FrontPen = 1;
		t->DrawMode = JAM1;
		t->LeftEdge = (w - 8 * (int)strlen(label)) / 2;
		t->TopEdge = (h - 8) / 2;
		t->ITextFont = &topaz8;
		t->IText = (UBYTE *)label;
		g->GadgetText = t;
	}
}

static void init_gadgets(void)
{
	int i;
	for (i = 0; i < ROWS; i++)
	{
		int y = oy + (i < DRIVES ? DRIVE_Y + i * ROW_H : SCSI_Y + (i - DRIVES) * ROW_H);
		make_button(&change_g[i], &change_t[i], "Change", ox + CHANGE_X, y, CHANGE_W, BUTTON_H,
			&change_border, GID_CHANGE + i);
		make_button(&eject_g[i], &eject_t[i], i < DRIVES ? "Eject" : "Remove", ox + EJECT_X, y,
			EJECT_W, BUTTON_H, &eject_border, GID_EJECT + i);
		make_button(&check_g[i], NULL, "", ox + CHECK_X, y, 16, BUTTON_H, &check_border, GID_CHECK + i);
	}
	for (i = 0; i < 3; i++)
		make_button(&bottom_g[i], &bottom_t[i], bottom_labels[i],
			ox + 8 + i * (INNER_W - 16 - BOTTOM_W) / 2, oy + BOTTOM_Y, BOTTOM_W, 14,
			&bottom_border, GID_SAVE + i);
}

static void draw_all(void)
{
	int i;
	SetAPen(rp, 1);
	print_at(rp, ox + 8, oy + 4, "Disk drives (a314disk.device)", 40);
	print_at(rp, ox + CHECK_X - 12, oy + 4, "At start", 8);
	print_at(rp, ox + 8, oy + SCSI_Y - 12, "Hard disks (a314scsi.device), from the next start", 58);
	print_at(rp, ox + CHECK_X - 12, oy + SCSI_Y - 12, "Writable", 8);
	for (i = 0; i < ROWS; i++)
	{
		char label[8];
		int y = oy + (i < DRIVES ? DRIVE_Y + i * ROW_H : SCSI_Y + (i - DRIVES) * ROW_H);
		if (i < DRIVES)
			sprintf(label, "PD%d:", i);
		else
			sprintf(label, "Unit %d", i - DRIVES);
		SetAPen(rp, 1);
		print_at(rp, ox + LABEL_X, y + 3, label, 6);
		draw_box(rp, ox + NAME_X, y, NAME_W, BUTTON_H);
		draw_row(i);
	}
}

/* ---------------------------------------------------------------------- */
/* The file browser                                                       */

static struct Window *fbw = NULL;
static struct RastPort *frp;
static int fox, foy;
static char fb_dir[PATH_LEN + 1];
static int fb_top, fb_selected;
static BOOL fb_writable;
static struct Gadget fb_scroller, fb_buttons[4];
static struct IntuiText fb_texts[4];
static struct PropInfo fb_scroll_info;
static struct Image fb_knob;
static SHORT fb_xy[] = { 0,0, 79,0, 79,13, 0,13, 0,0 };
static struct Border fb_border = { 0, 0, 1, 0, JAM1, 5, fb_xy, NULL };

static void fb_draw_list(void)
{
	int i;
	char text[NAME_LEN + 16], dir[FB_LIST_W / 8 + 1];

	clear(frp, fox + 8, foy + 6, FB_W - 16, 8);
	SetAPen(frp, 1);
	fit_path(fb_dir[0] ? fb_dir : "(home)", dir, FB_LIST_W / 8 - 4);
	sprintf(text, "In: %s", dir);
	print_at(frp, fox + 8, foy + 6, text, FB_LIST_W / 8);

	for (i = 0; i < FB_ROWS; i++)
	{
		int n = fb_top + i;
		int y = foy + FB_LIST_Y + i * FB_ROW_H;
		clear(frp, fox + FB_LIST_X + 1, y, FB_LIST_W - 2, FB_ROW_H);
		if (n >= nfiles)
			continue;
		if (n == fb_selected)
		{
			SetAPen(frp, 3);
			RectFill(frp, fox + FB_LIST_X + 1, y, fox + FB_LIST_X + FB_LIST_W - 2, y + FB_ROW_H - 1);
		}
		SetAPen(frp, n == fb_selected ? 2 : 1);
		if (files[n].dir)
			sprintf(text, "%.60s/", files[n].name);
		else
			sprintf(text, "%-40.40s %5luK", files[n].name, files[n].size >> 10);
		print_at(frp, fox + FB_LIST_X + 4, y + 1, text, FB_LIST_W / 8 - 1);
	}
	draw_box(frp, fox + FB_LIST_X, foy + FB_LIST_Y - 1, FB_LIST_W, FB_ROWS * FB_ROW_H + 2);
}

static void fb_update_scroller(void)
{
	UWORD body = nfiles > FB_ROWS ? (UWORD)((ULONG)MAXBODY * FB_ROWS / nfiles) : MAXBODY;
	UWORD pot = nfiles > FB_ROWS ? (UWORD)((ULONG)MAXPOT * fb_top / (nfiles - FB_ROWS)) : 0;
	ModifyProp(&fb_scroller, fbw, NULL, AUTOKNOB | FREEVERT | PROPNEWLOOK, 0, pot, 0, body);
}

static void fb_draw_check(void)
{
	clear(frp, fox + 9, foy + FB_H - 33, 14, 8);
	SetAPen(frp, 1);
	if (fb_writable)
		print_at(frp, fox + 12, foy + FB_H - 33, "X", 1);
}

static BOOL fb_enter(const char *dir)
{
	char old[PATH_LEN + 1];
	strcpy(old, fb_dir);
	strncpy(fb_dir, dir, PATH_LEN);
	fb_dir[PATH_LEN] = '\0';
	if (!list_files(fb_dir))
	{
		strcpy(fb_dir, old);
		list_files(fb_dir);
	}
	fb_top = 0;
	fb_selected = -1;
	fb_draw_list();
	fb_update_scroller();
	return TRUE;
}

/* A file browser for disk images on the Pi; TRUE with the chosen file in
   out. writable_choice: show the Writable box (for drives). */
static BOOL choose_file(char *out, BOOL *writable, BOOL writable_choice)
{
	struct NewWindow nw;
	struct IntuiMessage *im;
	BOOL done = FALSE, chosen = FALSE;
	int i;
	static const char *labels[4] = { "Parent", "", "OK", "Cancel" };

	memset(&nw, 0, sizeof(nw));
	nw.LeftEdge = win->LeftEdge + 40;
	nw.TopEdge = win->TopEdge + 6;
	nw.Width = FB_W + win->BorderLeft + win->BorderRight;
	nw.Height = FB_H + win->BorderTop + win->BorderBottom;
	nw.DetailPen = 0;
	nw.BlockPen = 1;
	nw.IDCMPFlags = IDCMP_GADGETUP | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_CLOSEWINDOW;
	nw.Flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_SMART_REFRESH |
		WFLG_NOCAREREFRESH | WFLG_ACTIVATE | WFLG_RMBTRAP;
	nw.Title = (UBYTE *)"Choose a disk image on the Pi";
	nw.Type = WBENCHSCREEN;
	fbw = OpenWindow(&nw);
	if (!fbw)
		return FALSE;
	frp = fbw->RPort;
	SetFont(frp, font);
	SetDrMd(frp, JAM1);
	fox = fbw->BorderLeft;
	foy = fbw->BorderTop;

	memset(&fb_scroller, 0, sizeof(fb_scroller));
	fb_scroller.LeftEdge = fox + FB_LIST_X + FB_LIST_W + 4;
	fb_scroller.TopEdge = foy + FB_LIST_Y;
	fb_scroller.Width = 16;
	fb_scroller.Height = FB_ROWS * FB_ROW_H;
	fb_scroller.Flags = GFLG_GADGHCOMP;
	fb_scroller.Activation = GACT_RELVERIFY | GACT_FOLLOWMOUSE;
	fb_scroller.GadgetType = GTYP_PROPGADGET;
	fb_scroller.GadgetRender = &fb_knob;
	fb_scroller.SpecialInfo = &fb_scroll_info;
	fb_scroller.GadgetID = GID_FB_SCROLL;
	fb_scroll_info.Flags = AUTOKNOB | FREEVERT | PROPNEWLOOK;
	fb_scroll_info.VertBody = MAXBODY;
	AddGadget(fbw, &fb_scroller, -1);

	for (i = 0; i < 4; i++)
	{
		if (i == 1)
		{
			if (!writable_choice)
				continue;
			make_button(&fb_buttons[i], NULL, "", fox + 8, foy + FB_H - 35, 16, BUTTON_H,
				&check_border, GID_FB_WRITE);
		}
		else
			make_button(&fb_buttons[i], &fb_texts[i], labels[i],
				fox + (i == 0 ? 8 : i == 2 ? FB_W - 176 : FB_W - 88), foy + FB_H - 18, 80, 14,
				&fb_border, i == 0 ? GID_FB_PARENT : i == 2 ? GID_FB_OK : GID_FB_CANCEL);
		AddGadget(fbw, &fb_buttons[i], -1);
	}
	RefreshGadgets(fbw->FirstGadget, fbw, NULL);
	if (writable_choice)
	{
		SetAPen(frp, 1);
		print_at(frp, fox + 30, foy + FB_H - 33, "Writable (otherwise read only)", 32);
		fb_writable = *writable;
		fb_draw_check();
	}

	/* Start in the directory of the current file, if any */
	{
		char dir[PATH_LEN + 1], *slash;
		strcpy(dir, out);
		slash = strrchr(dir, '/');
		if (slash)
			*slash = '\0';
		else
			dir[0] = '\0';
		fb_dir[0] = '\0';
		fb_enter(dir);
	}

	while (!done)
	{
		Wait(1L << fbw->UserPort->mp_SigBit);
		while ((im = (struct IntuiMessage *)GetMsg(fbw->UserPort)) != NULL)
		{
			ULONG cls = im->Class;
			UWORD code = im->Code;
			int mx = im->MouseX, my = im->MouseY;
			struct Gadget *g = (struct Gadget *)im->IAddress;
			ReplyMsg((struct Message *)im);

			if (cls == IDCMP_CLOSEWINDOW)
				done = TRUE;
			else if (cls == IDCMP_MOUSEMOVE || (cls == IDCMP_GADGETUP && g->GadgetID == GID_FB_SCROLL))
			{
				int top = nfiles > FB_ROWS ?
					(int)(((ULONG)fb_scroll_info.VertPot * (nfiles - FB_ROWS) + MAXPOT / 2) / MAXPOT) : 0;
				if (top != fb_top)
				{
					fb_top = top;
					fb_draw_list();
				}
			}
			else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN &&
				mx >= fox + FB_LIST_X && mx < fox + FB_LIST_X + FB_LIST_W &&
				my >= foy + FB_LIST_Y && my < foy + FB_LIST_Y + FB_ROWS * FB_ROW_H)
			{
				int n = fb_top + (my - foy - FB_LIST_Y) / FB_ROW_H;
				if (n < nfiles)
				{
					if (files[n].dir)
					{
						char dir[PATH_LEN + 1];
						if (!fb_dir[0])
							strcpy(dir, files[n].name);
						else if (strlen(fb_dir) + strlen(files[n].name) + 1 < PATH_LEN)
							sprintf(dir, "%s/%s", fb_dir, files[n].name);
						else
						{
							set_msg("The path is too long");
							continue;
						}
						fb_enter(dir);
					}
					else
					{
						fb_selected = n;
						fb_draw_list();
					}
				}
			}
			else if (cls == IDCMP_GADGETUP)
			{
				switch (g->GadgetID)
				{
				case GID_FB_PARENT:
				{
					char dir[PATH_LEN + 1], *slash;
					strcpy(dir, fb_dir);
					slash = strrchr(dir, '/');
					if (slash)
						*slash = '\0';
					else
						dir[0] = '\0';
					fb_enter(dir);
					break;
				}
				case GID_FB_WRITE:
					fb_writable = !fb_writable;
					fb_draw_check();
					break;
				case GID_FB_OK:
					if (fb_selected >= 0 && fb_selected < nfiles)
					{
						if (!fb_dir[0])
							strcpy(out, files[fb_selected].name);
						else if (strlen(fb_dir) + strlen(files[fb_selected].name) + 1 <= PATH_LEN)
							sprintf(out, "%s/%s", fb_dir, files[fb_selected].name);
						else
						{
							set_msg("The path is too long");
							break;
						}
						*writable = fb_writable;
						chosen = done = TRUE;
					}
					break;
				case GID_FB_CANCEL:
					done = TRUE;
					break;
				}
			}
		}
	}
	CloseWindow(fbw);
	fbw = NULL;
	/* Clicks on the main window while the browser was open are dropped */
	while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL)
		ReplyMsg((struct Message *)im);
	return chosen;
}

/* ---------------------------------------------------------------------- */
/* Actions                                                                */

/* The last part of a path on the Pi */
static const char *file_part(const char *path)
{
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}

static BOOL insert(int unit, struct Image_ *img)
{
	char cmd[2 * PATH_LEN + 24], msg[64];
	int n = sprintf(cmd, "INSERT %d %d ", unit, img->rw ? 1 : 0);
	to_hex(img->path, cmd + n, sizeof(cmd) - n);
	if (!simple_cmd(cmd))
		return FALSE;
	/* A short message of our own, rather than the Pi's, which names the
	   whole path */
	sprintf(msg, "PD%d: now has %.40s%s", unit, file_part(img->path),
		img->rw ? "" : " (read only)");
	set_msg(msg);
	return TRUE;
}

static BOOL eject(int unit)
{
	char cmd[16];
	sprintf(cmd, "EJECT %d", unit);
	if (!simple_cmd(cmd))
		return FALSE;
	sprintf(cmd, "PD%d: is empty", unit);
	set_msg(cmd);
	return TRUE;
}

static void change_pressed(int row)
{
	struct Image_ img;
	if (row < DRIVES && !drives_known)
	{
		set_msg("The drives are not in use yet: mount PD0: first");
		return;
	}
	img = row < DRIVES ? drives[row] : scsi[row - DRIVES];
	if (!choose_file(img.path, &img.rw, row < DRIVES))
		return;
	if (row < DRIVES)
	{
		if (insert(row, &img))
		{
			drives[row] = img;
			if (at_start[row])
				at_start_changed[row] = TRUE;
		}
	}
	else
	{
		strcpy(scsi[row - DRIVES].path, img.path);
		scsi_changed[row - DRIVES] = TRUE;
		set_msg("Save to use it from the next start");
	}
	draw_row(row);
}

static void eject_pressed(int row)
{
	if (row < DRIVES)
	{
		if (drives_known && drives[row].path[0] && eject(row))
		{
			drives[row].path[0] = '\0';
			if (at_start[row])
			{
				at_start[row] = FALSE;
				at_start_changed[row] = TRUE;
				set_msg("Ejected; Save to start without a disk in it too");
			}
		}
	}
	else if (scsi[row - DRIVES].path[0])
	{
		scsi[row - DRIVES].path[0] = '\0';
		scsi_changed[row - DRIVES] = TRUE;
		set_msg("Save to take it away from the next start");
	}
	draw_row(row);
}

static void check_pressed(int row)
{
	if (row < DRIVES)
	{
		if (!drives[row].path[0])
		{
			set_msg("Put an image in the drive first");
			return;
		}
		at_start[row] = !at_start[row];
		at_start_changed[row] = TRUE;
		set_msg(at_start[row] ? "Save to have this disk in the drive at start" :
			"Save to start without a disk in this drive");
	}
	else
	{
		scsi[row - DRIVES].rw = !scsi[row - DRIVES].rw;
		scsi_changed[row - DRIVES] = TRUE;
	}
	draw_check(row);
}

/* Save: the start-up images and SCSI units that were changed */
static BOOL save_settings(void)
{
	char cmd[2 * PATH_LEN + 24];
	int i, n;
	BOOL ok = TRUE;

	for (i = 0; i < DRIVES; i++)
	{
		if (!at_start_changed[i])
			continue;
		n = sprintf(cmd, "AUTO %d %d ", i, drives[i].rw ? 1 : 0);
		to_hex(at_start[i] ? drives[i].path : "", cmd + n, sizeof(cmd) - n);
		ok = simple_cmd(cmd) && ok;
	}
	if (scsi_changed[0] && !confirm("Hard disk unit 0 is usually the disk the",
		"Amiga starts from. Change it?"))
		return FALSE;
	for (i = 0; i < SCSI_UNITS; i++)
	{
		if (!scsi_changed[i])
			continue;
		n = sprintf(cmd, "SCSISET %d %d ", i, scsi[i].rw ? 1 : 0);
		to_hex(scsi[i].path, cmd + n, sizeof(cmd) - n);
		ok = simple_cmd(cmd) && ok;
	}
	return ok;
}

/* Cancel: the drives as they were */
static void restore_drives(void)
{
	int i;
	if (!drives_known)
		return;
	for (i = 0; i < DRIVES; i++)
	{
		if (same_image(&drives[i], &drives_orig[i]))
			continue;
		if (drives_orig[i].path[0])
			insert(i, &drives_orig[i]);
		else
			eject(i);
	}
}

static BOOL unsaved(void)
{
	int i;
	for (i = 0; i < SCSI_UNITS; i++)
		if (scsi_changed[i])
			return TRUE;
	for (i = 0; i < DRIVES; i++)
		if (at_start_changed[i])
			return TRUE;
	return FALSE;
}

static void handle_window(void)
{
	struct IntuiMessage *im;

	while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL)
	{
		ULONG cls = im->Class;
		struct Gadget *g = (struct Gadget *)im->IAddress;
		ReplyMsg((struct Message *)im);

		if (cls == IDCMP_CLOSEWINDOW)
		{
			restore_drives();
			quit = TRUE;
		}
		else if (cls == IDCMP_GADGETUP)
		{
			int id = g->GadgetID;
			if (id >= GID_CHANGE && id < GID_CHANGE + ROWS)
				change_pressed(id - GID_CHANGE);
			else if (id >= GID_EJECT && id < GID_EJECT + ROWS)
				eject_pressed(id - GID_EJECT);
			else if (id >= GID_CHECK && id < GID_CHECK + ROWS)
				check_pressed(id - GID_CHECK);
			else if (id == GID_SAVE)
			{
				if (save_settings())
					quit = TRUE;
			}
			else if (id == GID_USE)
			{
				if (!unsaved() || confirm("Start-up and hard disk changes are only kept",
					"with Save. Close without saving them?"))
					quit = TRUE;
			}
			else if (id == GID_CANCEL)
			{
				restore_drives();
				quit = TRUE;
			}
		}
	}
}

static BOOL open_window(void)
{
	struct Screen wb;
	struct NewWindow nw;
	int i;

	if (!GetScreenData((APTR)&wb, sizeof(wb), WBENCHSCREEN, NULL))
		return FALSE;

	memset(&nw, 0, sizeof(nw));
	nw.Width = INNER_W + wb.WBorLeft + wb.WBorRight;
	nw.Height = INNER_H + wb.WBorTop + wb.Font->ta_YSize + 1 + wb.WBorBottom;
	nw.LeftEdge = (wb.Width - nw.Width) / 2;
	nw.TopEdge = wb.BarHeight + 1;
	if (nw.LeftEdge < 0)
		nw.LeftEdge = 0;
	if (nw.TopEdge + nw.Height > wb.Height)
		nw.TopEdge = 0;
	nw.DetailPen = 0;
	nw.BlockPen = 1;
	nw.IDCMPFlags = IDCMP_CLOSEWINDOW | IDCMP_GADGETUP;
	nw.Flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
		WFLG_SMART_REFRESH | WFLG_NOCAREREFRESH | WFLG_ACTIVATE | WFLG_RMBTRAP;
	nw.Title = (UBYTE *)"A314 Disks";
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
	for (i = 0; i < ROWS; i++)
	{
		AddGadget(win, &change_g[i], -1);
		AddGadget(win, &eject_g[i], -1);
		AddGadget(win, &check_g[i], -1);
	}
	for (i = 0; i < 3; i++)
		AddGadget(win, &bottom_g[i], -1);
	RefreshGadgets(win->FirstGadget, win, NULL);
	draw_all();
	return TRUE;
}

int main(void)
{
	int rc = 0;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
	GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 0);
	if (!IntuitionBase || !GfxBase)
	{
		rc = 20;
		goto cleanup;
	}

	files = AllocMem(MAX_FILES * sizeof(struct FileEntry), MEMF_PUBLIC | MEMF_CLEAR);
	mp = CreatePort(NULL, 0);
	if (mp)
	{
		cmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
		rmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
		wmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
		treq = (struct timerequest *)CreateExtIO(mp, sizeof(struct timerequest));
	}
	if (!files || !cmsg || !rmsg || !wmsg || !treq)
	{
		report("Not enough memory");
		rc = 20;
		goto cleanup;
	}
	if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0) == 0)
		timer_open = TRUE;
	if (OpenDevice((STRPTR)A314_NAME, 0, (struct IORequest *)cmsg, 0) == 0)
	{
		a314_open = TRUE;
		memcpy(rmsg, cmsg, sizeof(struct A314_IORequest));
		memcpy(wmsg, cmsg, sizeof(struct A314_IORequest));
	}
	if (!timer_open || !a314_open)
	{
		report("a314.device not found - is the A314 software installed?");
		rc = 20;
		goto cleanup;
	}
	if (!connect_pi())
	{
		report(cmsg->a314_Request.io_Error == A314_CONNECT_UNKNOWN_SERVICE ?
			"The a314prefs service is not installed on the Pi" :
			"No answer from the Pi");
		rc = 20;
		goto cleanup;
	}

	font = OpenFont(&topaz8);
	if (!load_state() || !font || !open_window())
	{
		report(font ? "Unable to read the disk settings from the Pi" : "Unable to open the window");
		rc = 20;
		goto cleanup;
	}

	while (!quit)
	{
		Wait(1L << win->UserPort->mp_SigBit);
		handle_window();
	}

cleanup:
	if (a314_open)
	{
		disconnect();
		CloseDevice((struct IORequest *)cmsg);
	}
	if (timer_open)
		CloseDevice((struct IORequest *)treq);
	if (win)
		CloseWindow(win);
	win = NULL;
	if (font)
		CloseFont(font);
	if (treq)
		DeleteExtIO((struct IORequest *)treq);
	if (wmsg)
		DeleteExtIO((struct IORequest *)wmsg);
	if (rmsg)
		DeleteExtIO((struct IORequest *)rmsg);
	if (cmsg)
		DeleteExtIO((struct IORequest *)cmsg);
	if (mp)
		DeletePort(mp);
	if (files)
		FreeMem(files, MAX_FILES * sizeof(struct FileEntry));
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	return rc;
}
