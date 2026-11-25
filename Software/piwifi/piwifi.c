/*
 * PiWiFi - choose the WiFi network the Raspberry Pi connects to, from the
 * Workbench.
 *
 * Shows the Pi's current connection and the networks it can see, and asks
 * the piwifi service on the Pi to connect to, or forget, a network.
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
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../a314device/a314.h"

#define SERVICE_NAME	"piwifi"

#define MAX_NETS	30
#define MAX_WRITE	252		/* the most a314.device takes in one write */
#define SSID_LEN	32		/* bytes, as UTF-8 */
#define PASS_LEN	63

/* Layout, relative to the inside of the window borders */
#define INNER_W		432
#define INNER_H		174
#define LIST_X		8
#define LIST_Y		28
#define LIST_W		392
#define ROWS		8
#define ROW_H		10
#define FIELD_X		96
#define FIELD_W		240
#define SSID_Y		116
#define PASS_Y		130
#define BUTTON_Y	146
#define BUTTON_W	80
#define BUTTON_H	14
#define MSG_Y		164

/* Gadget IDs */
#define GID_SCROLL	1
#define GID_SSID	2
#define GID_PASS	3
#define GID_SCAN	4
#define GID_CONNECT	5
#define GID_FORGET	6

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

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };
static struct TextFont *font = NULL;
static struct Window *win = NULL;
static struct RastPort *rp;
static int ox, oy;		/* top left corner inside the window borders */

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
static int top = 0;		/* first row shown */
static int selected = -1;
static char chosen_name[SSID_LEN + 1];	/* the network last clicked */
static char chosen_hex[2 * SSID_LEN + 1];
static char cur_state[16] = "";
static char cur_ip[20] = "";
static char cur_name[SSID_LEN + 1] = "";
static int cur_signal = -1;
static char msg[64] = "";
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
/* Gadgets                                                                */

static char ssid_buf[SSID_LEN + 1], ssid_undo[SSID_LEN + 1];
static char pass_buf[PASS_LEN + 1], pass_undo[PASS_LEN + 1];
static struct StringInfo ssid_info = { (UBYTE *)ssid_buf, (UBYTE *)ssid_undo, 0, SSID_LEN + 1 };
static struct StringInfo pass_info = { (UBYTE *)pass_buf, (UBYTE *)pass_undo, 0, PASS_LEN + 1 };

