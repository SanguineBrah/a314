/*
 * Serial - the A314 serial settings (Prefs/A314/Serial).
 *
 * Chooses which Raspberry Pi serial port each unit of a314serial.device
 * uses. The choice is kept in the environment variable A314/SerialUnit<n>:
 * Use sets it in ENV:, Save also in ENVARC: (or, where there is no
 * ENVARC:, as on Workbench 1.3, in SYS:Prefs/Env-Archive), as the
 * Workbench's own preferences editors do. "auto" means the first free
 * port. The list of ports comes from the piserial service on the Pi.
 *
 * Only uses Kickstart/Workbench 1.3 functions.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <workbench/startup.h>
#include <libraries/dos.h>

#include <proto/alib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../a314device/a314.h"

#define SERVICE_NAME	"piserial"

#define UNITS		4
#define MAX_PORTS	16
#define NAME_LEN	127		/* as in a314serial.device */
#define DESC_LEN	60

/* Layout, inside the window borders */
#define INNER_W		560
#define INNER_H		132
#define UNIT_Y		22		/* first unit's button */
#define UNIT_STEP	18
#define CHOICE_X	72
#define CHOICE_W	480
#define BUTTON_H	14
#define MSG_Y		98
#define BUTTON_Y	114
#define BUTTON_W	96

#define GID_UNIT	1		/* 1 to 4 */
#define GID_SAVE	10
#define GID_USE		11
#define GID_CANCEL	12
#define GID_RESCAN	13

struct Port
{
	char name[NAME_LEN + 1];
	char desc[DESC_LEN + 1];
	BOOL free;
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

static struct MsgPort *mp = NULL;
static struct A314_IORequest *cmsg = NULL, *rmsg = NULL, *wmsg = NULL, *xmsg = NULL;
static BOOL a314_open = FALSE, connected = FALSE;
static BOOL pending_connect = FALSE, pending_read = FALSE, pending_write = FALSE;
static ULONG socket;
static int socket_seq = 0;
static UBYTE arbuf[256];
static UBYTE awbuf[16];

static struct Port ports[MAX_PORTS];
static int nports = 0;
static struct Port new_ports[MAX_PORTS];	/* list being received */
static int nnew = 0;
static BOOL have_list = FALSE;

static char choice[UNITS][NAME_LEN + 1];	/* port name, or "" for automatic */
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

/* ---------------------------------------------------------------------- */
/* The settings                                                           */

static void var_name(char *buf, const char *dir, int unit)
{
	sprintf(buf, "%sA314/SerialUnit%d", dir, unit);
}

/* Reads a unit's port name from a file; FALSE if there is none */
static BOOL read_var(const char *path, char *out)
{
	BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
	LONG n, i;

	if (!fh)
		return FALSE;
	n = Read(fh, out, NAME_LEN);
	Close(fh);
	if (n < 0)
		n = 0;
	for (i = 0; i < n && out[i] > ' '; i++)
		;
	out[i] = '\0';
	return TRUE;
}

static BOOL write_var(const char *dir, int unit, const char *value)
{
	char path[64];
	BPTR fh, lock;
	LONG n = strlen(value);

	sprintf(path, "%sA314", dir);
	lock = CreateDir((STRPTR)path);	/* fails harmlessly if it exists */
	if (lock)
		UnLock(lock);
	var_name(path, dir, unit);
	fh = Open((STRPTR)path, MODE_NEWFILE);
	if (!fh)
		return FALSE;
	if (Write(fh, (APTR)value, n) != n)
	{
		Close(fh);
		return FALSE;
	}
	Close(fh);
	return TRUE;
}

/* Where Save keeps the settings: ENVARC:, or on Workbench 1.3 (which has
   no ENVARC:) the directory that is ENVARC: on later versions. */
static const char *archive_dir(void)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old = me->pr_WindowPtr;
	BPTR lock;

	me->pr_WindowPtr = (APTR)-1;	/* no "insert volume" requester */
	lock = Lock((STRPTR)"ENVARC:", ACCESS_READ);
	me->pr_WindowPtr = old;
	if (lock)
	{
		UnLock(lock);
		return "ENVARC:";
	}
	return "SYS:Prefs/Env-Archive/";
}

static void load_settings(void)
{
	char path[64];
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old = me->pr_WindowPtr;
	int u;

	me->pr_WindowPtr = (APTR)-1;
	for (u = 0; u < UNITS; u++)
	{
		var_name(path, "ENV:", u);
		if (!read_var(path, choice[u]))
		{
			var_name(path, archive_dir(), u);
			if (!read_var(path, choice[u]))
				choice[u][0] = '\0';
		}
		if (strcmp(choice[u], "auto") == 0)
			choice[u][0] = '\0';
	}
	me->pr_WindowPtr = old;
}

