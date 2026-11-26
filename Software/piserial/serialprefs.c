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
 * The window is made by a314gui (gadtools.library on 2.04 and later).
 * Only needs Kickstart/Workbench 1.3.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <libraries/dos.h>
#include <dos/dosextens.h>

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
#include "../prefs/a314gui.h"

#define SERVICE_NAME	"piserial"

#define UNITS		4
#define MAX_PORTS	16
#define NAME_LEN	127		/* as in a314serial.device */
#define DESC_LEN	60

/* Gadgets and menu items */
#define ID_UNIT		1		/* 1 to 4 */
#define ID_MSG		10
#define ID_RESCAN	11
#define ID_SAVE		12
#define ID_USE		13
#define ID_CANCEL	14
#define ID_QUIT		20
#define ID_DEFAULTS	21
#define ID_LAST_SAVED	22
#define ID_RESTORE	23

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

static struct Gui *gui = NULL;

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
static char start_choice[UNITS][NAME_LEN + 1];	/* as when the editor started */
static BOOL quit = FALSE;

/* The choices of each unit's cycle gadget: automatic, the ports, and a
   chosen port that isn't plugged in */
static char labels_text[UNITS][MAX_PORTS + 2][DESC_LEN + 24];
static const char *labels[UNITS][MAX_PORTS + 2];
static int nlabels[UNITS];

static const struct GuiItem items[] =
{
	{ GUI_LABEL, ID_MSG + 100, 0, 0, 45, 0, "", 0 },
	{ GUI_BUTTON, ID_RESCAN, 47, 0, 11, 0, "_Rescan", 0 },
	{ GUI_CYCLE, ID_UNIT + 0, 9, 1, 49, 0, "Unit _0", 0 },
	{ GUI_CYCLE, ID_UNIT + 1, 9, 2, 49, 0, "Unit _1", 0 },
	{ GUI_CYCLE, ID_UNIT + 2, 9, 3, 49, 0, "Unit _2", 0 },
	{ GUI_CYCLE, ID_UNIT + 3, 9, 4, 49, 0, "Unit _3", 0 },
	{ GUI_TEXT, ID_MSG, 0, 5, 58, 0, NULL, 0 },
	{ GUI_BUTTON, ID_SAVE, 0, 7, 12, 0, "_Save", 0 },
	{ GUI_BUTTON, ID_USE, 23, 7, 12, 0, "_Use", 0 },
	{ GUI_BUTTON, ID_CANCEL, 46, 7, 12, 0, "_Cancel", 0 },
};

static const struct GuiMenu menus[] =
{
	{ GUI_MENU_TITLE, 0, 0, "Project" },
	{ GUI_MENU_ITEM, 'Q', ID_QUIT, "Quit" },
	{ GUI_MENU_TITLE, 0, 0, "Edit" },
	{ GUI_MENU_ITEM, 'D', ID_DEFAULTS, "Reset to Defaults" },
	{ GUI_MENU_ITEM, 'L', ID_LAST_SAVED, "Last Saved" },
	{ GUI_MENU_ITEM, 'R', ID_RESTORE, "Restore" },
	{ GUI_MENU_END, 0, 0, NULL }
};

/* Shows a message in the shell, or in a requester when started from
   Workbench (where there is no shell) */