static SHORT field_xy[] = { -2,-2, FIELD_W+1,-2, FIELD_W+1,9, -2,9, -2,-2 };
static struct Border field_border = { 0, 0, 1, 0, JAM1, 5, field_xy, NULL };
static SHORT button_xy[] = { 0,0, BUTTON_W-1,0, BUTTON_W-1,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border button_border = { 0, 0, 1, 0, JAM1, 5, button_xy, NULL };

static struct Gadget scroller, ssid_gadget, pass_gadget, buttons[3];
static struct PropInfo scroll_info;
static struct Image scroll_knob;
static struct IntuiText button_texts[3];
static const char *button_labels[3] = { "SCAN", "CONNECT", "FORGET" };

static void init_gadgets(void)
{
	int i;

	memset(&scroller, 0, sizeof(scroller));
	scroller.LeftEdge = ox + LIST_X + LIST_W + 4;
	scroller.TopEdge = oy + LIST_Y;
	scroller.Width = 16;
	scroller.Height = ROWS * ROW_H;
	scroller.Flags = GFLG_GADGHCOMP;
	scroller.Activation = GACT_RELVERIFY | GACT_FOLLOWMOUSE;
	scroller.GadgetType = GTYP_PROPGADGET;
	scroller.GadgetRender = &scroll_knob;
	scroller.SpecialInfo = &scroll_info;
	scroller.GadgetID = GID_SCROLL;
	scroll_info.Flags = AUTOKNOB | FREEVERT | PROPNEWLOOK;
	scroll_info.VertBody = MAXBODY;

	memset(&ssid_gadget, 0, sizeof(ssid_gadget));
	ssid_gadget.LeftEdge = ox + FIELD_X;
	ssid_gadget.TopEdge = oy + SSID_Y;
	ssid_gadget.Width = FIELD_W;
	ssid_gadget.Height = 8;
	ssid_gadget.Flags = GFLG_GADGHCOMP;
	ssid_gadget.Activation = GACT_RELVERIFY;
	ssid_gadget.GadgetType = GTYP_STRGADGET;
	ssid_gadget.GadgetRender = &field_border;
	ssid_gadget.SpecialInfo = &ssid_info;
	ssid_gadget.GadgetID = GID_SSID;

	pass_gadget = ssid_gadget;
	pass_gadget.TopEdge = oy + PASS_Y;
	pass_gadget.SpecialInfo = &pass_info;
	pass_gadget.GadgetID = GID_PASS;

	for (i = 0; i < 3; i++)
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
		g->LeftEdge = ox + 8 + i * (BUTTON_W + 16);
		g->TopEdge = oy + BUTTON_Y;
		g->Width = BUTTON_W;
		g->Height = BUTTON_H;
		g->Flags = GFLG_GADGHCOMP;
		g->Activation = GACT_RELVERIFY;
		g->GadgetType = GTYP_BOOLGADGET;
		g->GadgetRender = &button_border;
		g->GadgetText = t;
		g->GadgetID = GID_SCAN + i;
	}
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

static void draw_status(void)
{
	char buf[64];

	clear(0, 2, INNER_W, 22);
	SetAPen(rp, 1);
	if (!connected)
		strcpy(buf, "NOT CONNECTED TO THE PI");
	else if (strcmp(cur_state, "connected") == 0)
		sprintf(buf, "CONNECTED TO %s", cur_name);
	else if (strcmp(cur_state, "connecting") == 0)
		strcpy(buf, "CONNECTING...");
	else if (strcmp(cur_state, "unavailable") == 0)
		strcpy(buf, "THE WIFI ADAPTER IS NOT AVAILABLE");
	else
		strcpy(buf, cur_state[0] ? "NOT CONNECTED" : "");
	print_at(8, 3, buf, 52);

	buf[0] = 0;
	if (connected && strcmp(cur_state, "connected") == 0)
	{
		if (cur_signal >= 0)
			sprintf(buf, "IP %s   SIGNAL %d%%", cur_ip, cur_signal);
		else
			sprintf(buf, "IP %s", cur_ip);
	}
	print_at(8, 13, buf, 52);
}

static void draw_list(void)
{
	char buf[64];
	int r;

	for (r = 0; r < ROWS; r++)
	{
		int i = top + r, y = LIST_Y + r * ROW_H;
		struct Network *n = &nets[i];

		clear(LIST_X, y, LIST_W, ROW_H);
		if (i >= nnets)
			continue;
		if (i == selected)
		{
			SetAPen(rp, 3);
			RectFill(rp, ox + LIST_X, oy + y, ox + LIST_X + LIST_W - 1, oy + y + ROW_H - 1);
		}
		sprintf(buf, "%c %-26.26s %3d%% %-11.11s %s", n->in_use ? '*' : ' ', n->name,
			n->signal, n->security, n->saved ? "S" : "");
		SetAPen(rp, 1);
		print_at(LIST_X + 2, y + 1, buf, LIST_W / 8 - 1);
	}

	/* Frame */
	SetAPen(rp, 1);
	Move(rp, ox + LIST_X - 1, oy + LIST_Y - 1);
	Draw(rp, ox + LIST_X + LIST_W, oy + LIST_Y - 1);
	Draw(rp, ox + LIST_X + LIST_W, oy + LIST_Y + ROWS * ROW_H);
	Draw(rp, ox + LIST_X - 1, oy + LIST_Y + ROWS * ROW_H);
	Draw(rp, ox + LIST_X - 1, oy + LIST_Y - 1);
}

static void update_scroller(void)
{
	UWORD body = nnets > ROWS ? (UWORD)((ULONG)MAXBODY * ROWS / nnets) : MAXBODY;
	UWORD pot = nnets > ROWS ? (UWORD)((ULONG)MAXPOT * top / (nnets - ROWS)) : 0;
	ModifyProp(&scroller, win, NULL, scroll_info.Flags, 0, pot, MAXBODY, body);
}

static void draw_msg(void)
{
	clear(0, MSG_Y, INNER_W, 8);
	SetAPen(rp, 1);
	print_at(8, MSG_Y, msg, 52);
}

static void set_msg(const char *s)
{
	strncpy(msg, s, sizeof(msg) - 1);
	msg[sizeof(msg) - 1] = 0;
	draw_msg();
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

/* The hex of the SSID in the NETWORK field: the exact bytes the Pi sent if
   it is the selected network, or else the typed text. */
static BOOL ssid_hex(char *out, int size)
{
	if (chosen_hex[0] && strcmp(ssid_buf, chosen_name) == 0)
	{
		strcpy(out, chosen_hex);
		return TRUE;
	}
	return text_to_hex(ssid_buf, out, size);
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
	set_msg("CONNECTING TO THE PI...");
}

static void handle_connect_completed(void)
{
	BYTE err = cmsg->a314_Request.io_Error;

	pending_connect = FALSE;
	if (err != A314_CONNECT_OK)
	{
		sendq_len = 0;
		set_msg(err == A314_CONNECT_UNKNOWN_SERVICE ?
			"THE PIWIFI SERVICE IS NOT INSTALLED ON THE PI" :
			"CANNOT CONNECT TO THE PI");
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
		set_msg("THE REQUEST IS TOO LONG");
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
		top = 0;
		selected = -1;
		update_scroller();
		draw_list();
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
		set_msg("CONNECTION TO THE PI LOST - PRESS SCAN TO RETRY");
	}
}

/* ---------------------------------------------------------------------- */
/* User input                                                             */

/* Asks the user to confirm with a standard requester. */
static BOOL confirm(const char *line1, const char *line2)
{
	struct IntuiText t2 = { 0, 1, JAM2, 16, 16, &topaz8, (UBYTE *)line2, NULL };
	struct IntuiText t1 = { 0, 1, JAM2, 16, 6, &topaz8, (UBYTE *)line1, &t2 };
	struct IntuiText yes = { 0, 1, JAM2, 6, 3, &topaz8, (UBYTE *)"Yes", NULL };
	struct IntuiText no = { 0, 1, JAM2, 6, 3, &topaz8, (UBYTE *)"No", NULL };
	int w = 8 * (strlen(line1) > strlen(line2) ? strlen(line1) : strlen(line2)) + 48;
	return AutoRequest(win, &t1, &yes, &no, 0, 0, w < 200 ? 200 : w, 60);
}

static void connect_pressed(void)
{
	char line[16 + 4 * SSID_LEN + 4 * PASS_LEN + 4], hex[2 * SSID_LEN + 1];
	char q[SSID_LEN + 20];

	if (!ssid_buf[0])
	{
		set_msg("CHOOSE A NETWORK FROM THE LIST, OR TYPE ITS NAME");
		return;
	}
	if (!ssid_hex(hex, sizeof(hex)))
	{
		set_msg("THE NETWORK NAME IS TOO LONG");
		return;
	}
	sprintf(q, "Connect the Pi to %s?", ssid_buf);
	if (!confirm(q, "Connections to the Pi may be lost."))
		return;

	sprintf(line, "CONNECT %s", hex);
	if (pass_buf[0])
	{
		strcat(line, " ");
		text_to_hex(pass_buf, line + strlen(line), sizeof(line) - strlen(line));
	}
	send_line(line);

	/* Don't leave the password on the screen */
	pass_buf[0] = 0;
	pass_info.BufferPos = pass_info.DispPos = 0;
	RefreshGadgets(&pass_gadget, win, NULL);
}

static void forget_pressed(void)
{
	char line[16 + 2 * SSID_LEN], hex[2 * SSID_LEN + 1];
	char q[SSID_LEN + 20];

	if (!ssid_buf[0] || !ssid_hex(hex, sizeof(hex)))
	{
		set_msg("CHOOSE A SAVED NETWORK FROM THE LIST");
		return;
	}
	sprintf(q, "Forget %s?", ssid_buf);
	if (!confirm(q, "Its password will be deleted."))
		return;
	sprintf(line, "FORGET %s", hex);
	send_line(line);
}

static void select_row(int y)
{
	int i = top + (y - oy - LIST_Y) / ROW_H;
	if (i < 0 || i >= nnets)
		return;
	selected = i;
	strcpy(ssid_buf, nets[i].name);
	strcpy(chosen_name, nets[i].name);
	strcpy(chosen_hex, nets[i].hex);
	ssid_info.BufferPos = ssid_info.DispPos = 0;
	RefreshGadgets(&ssid_gadget, win, NULL);
	draw_list();
}

static void scrolled(void)
{
	int t = nnets > ROWS ? (int)(((ULONG)scroll_info.VertPot * (nnets - ROWS) + MAXPOT / 2) / MAXPOT) : 0;
	if (t != top)
	{
		top = t;
		draw_list();
	}
}

static void handle_window(void)
{
	struct IntuiMessage *im;

	while ((im = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL)
	{
		ULONG cls = im->Class;
		UWORD code = im->Code;
		int mx = im->MouseX, my = im->MouseY;
		struct Gadget *g = (struct Gadget *)im->IAddress;
		ReplyMsg((struct Message *)im);

		if (cls == IDCMP_CLOSEWINDOW)
			quit = TRUE;
		else if (cls == IDCMP_MOUSEMOVE)
			scrolled();
		else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN &&
			mx >= ox + LIST_X && mx < ox + LIST_X + LIST_W &&
			my >= oy + LIST_Y && my < oy + LIST_Y + ROWS * ROW_H)
			select_row(my);
		else if (cls == IDCMP_GADGETUP)
		{
			switch (g->GadgetID)
			{
			case GID_SCROLL:
				scrolled();
				break;
			case GID_SSID:
				ActivateGadget(&pass_gadget, win, NULL);
				break;
			case GID_PASS:
				connect_pressed();
				break;
			case GID_SCAN:
				send_line("SCAN");
				break;
			case GID_CONNECT:
				connect_pressed();
				break;
			case GID_FORGET:
				forget_pressed();
				break;
			}
		}
	}
}

/* ---------------------------------------------------------------------- */

static BOOL open_window(void)
{
	struct Screen wb;
	struct NewWindow nw;

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
	nw.IDCMPFlags = IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE;
	nw.Flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
		WFLG_SMART_REFRESH | WFLG_NOCAREREFRESH | WFLG_ACTIVATE | WFLG_RMBTRAP;
	nw.Title = (UBYTE *)"PiWiFi";
	nw.Type = WBENCHSCREEN;

	win = OpenWindow(&nw);
	if (!win)
		return FALSE;
	rp = win->RPort;
	SetFont(rp, font);
	SetDrMd(rp, JAM1);

	/* Place the gadgets inside the window's real borders */
	ox = win->BorderLeft;
	oy = win->BorderTop;
	init_gadgets();
	AddGadget(win, &scroller, -1);
	AddGadget(win, &ssid_gadget, -1);
	AddGadget(win, &pass_gadget, -1);
	AddGadget(win, &buttons[0], -1);
	AddGadget(win, &buttons[1], -1);
	AddGadget(win, &buttons[2], -1);
	RefreshGadgets(win->FirstGadget, win, NULL);

	SetAPen(rp, 1);
	print_at(8, SSID_Y, "NETWORK", 7);
	print_at(8, PASS_Y, "PASSWORD", 8);
	draw_list();
	draw_status();
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
		send_line("STATUS");
		send_line("SCAN");
	}
	else
		set_msg("A314.DEVICE NOT FOUND - IS A314 INSTALLED?");

	winsig = 1L << win->UserPort->mp_SigBit;
	portsig = 1L << mp->mp_SigBit;

	while (!quit)
	{
		ULONG sig;
		send_pending();
		sig = Wait(winsig | portsig | SIGBREAKF_CTRL_C);
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