/* Use: ENV: only. Save: ENV: and the archive. */
static BOOL store_settings(BOOL save)
{
	const char *arch = archive_dir();
	BOOL ok = TRUE;
	int u;

	for (u = 0; u < UNITS; u++)
	{
		const char *v = choice[u][0] ? choice[u] : "auto";
		ok = write_var("ENV:", u, v) && ok;
		if (save)
			ok = write_var(arch, u, v) && ok;
	}
	return ok;
}

/* ---------------------------------------------------------------------- */
/* Gadgets and drawing                                                    */

static SHORT choice_xy[] = { 0,0, CHOICE_W-1,0, CHOICE_W-1,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border choice_border = { 0, 0, 1, 0, JAM1, 5, choice_xy, NULL };
static SHORT button_xy[] = { 0,0, BUTTON_W-1,0, BUTTON_W-1,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border button_border = { 0, 0, 1, 0, JAM1, 5, button_xy, NULL };

static struct Gadget unit_gadgets[UNITS], buttons[4];
static struct IntuiText button_texts[4];
static const char *button_labels[4] = { "Save", "Use", "Cancel", "Rescan" };
static const int button_ids[4] = { GID_SAVE, GID_USE, GID_CANCEL, GID_RESCAN };

static void init_gadgets(void)
{
	int i;

	for (i = 0; i < UNITS; i++)
	{
		struct Gadget *g = &unit_gadgets[i];
		memset(g, 0, sizeof(*g));
		g->LeftEdge = ox + CHOICE_X;
		g->TopEdge = oy + UNIT_Y + i * UNIT_STEP;
		g->Width = CHOICE_W;
		g->Height = BUTTON_H;
		g->Flags = GFLG_GADGHCOMP;
		g->Activation = GACT_RELVERIFY;
		g->GadgetType = GTYP_BOOLGADGET;
		g->GadgetRender = &choice_border;
		g->GadgetID = GID_UNIT + i;
	}

	for (i = 0; i < 4; i++)
	{
		struct Gadget *g = &buttons[i];
		struct IntuiText *t = &button_texts[i];
		memset(t, 0, sizeof(*t));
		t->FrontPen = 1;
		t->DrawMode = JAM1;
		t->LeftEdge = (BUTTON_W - 8 * (int)strlen(button_labels[i])) / 2;
		t->TopEdge = 3;
		t->ITextFont = &topaz8;
		t->IText = (UBYTE *)button_labels[i];

		memset(g, 0, sizeof(*g));
		/* Save on the left, Cancel on the right, as in the Workbench's
		   preferences editors; Rescan goes with the port list */
		if (i < 3)
		{
			g->LeftEdge = ox + 8 + i * (INNER_W - 16 - BUTTON_W) / 2;
			g->TopEdge = oy + BUTTON_Y;
		}
		else
		{
			g->LeftEdge = ox + INNER_W - 8 - BUTTON_W;
			g->TopEdge = oy + MSG_Y - 3;
		}
		g->Width = BUTTON_W;
		g->Height = BUTTON_H;
		g->Flags = GFLG_GADGHCOMP;
		g->Activation = GACT_RELVERIFY;
		g->GadgetType = GTYP_BOOLGADGET;
		g->GadgetRender = &button_border;
		g->GadgetText = t;
		g->GadgetID = button_ids[i];
	}
}

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

static struct Port *find_port(const char *name)
{
	int i;
	for (i = 0; i < nports; i++)
		if (strcmp(ports[i].name, name) == 0)
			return &ports[i];
	return NULL;
}

static void draw_unit(int u)
{
	char text[NAME_LEN + 40];
	struct Port *p;
	int y = UNIT_Y + u * UNIT_STEP;

	clear(CHOICE_X + 1, y + 1, CHOICE_W - 2, BUTTON_H - 2);
	if (!choice[u][0])
		strcpy(text, "Automatic (the first free port)");
	else if ((p = find_port(choice[u])) != NULL)
		sprintf(text, "%s%s", p->desc, p->free ? "" : "  (in use)");
	else
		sprintf(text, "%s%s", choice[u], have_list ? "  (not connected)" : "");
	SetAPen(rp, 1);
	print_at(CHOICE_X + 8, y + 3, text, (CHOICE_W - 16) / 8);
}

static void draw_all(void)
{
	int u;
	for (u = 0; u < UNITS; u++)
		draw_unit(u);
}

static void set_msg(const char *s)
{
	clear(8, MSG_Y, INNER_W - 24 - BUTTON_W, 8);
	SetAPen(rp, 1);
	print_at(8, MSG_Y, s, (INNER_W - 24 - BUTTON_W) / 8);
}

/* The next choice for a unit: automatic, then each port that no other
   unit has, then automatic again. A chosen port that isn't plugged in is
   shown as such, and kept unless the unit is changed. */
static void next_choice(int u)
{
	int i, start = -1, k;

	if (choice[u][0])
		for (i = 0; i < nports; i++)
			if (strcmp(ports[i].name, choice[u]) == 0)
				start = i;
	for (i = start + 1; i < nports; i++)
	{
		for (k = 0; k < UNITS; k++)
			if (k != u && strcmp(choice[k], ports[i].name) == 0)
				break;
		if (k == UNITS)
		{
			strcpy(choice[u], ports[i].name);
			return;
		}
	}
	choice[u][0] = '\0';
}

/* ---------------------------------------------------------------------- */
/* A314 connection: asks the Pi for its ports, once                       */

static void start_a314_cmd(struct A314_IORequest *ior, UWORD command, char *buffer, int length)
{
	ior->a314_Request.io_Command = command;
	ior->a314_Request.io_Error = 0;
	ior->a314_Socket = socket;
	ior->a314_Buffer = (STRPTR)buffer;
	ior->a314_Length = length;
	SendIO((struct IORequest *)ior);
}

static void disconnect(void)
{
	if (!connected && !pending_connect && !pending_read && !pending_write)
		return;
	xmsg->a314_Request.io_Command = A314_RESET;
	xmsg->a314_Socket = socket;
	DoIO((struct IORequest *)xmsg);
	if (pending_connect)
		WaitIO((struct IORequest *)cmsg);
	if (pending_read)
		WaitIO((struct IORequest *)rmsg);
	if (pending_write)
		WaitIO((struct IORequest *)wmsg);
	pending_connect = pending_read = pending_write = FALSE;
	connected = FALSE;
}

static void ask_for_ports(void)
{
	if (!a314_open)
		return;
	disconnect();
	socket = ((ULONG)time(NULL) << 4) + (socket_seq++ & 15);
	start_a314_cmd(cmsg, A314_CONNECT, SERVICE_NAME, strlen(SERVICE_NAME));
	pending_connect = TRUE;
	nnew = 0;
	set_msg("Asking the Pi for its serial ports...");
}

/* "PORT <free> <name> <description>" or "END" */
static void handle_line(char *p, int len)
{
	p[len] = '\0';
	if (strncmp(p, "PORT ", 5) == 0 && nnew < MAX_PORTS)
	{
		struct Port *np = &new_ports[nnew];
		char *name = p + 7, *end;
		if (len < 8)
			return;
		np->free = p[5] == '1';
		end = strchr(name, ' ');
		if (end)
			*end++ = '\0';
		strncpy(np->name, name, NAME_LEN);
		np->name[NAME_LEN] = '\0';
		strncpy(np->desc, end ? end : name, DESC_LEN);
		np->desc[DESC_LEN] = '\0';
		nnew++;
	}
	else if (strcmp(p, "END") == 0)
	{
		char text[48];
		memcpy(ports, new_ports, sizeof(ports));
		nports = nnew;
		have_list = TRUE;
		disconnect();
		if (nports == 0)
			strcpy(text, "No serial ports found on the Pi");
		else
			sprintf(text, "%d serial port%s found on the Pi", nports, nports == 1 ? "" : "s");
		set_msg(text);
		draw_all();
	}
}

static void handle_a314_port(void)
{
	struct Message *m;

	while ((m = GetMsg(mp)) != NULL)
	{
		if (m == (struct Message *)cmsg)
		{
			pending_connect = FALSE;
			if (cmsg->a314_Request.io_Error != A314_CONNECT_OK)
			{
				set_msg(cmsg->a314_Request.io_Error == A314_CONNECT_UNKNOWN_SERVICE ?
					"The piserial service is not installed on the Pi" :
					"Cannot connect to the Pi");
				continue;
			}
			connected = TRUE;
			start_a314_cmd(rmsg, A314_READ, (char *)arbuf, sizeof(arbuf) - 1);
			pending_read = TRUE;
			strcpy((char *)awbuf, "CPORTS");
			start_a314_cmd(wmsg, A314_WRITE, (char *)awbuf, 6);
			pending_write = TRUE;
		}
		else if (m == (struct Message *)wmsg)
			pending_write = FALSE;
		else if (m == (struct Message *)rmsg)
		{
			pending_read = FALSE;
			if (rmsg->a314_Request.io_Error != A314_READ_OK)
			{
				disconnect();
				set_msg("The connection to the Pi was lost");
				continue;
			}
			if (rmsg->a314_Length > 0 && arbuf[0] == 'C')
				handle_line((char *)arbuf + 1, rmsg->a314_Length - 1);
			if (connected)
			{
				start_a314_cmd(rmsg, A314_READ, (char *)arbuf, sizeof(arbuf) - 1);
				pending_read = TRUE;
			}
		}
	}
}

/* ---------------------------------------------------------------------- */

static void handle_window(void)
{
	struct IntuiMessage *im;

	while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL)
	{
		ULONG cls = im->Class;
		struct Gadget *g = (struct Gadget *)im->IAddress;
		ReplyMsg((struct Message *)im);

		if (cls == IDCMP_CLOSEWINDOW)
			quit = TRUE;
		else if (cls == IDCMP_GADGETUP)
		{
			int id = g->GadgetID;
			if (id >= GID_UNIT && id < GID_UNIT + UNITS)
			{
				next_choice(id - GID_UNIT);
				draw_unit(id - GID_UNIT);
			}
			else if (id == GID_SAVE || id == GID_USE)
			{
				if (store_settings(id == GID_SAVE))
					quit = TRUE;
				else
					report("Unable to store the settings");
			}
			else if (id == GID_CANCEL)
				quit = TRUE;
			else if (id == GID_RESCAN)
				ask_for_ports();
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
	nw.TopEdge = wb.BarHeight + 2;
	if (nw.LeftEdge < 0 || nw.TopEdge + nw.Height > wb.Height)
		nw.LeftEdge = nw.TopEdge = 0;
	nw.DetailPen = 0;
	nw.BlockPen = 1;
	nw.IDCMPFlags = IDCMP_CLOSEWINDOW | IDCMP_GADGETUP;
	nw.Flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
		WFLG_SMART_REFRESH | WFLG_NOCAREREFRESH | WFLG_ACTIVATE | WFLG_RMBTRAP;
	nw.Title = (UBYTE *)"A314 Serial";
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
	for (i = 0; i < UNITS; i++)
		AddGadget(win, &unit_gadgets[i], -1);
	for (i = 0; i < 4; i++)
		AddGadget(win, &buttons[i], -1);
	RefreshGadgets(win->FirstGadget, win, NULL);

	SetAPen(rp, 1);
	print_at(8, 6, "The Pi serial port used by each unit of a314serial.device:", 68);
	for (i = 0; i < UNITS; i++)
	{
		char label[8];
		sprintf(label, "Unit %d", i);
		print_at(8, UNIT_Y + i * UNIT_STEP + 3, label, 6);
	}
	draw_all();
	return TRUE;
}

int main(void)
{
	ULONG winsig, portsig;
	int rc = 0;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
	GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 0);
	if (!IntuitionBase || !GfxBase)
	{
		rc = 20;
		goto cleanup;
	}

	mp = CreatePort(NULL, 0);
	if (mp)
	{
		cmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
		rmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
		wmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
		xmsg = (struct A314_IORequest *)CreateExtIO(mp, sizeof(struct A314_IORequest));
	}
	if (!cmsg || !rmsg || !wmsg || !xmsg)
	{
		report("Not enough memory");
		rc = 20;
		goto cleanup;
	}

	load_settings();

	font = OpenFont(&topaz8);
	if (!font || !open_window())
	{
		report("Unable to open the window");
		rc = 20;
		goto cleanup;
	}

	if (OpenDevice((STRPTR)A314_NAME, 0, (struct IORequest *)cmsg, 0) == 0)
	{
		a314_open = TRUE;
		memcpy(rmsg, cmsg, sizeof(struct A314_IORequest));
		memcpy(wmsg, cmsg, sizeof(struct A314_IORequest));
		memcpy(xmsg, cmsg, sizeof(struct A314_IORequest));
		ask_for_ports();
	}
	else
		set_msg("a314.device not found - is the A314 software installed?");

	winsig = 1L << win->UserPort->mp_SigBit;
	portsig = 1L << mp->mp_SigBit;

	while (!quit)
	{
		ULONG sig = Wait(winsig | portsig | SIGBREAKF_CTRL_C);
		if (sig & portsig)
			handle_a314_port();
		if (sig & winsig)
			handle_window();
		if (sig & SIGBREAKF_CTRL_C)
			quit = TRUE;
	}

cleanup:
	if (a314_open)
	{
		disconnect();
		CloseDevice((struct IORequest *)cmsg);
	}
	if (win)
		CloseWindow(win);
	win = NULL;
	if (font)
		CloseFont(font);
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
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	return rc;
}
