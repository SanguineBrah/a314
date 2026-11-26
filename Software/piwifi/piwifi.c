/*
 * PiWiFi - choose the WiFi network the Raspberry Pi connects to, from the
 * Workbench.
 *
 * Shows the Pi's current connection and the networks it can see, and asks
 * the piwifi service on the Pi to connect to, or forget, a network.
 *
 * The window is made by a314gui (gadtools.library on 2.04 and later).
 * Only needs Kickstart/Workbench 1.3.
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
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../a314device/a314.h"
#include "../prefs/a314gui.h"

#define SERVICE_NAME	"piwifi"

#define MAX_NETS	30
#define MAX_WRITE	252		/* the most a314.device takes in one write */
#define SSID_LEN	32		/* bytes, as UTF-8 */
#define PASS_LEN	63

/* Gadgets and menu items */
#define ID_STATUS	1
#define ID_ADDRESS	2
#define ID_LIST		3
#define ID_SSID		4
#define ID_PASS		5
#define ID_MSG		6
#define ID_SCAN		7
#define ID_CONNECT	8
#define ID_FORGET	9
#define ID_QUIT		20

struct Network
{
	int signal;
	BOOL in_use, saved;
	char security[16];
	char hex[2 * SSID_LEN + 1];	/* the SSID as sent by the Pi */
	char name[SSID_LEN + 1];	/* the SSID for display */
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
static UBYTE awbuf[256];
static char linebuf[256];
static int linelen = 0;
static char sendq[512];		/* lines waiting to be sent */
static int sendq_len = 0;

static struct Network nets[MAX_NETS];
static int nnets = 0;
static struct Network new_nets[MAX_NETS];	/* list being received */
static int nnew = 0;
static char list_text[MAX_NETS][SSID_LEN + 48];
static const char *list_lines[MAX_NETS];
static char chosen_name[SSID_LEN + 1];	/* the network last chosen */
static char chosen_hex[2 * SSID_LEN + 1];
static char cur_state[16] = "";
static char cur_ip[20] = "";
static char cur_name[SSID_LEN + 1] = "";
static int cur_signal = -1;
static BOOL quit = FALSE;

static const struct GuiItem items[] =
{
	{ GUI_TEXT, ID_STATUS, 10, 0, 40, 0, "Status", 0 },
	{ GUI_TEXT, ID_ADDRESS, 10, 1, 40, 0, "Address", 0 },
	{ GUI_LIST, ID_LIST, 0, 3, 50, 5, "Networks the Pi can see", 0 },
	{ GUI_STRING, ID_SSID, 10, 8, 40, 0, "N_etwork", SSID_LEN },
	{ GUI_STRING, ID_PASS, 10, 9, 40, 0, "_Password", PASS_LEN },
	{ GUI_TEXT, ID_MSG, 0, 10, 50, 0, NULL, 0 },
	{ GUI_BUTTON, ID_SCAN, 0, 12, 14, 0, "_Scan", 0 },
	{ GUI_BUTTON, ID_CONNECT, 18, 12, 14, 0, "C_onnect", 0 },
	{ GUI_BUTTON, ID_FORGET, 36, 12, 14, 0, "_Forget", 0 },
};

static const struct GuiMenu menus[] =
{
	{ GUI_MENU_TITLE, 0, 0, "Project" },
	{ GUI_MENU_ITEM, 'S', ID_SCAN, "Scan" },
	{ GUI_MENU_BAR, 0, 0, NULL },
	{ GUI_MENU_ITEM, 'Q', ID_QUIT, "Quit" },
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
/* The window                                                             */

static void set_msg(const char *s)
{
	if (gui)
		gui_set_text(gui, ID_MSG, s);
}

static void draw_status(void)
{
	char buf[64];

	if (!connected)
		strcpy(buf, "Not connected to the Pi");
	else if (strcmp(cur_state, "connected") == 0)
		sprintf(buf, "Connected to %s", cur_name);
	else if (strcmp(cur_state, "connecting") == 0)
		strcpy(buf, "Connecting...");
	else if (strcmp(cur_state, "unavailable") == 0)
		strcpy(buf, "The WiFi adapter is not available");
	else
		strcpy(buf, cur_state[0] ? "Not connected" : "");
	gui_set_text(gui, ID_STATUS, buf);

	buf[0] = 0;
	if (connected && strcmp(cur_state, "connected") == 0)
	{
		if (cur_signal >= 0)
			sprintf(buf, "%s, signal %d%%", cur_ip, cur_signal);
		else
			strcpy(buf, cur_ip);
	}
	gui_set_text(gui, ID_ADDRESS, buf);
}

/* The networks: the one in use marked, with signal, security and whether
   the Pi has its password */
static void show_list(int selected)
{
	int i;
	for (i = 0; i < nnets; i++)
	{
		struct Network *n = &nets[i];
		sprintf(list_text[i], "%s%s  (%d%%, %s%s)", n->in_use ? "\273 " : "", n->name,
			n->signal, n->security, n->saved ? ", saved" : "");
		list_lines[i] = list_text[i];
	}
	gui_set_list(gui, ID_LIST, list_lines, nnets, selected);
}

/* ---------------------------------------------------------------------- */
/* SSIDs and passwords                                                    */

/* hex (UTF-8) to Latin-1 text for display; other characters become '?' */
static void hex_to_text(const char *hex, char *out, int size)
{
	int n = 0;
	while (hex[0] && hex[1] && n < size - 1)
	{
		char pair[3] = { hex[0], hex[1], 0 };
		int c = (int)strtol(pair, NULL, 16);
		hex += 2;
		if ((c == 0xC2 || c == 0xC3) && hex[0] && hex[1])
		{
			char next[3] = { hex[0], hex[1], 0 };
			c = ((c & 3) << 6) | ((int)strtol(next, NULL, 16) & 0x3F);
			hex += 2;
		}
		else if (c >= 0x80)
			c = '?';
		out[n++] = (char)(c < 32 ? '?' : c);
	}
	out[n] = 0;
}

/* Latin-1 text to hex (UTF-8). Returns FALSE if it doesn't fit. */
static BOOL text_to_hex(const char *s, char *out, int size)
{
	int n = 0;
	for (; *s; s++)
	{
		UBYTE c = (UBYTE)*s;
		if (n + (c < 0x80 ? 2 : 4) + 1 > size)
			return FALSE;
		if (c < 0x80)
			n += sprintf(out + n, "%02x", c);
		else
			n += sprintf(out + n, "%02x%02x", 0xC0 | (c >> 6), 0x80 | (c & 0x3F));
	}
	out[n] = 0;
	return TRUE;
}

/* The hex of the SSID in the Network field: the exact bytes the Pi sent if
   it is the chosen network, or else the typed text. */
static BOOL ssid_hex(char *out, int size)
{
	const char *ssid = gui_get_text(gui, ID_SSID);
	if (chosen_hex[0] && strcmp(ssid, chosen_name) == 0)
	{
		strcpy(out, chosen_hex);
		return TRUE;
	}
	return text_to_hex(ssid, out, size);
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

/* Starts connecting to the service. The device answers when the Pi does,
   so this doesn't wait. */
static void start_connect(void)
{
	socket = ((ULONG)time(NULL) << 4) + (socket_seq++ & 15);
	start_a314_cmd(cmsg, A314_CONNECT, SERVICE_NAME, strlen(SERVICE_NAME));
	pending_connect = TRUE;
	set_msg("Connecting to the Pi...");
}

static void handle_connect_completed(void)
{
	BYTE err = cmsg->a314_Request.io_Error;

	pending_connect = FALSE;
	if (err != A314_CONNECT_OK)
	{
		sendq_len = 0;
		set_msg(err == A314_CONNECT_UNKNOWN_SERVICE ?
			"The piwifi service is not installed on the Pi" :
			"Cannot connect to the Pi");
		return;
	}
	connected = TRUE;
	linelen = 0;
	start_read();
	set_msg("");
	draw_status();
}

/* Closes the connection and waits for the device to return every request.
   A reset makes the device return them at once. */
static void disconnect(void)
{
	struct IORequest *xreq = (struct IORequest *)xmsg;

	if (!connected && !pending_connect && !pending_read && !pending_write)
		return;
	xmsg->a314_Request.io_Command = A314_RESET;
	xmsg->a314_Socket = socket;
	DoIO(xreq);
	if (pending_connect)
		WaitIO((struct IORequest *)cmsg);
	if (pending_read)
		WaitIO((struct IORequest *)rmsg);
	if (pending_write)
		WaitIO((struct IORequest *)wmsg);
	pending_connect = pending_read = pending_write = FALSE;
	connected = FALSE;
	sendq_len = 0;
}

static void send_pending(void)
{
	int n;

	if (!connected || pending_write || sendq_len == 0)
		return;
	/* Send whole lines, as many as fit */
	for (n = 0; n < sendq_len; )
	{
		char *nl = memchr(sendq + n, '\n', sendq_len - n);
		int len = nl - (sendq + n) + 1;
		if (n + len > MAX_WRITE)
			break;
		n += len;
	}
	memcpy(awbuf, sendq, n);
	memmove(sendq, sendq + n, sendq_len - n);
	sendq_len -= n;
	start_a314_cmd(wmsg, A314_WRITE, (char *)awbuf, n);
	pending_write = TRUE;
}

/* Queues a line for the Pi, connecting first if necessary. */
static void send_line(const char *s)
{
	int n = strlen(s);
	if (!a314_open)
		return;
	if (n + 1 > MAX_WRITE || sendq_len + n + 1 > sizeof(sendq))
	{
		set_msg("The request is too long");
		return;
	}
	if (!connected && !pending_connect)
		start_connect();
	memcpy(sendq + sendq_len, s, n);
	sendq_len += n;
	sendq[sendq_len++] = '\n';
}

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

static void copy_word(char *dst, const char *src, int size)
{
	strncpy(dst, src ? src : "", size - 1);
	dst[size - 1] = 0;
}

static void handle_line(char *p)
{
	char *cmd = word(&p);
	if (!cmd)
		return;

	if (strcmp(cmd, "CUR") == 0)
	{
		char *signal, *ssid;
		copy_word(cur_state, word(&p), sizeof(cur_state));
		signal = word(&p);
		cur_signal = signal && signal[0] != '-' ? atoi(signal) : -1;
		copy_word(cur_ip, word(&p), sizeof(cur_ip));
		ssid = word(&p);
		if (ssid && ssid[0] != '-')
			hex_to_text(ssid, cur_name, sizeof(cur_name));
		else
			cur_name[0] = 0;
		draw_status();
	}
	else if (strcmp(cmd, "NETS") == 0)
		nnew = 0;
	else if (strcmp(cmd, "NET") == 0 && nnew < MAX_NETS)
	{
		struct Network *n = &new_nets[nnew];
		char *signal = word(&p), *flags = word(&p), *security = word(&p), *ssid = word(&p);
		if (!ssid || strlen(ssid) > 2 * SSID_LEN)
			return;
		n->signal = atoi(signal);
		n->in_use = strchr(flags, 'a') != NULL;
		n->saved = strchr(flags, 's') != NULL;
		copy_word(n->security, security, sizeof(n->security));
		strcpy(n->hex, ssid);
		hex_to_text(ssid, n->name, sizeof(n->name));
		nnew++;
	}
	else if (strcmp(cmd, "END") == 0)
	{
		memcpy(nets, new_nets, sizeof(nets));
		nnets = nnew;
		show_list(-1);
	}
	else if (strcmp(cmd, "BUSY") == 0 || strcmp(cmd, "DONE") == 0)
	{
		if (cmd[0] == 'D')
			word(&p);	/* 0 or 1; the text says what happened */
		while (*p == ' ')
			p++;
		set_msg(p);
	}
}

static void handle_a314_port(void)
{
	struct Message *m;
	BOOL lost = FALSE;
	int i;

	while ((m = GetMsg(mp)) != NULL)
	{
		if (m == (struct Message *)cmsg)
		{
			handle_connect_completed();
			continue;
		}
		if (m == (struct Message *)wmsg)
		{
			pending_write = FALSE;
			if (wmsg->a314_Request.io_Error != A314_WRITE_OK && connected)
				lost = TRUE;
		}
		else if (m == (struct Message *)rmsg)
		{
			pending_read = FALSE;
			if (rmsg->a314_Request.io_Error != A314_READ_OK)
			{
				if (connected)
					lost = TRUE;
				continue;
			}
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
			if (connected && !lost)
				start_read();
		}
	}

	if (lost)
	{
		/* Let the device return the other request, if any */
		disconnect();
		draw_status();
		set_msg("The connection to the Pi was lost - press Scan to try again");
	}
}

/* ---------------------------------------------------------------------- */
/* User input                                                             */

static void connect_pressed(void)
{
	char line[16 + 4 * SSID_LEN + 4 * PASS_LEN + 4], hex[2 * SSID_LEN + 1];
	char q[SSID_LEN + 64];
	const char *ssid = gui_get_text(gui, ID_SSID), *pass;

	if (!ssid[0])
	{
		set_msg("Choose a network from the list, or type its name");
		return;
	}
	if (!ssid_hex(hex, sizeof(hex)))
	{
		set_msg("The network name is too long");
		return;
	}
	sprintf(q, "Connect the Pi to %s?\nConnections to the Pi may be lost for a moment.", ssid);
	if (!gui_ask(gui, q, "Connect|Cancel"))
		return;

	sprintf(line, "CONNECT %s", hex);
	pass = gui_get_text(gui, ID_PASS);
	if (pass[0])
	{
		strcat(line, " ");
		text_to_hex(pass, line + strlen(line), sizeof(line) - strlen(line));
	}
	send_line(line);

	/* Don't leave the password on the screen */
	gui_set_text(gui, ID_PASS, "");
}

static void forget_pressed(void)
{
	char line[16 + 2 * SSID_LEN], hex[2 * SSID_LEN + 1];
	char q[SSID_LEN + 64];
	const char *ssid = gui_get_text(gui, ID_SSID);

	if (!ssid[0] || !ssid_hex(hex, sizeof(hex)))
	{
		set_msg("Choose a saved network from the list");
		return;
	}
	sprintf(q, "Forget %s?\nIts password will be deleted from the Pi.", ssid);
	if (!gui_ask(gui, q, "Forget|Cancel"))
		return;
	sprintf(line, "FORGET %s", hex);
	send_line(line);
}

static void network_chosen(int i)
{
	if (i < 0 || i >= nnets)
		return;
	strcpy(chosen_name, nets[i].name);
	strcpy(chosen_hex, nets[i].hex);
	gui_set_text(gui, ID_SSID, nets[i].name);
	/* A saved network needs no password: otherwise ask for it */
	if (!nets[i].saved && strcmp(nets[i].security, "open") != 0)
		gui_activate(gui, ID_PASS);
}

static void handle_event(struct GuiEvent *ev)
{
	if (ev->type == GUI_EV_CLOSE)
	{
		quit = TRUE;
		return;
	}
	switch (ev->id)
	{
	case ID_LIST:
		network_chosen((int)ev->value);
		break;
	case ID_SSID:
		gui_activate(gui, ID_PASS);
		break;
	case ID_PASS:
	case ID_CONNECT:
		connect_pressed();
		break;
	case ID_SCAN:
		send_line("SCAN");
		break;
	case ID_FORGET:
		forget_pressed();
		break;
	case ID_QUIT:
		quit = TRUE;
		break;
	}
}

/* ---------------------------------------------------------------------- */

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

	gui = gui_open("A314 WiFi", items, sizeof(items) / sizeof(items[0]), menus);
	if (!gui)
	{
		report("Unable to open the window");
		rc = 20;
		goto cleanup;
	}
	draw_status();

	if (OpenDevice((STRPTR)A314_NAME, 0, (struct IORequest *)cmsg, 0) == 0)
	{
		a314_open = TRUE;
		memcpy(rmsg, cmsg, sizeof(struct A314_IORequest));
		memcpy(wmsg, cmsg, sizeof(struct A314_IORequest));
		memcpy(xmsg, cmsg, sizeof(struct A314_IORequest));
		send_line("STATUS");
		send_line("SCAN");
	}
	else
		set_msg("The A314 software (a314.device) is not installed");

	winsig = gui_signal(gui);
	portsig = 1L << mp->mp_SigBit;

	while (!quit)
	{
		struct GuiEvent ev;
		ULONG sig;
		send_pending();
		sig = Wait(winsig | portsig | SIGBREAKF_CTRL_C);
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