static void report(const char *text)
{
	if (!_WBenchMsg || !IntuitionBase)
		printf("%s\n", text);
	else
		gui_message(gui, text);
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

/* Reads the settings: from ENV:, or failing that from the archive (or
   only from the archive, for Last Saved) */
static void load_settings(BOOL archive_only)
{
	char path[64];
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old = me->pr_WindowPtr;
	int u;

	me->pr_WindowPtr = (APTR)-1;
	for (u = 0; u < UNITS; u++)
	{
		BOOL found = FALSE;
		if (!archive_only)
		{
			var_name(path, "ENV:", u);
			found = read_var(path, choice[u]);
		}
		if (!found)
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
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old = me->pr_WindowPtr;
	const char *arch = archive_dir();
	BOOL ok = TRUE;
	int u;

	/* A missing ENV: is reported below, not with an "insert volume"
	   requester */
	me->pr_WindowPtr = (APTR)-1;
	for (u = 0; u < UNITS; u++)
	{
		const char *v = choice[u][0] ? choice[u] : "auto";
		ok = write_var("ENV:", u, v) && ok;
		if (save)
			ok = write_var(arch, u, v) && ok;
	}
	me->pr_WindowPtr = old;
	return ok;
}

/* ---------------------------------------------------------------------- */
/* The window                                                             */

static struct Port *find_port(const char *name)
{
	int i;
	for (i = 0; i < nports; i++)
		if (strcmp(ports[i].name, name) == 0)
			return &ports[i];
	return NULL;
}

static void set_msg(const char *s)
{
	gui_set_text(gui, ID_MSG, s);
}

/* Fills a unit's cycle gadget: automatic, every port, and the chosen
   port if it isn't plugged in */
static void show_unit(int u)
{
	int i, n = 0, active = 0;

	strcpy(labels_text[u][n++], "Automatic (the first free port)");
	for (i = 0; i < nports; i++)
	{
		sprintf(labels_text[u][n], "%s%s", ports[i].desc, ports[i].free ? "" : "  (in use)");
		if (choice[u][0] && strcmp(choice[u], ports[i].name) == 0)
			active = n;
		n++;
	}
	if (choice[u][0] && !find_port(choice[u]))
	{
		sprintf(labels_text[u][n], "%.60s%s", choice[u], have_list ? "  (not connected)" : "");
		active = n++;
	}
	for (i = 0; i < n; i++)
		labels[u][i] = labels_text[u][i];
	labels[u][n] = NULL;
	nlabels[u] = n;
	gui_set_cycle(gui, ID_UNIT + u, labels[u], n, active);
}

static void show_all(void)
{
	int u;
	for (u = 0; u < UNITS; u++)
		show_unit(u);
}

/* A unit's cycle gadget was changed to choice a. A port is used by one
   unit only, so a unit that had it goes back to automatic. */
static void unit_changed(int u, int a)
{
	int k;

	if (a <= 0)
		choice[u][0] = '\0';
	else if (a - 1 < nports)
	{
		strcpy(choice[u], ports[a - 1].name);
		for (k = 0; k < UNITS; k++)
			if (k != u && strcmp(choice[k], choice[u]) == 0)
			{
				choice[k][0] = '\0';
				show_unit(k);
			}
	}
	/* else the port that isn't plugged in: kept */
}

/* ---------------------------------------------------------------------- */
/* A314 connection: asks the Pi for its ports                             */

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
		show_all();
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

static void handle_event(struct GuiEvent *ev)
{
	int u;

	if (ev->type == GUI_EV_CLOSE)
		quit = TRUE;
	else if (ev->type == GUI_EV_GADGET && ev->id >= ID_UNIT && ev->id < ID_UNIT + UNITS)
		unit_changed(ev->id - ID_UNIT, (int)ev->value);
	else
	{
		switch (ev->id)
		{
		case ID_SAVE:
		case ID_USE:
			if (store_settings(ev->id == ID_SAVE))
				quit = TRUE;
			else
				report("Unable to store the settings");
			break;
		case ID_CANCEL:
		case ID_QUIT:
			quit = TRUE;
			break;
		case ID_RESCAN:
			ask_for_ports();
			break;
		case ID_DEFAULTS:
			for (u = 0; u < UNITS; u++)
				choice[u][0] = '\0';
			show_all();
			break;
		case ID_LAST_SAVED:
			load_settings(TRUE);
			show_all();
			break;
		case ID_RESTORE:
			memcpy(choice, start_choice, sizeof(choice));
			show_all();
			break;
		}
	}
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

	load_settings(FALSE);
	memcpy(start_choice, choice, sizeof(choice));

	gui = gui_open("A314 Serial Preferences", items, sizeof(items) / sizeof(items[0]), menus);
	if (!gui)
	{
		report("Unable to open the window");
		rc = 20;
		goto cleanup;
	}
	gui_set_text(gui, ID_MSG + 100, "The Raspberry Pi serial port of each unit:");
	show_all();

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

	winsig = gui_signal(gui);
	portsig = 1L << mp->mp_SigBit;

	while (!quit)
	{
		struct GuiEvent ev;
		ULONG sig = Wait(winsig | portsig | SIGBREAKF_CTRL_C);
		if (sig & portsig)
			handle_a314_port();
		if (sig & winsig)
			while (!quit && gui_event(gui, &ev))
				handle_event(&ev);
		if (sig & SIGBREAKF_CTRL_C)
			quit = TRUE;
	}

cleanup:
	if (a314_open)
	{
		disconnect();
		CloseDevice((struct IORequest *)cmsg);
	}
	gui_close(gui);
	gui = NULL;
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
