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
 * - USB memory sticks in the Pi (the drives USB0: to USB3:, if they are
 *   installed) are shown, and Eject makes one safe to pull out. This is
 *   done with DOS packets to the drives' handler (L:a314usb), so it works
 *   whether or not the USB drives are there.
 *
 * As in the Workbench's preferences editors, Save keeps the settings (the
 * start-up images and the SCSI units), Use keeps the drives as they are
 * now without saving, and Cancel puts the drives back as they were.
 *
 * The settings are kept on the Pi; the a314prefs service there reads and
 * changes them (see a314prefs.py). The images are chosen from the files in
 * the Pi user's home directory.
 *
 * The windows are made by a314gui (gadtools.library on 2.04 and later).
 * Only needs Kickstart/Workbench 1.3.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <libraries/dos.h>
#include <libraries/dosextens.h>

#include <proto/alib.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../a314device/a314.h"
#include "a314gui.h"

#define SERVICE_NAME	"a314prefs"

#define DRIVES		4
#define SCSI_UNITS	4
#define ROWS		(DRIVES + SCSI_UNITS)
#define PATH_LEN	250		/* relative to the Pi user's home */
#define MAX_FILES	200
#define NAME_LEN	120		/* the longest the Pi sends */

/* Gadgets and menu items of the main window: + row (drives 0-3, SCSI 4-7) */
#define ID_IMAGE	100
#define ID_CHANGE	10
#define ID_EJECT	20
#define ID_CHECK	30
#define ID_HEAD1	40
#define ID_HEAD2	41
#define ID_HEAD3	42
#define ID_HEAD4	43
#define ID_MSG		44
#define ID_SAVE		45
#define ID_USE		46
#define ID_CANCEL	47
#define ID_HEAD5	48
#define ID_USB		60		/* + 0-3: what is in USB0: to USB3: */
#define ID_USB_EJECT	64		/* + 0-3 */
#define ID_QUIT		50
#define ID_RESTORE	51

/* ... of the file browser */
#define ID_FB_DIR	1
#define ID_FB_LIST	2
#define ID_FB_WRITE	3
#define ID_FB_PARENT	4
#define ID_FB_OK	5
#define ID_FB_CANCEL	6

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

static struct Gui *gui = NULL;

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

#define ROW(name, kind, r, y) \
	{ GUI_TEXT, ID_IMAGE + (r), 7, (y), 33, 0, name, 0 }, \
	{ GUI_BUTTON, ID_CHANGE + (r), 41, (y), 10, 0, "Change...", 0 }, \
	{ GUI_BUTTON, ID_EJECT + (r), 52, (y), 8, 0, kind, 0 }, \
	{ GUI_CHECKBOX, ID_CHECK + (r), 62, (y), 4, 0, NULL, 0 }

#define USB_ROW(name, n, y) \
	{ GUI_TEXT, ID_USB + (n), 7, (y), 44, 0, name, 0 }, \
	{ GUI_BUTTON, ID_USB_EJECT + (n), 52, (y), 8, 0, "Eject", 0 }

static const struct GuiItem items[] =
{
	{ GUI_LABEL, ID_HEAD1, 0, 0, 50, 0, NULL, 0 },
	{ GUI_LABEL, ID_HEAD2, 59, 0, 9, 0, NULL, 0 },
	ROW("PD0:", "Eject", 0, 1), ROW("PD1:", "Eject", 1, 2),
	ROW("PD2:", "Eject", 2, 3), ROW("PD3:", "Eject", 3, 4),
	{ GUI_LABEL, ID_HEAD3, 0, 5, 50, 0, NULL, 0 },
	{ GUI_LABEL, ID_HEAD4, 59, 5, 9, 0, NULL, 0 },
	ROW("Unit 0", "Remove", 4, 6), ROW("Unit 1", "Remove", 5, 7),
	ROW("Unit 2", "Remove", 6, 8), ROW("Unit 3", "Remove", 7, 9),
	{ GUI_LABEL, ID_HEAD5, 0, 10, 50, 0, NULL, 0 },
	USB_ROW("USB0:", 0, 11), USB_ROW("USB1:", 1, 12),
	USB_ROW("USB2:", 2, 13), USB_ROW("USB3:", 3, 14),
	{ GUI_TEXT, ID_MSG, 0, 15, 68, 0, NULL, 0 },
	{ GUI_BUTTON, ID_SAVE, 0, 17, 14, 0, "_Save", 0 },
	{ GUI_BUTTON, ID_USE, 27, 17, 14, 0, "_Use", 0 },
	{ GUI_BUTTON, ID_CANCEL, 54, 17, 14, 0, "_Cancel", 0 },
};

static const struct GuiMenu menus[] =
{
	{ GUI_MENU_TITLE, 0, 0, "Project" },
	{ GUI_MENU_ITEM, 'Q', ID_QUIT, "Quit" },
	{ GUI_MENU_TITLE, 0, 0, "Edit" },
	{ GUI_MENU_ITEM, 'R', ID_RESTORE, "Restore" },
	{ GUI_MENU_END, 0, 0, NULL }
};

/* The file browser, with and without the Writable box */
static const struct GuiItem fb_items[] =
{
	{ GUI_TEXT, ID_FB_DIR, 4, 0, 42, 0, "In", 0 },
	{ GUI_LIST, ID_FB_LIST, 0, 1, 46, 8, NULL, 0 },
	{ GUI_CHECKBOX, ID_FB_WRITE, 0, 9, 4, 0, "_Writable (otherwise read only)", 0 },
	{ GUI_BUTTON, ID_FB_PARENT, 0, 11, 12, 0, "_Parent", 0 },
	{ GUI_BUTTON, ID_FB_OK, 17, 11, 12, 0, "_OK", 0 },
	{ GUI_BUTTON, ID_FB_CANCEL, 34, 11, 12, 0, "_Cancel", 0 },
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

static void set_msg(const char *s)
{
	if (gui)
		gui_set_text(gui, ID_MSG, s);
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
/* The main window                                                        */

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

static void show_row(int row)
{
	struct Image_ *img = row < DRIVES ? &drives[row] : &scsi[row - DRIVES];
	char text[80], name[64];

	if (row < DRIVES && !drives_known)
		strcpy(text, "(not in use: mount PD0: first)");
	else if (!img->path[0])
		strcpy(text, row < DRIVES ? "(empty)" : "(none)");
	else
	{
		fit_path(img->path, name, row < DRIVES && img->rw ? 26 : 38);
		sprintf(text, "%s%s", name, row < DRIVES && img->rw ? " (writable)" : "");
	}
	gui_set_text(gui, ID_IMAGE + row, text);
	gui_set_check(gui, ID_CHECK + row, row < DRIVES ? at_start[row] : scsi[row - DRIVES].rw);
}

static void show_all(void)
{
	int r;
	for (r = 0; r < ROWS; r++)
		show_row(r);
}

/* ---------------------------------------------------------------------- */
/* The file browser                                                       */

static char fb_dir[PATH_LEN + 1];
static char fb_text[MAX_FILES][NAME_LEN + 16];
static const char *fb_lines[MAX_FILES];

static void fb_show(struct Gui *fb)
{
	char dir[48];
	int i;

	fit_path(fb_dir[0] ? fb_dir : "(your home directory on the Pi)", dir, 40);
	gui_set_text(fb, ID_FB_DIR, dir);
	for (i = 0; i < nfiles; i++)
	{
		if (files[i].dir)
			sprintf(fb_text[i], "%.100s/", files[i].name);
		else
			sprintf(fb_text[i], "%.100s  (%luK)", files[i].name, files[i].size >> 10);
		fb_lines[i] = fb_text[i];
	}
	gui_set_list(fb, ID_FB_LIST, fb_lines, nfiles, -1);
}

static void fb_enter(struct Gui *fb, const char *dir)
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
	fb_show(fb);
}

/* A file browser for disk images on the Pi; TRUE with the chosen file in
   out. writable_choice: show the Writable box (for drives). */
static BOOL choose_file(char *out, BOOL *writable, BOOL writable_choice)
{
	struct Gui *fb;
	struct GuiEvent ev;
	BOOL done = FALSE, chosen = FALSE;
	int selected = -1;

	if (writable_choice)
		fb = gui_open("Choose a Disk Image on the Pi", fb_items, 6, NULL);
	else
	{
		/* Without the Writable box */
		static struct GuiItem items2[5];
		memcpy(items2, fb_items, 2 * sizeof(struct GuiItem));
		memcpy(items2 + 2, fb_items + 3, 3 * sizeof(struct GuiItem));
		fb = gui_open("Choose a Hard Disk Image on the Pi", items2, 5, NULL);
	}
	if (!fb)
		return FALSE;
	if (writable_choice)
		gui_set_check(fb, ID_FB_WRITE, *writable);

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
		fb_enter(fb, dir);
	}

	while (!done)
	{
		Wait(gui_signal(fb));
		while (!done && gui_event(fb, &ev))
		{
			if (ev.type == GUI_EV_CLOSE)
			{
				done = TRUE;
				break;
			}
			switch (ev.id)
			{
			case ID_FB_LIST:
			{
				int n = (int)ev.value;
				if (n < 0 || n >= nfiles)
					break;
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
						break;
					}
					selected = -1;
					fb_enter(fb, dir);
				}
				else if (n == selected)
					goto ok;	/* chosen twice: as OK */
				else
					selected = n;
				break;
			}
			case ID_FB_PARENT:
			{
				char dir[PATH_LEN + 1], *slash;
				strcpy(dir, fb_dir);
				slash = strrchr(dir, '/');
				if (slash)
					*slash = '\0';
				else
					dir[0] = '\0';
				selected = -1;
				fb_enter(fb, dir);
				break;
			}
			case ID_FB_OK:
			ok:
				if (selected >= 0 && selected < nfiles)
				{
					if (!fb_dir[0])
						strcpy(out, files[selected].name);
					else if (strlen(fb_dir) + strlen(files[selected].name) + 1 <= PATH_LEN)
						sprintf(out, "%s/%s", fb_dir, files[selected].name);
					else
					{
						set_msg("The path is too long");
						break;
					}
					if (writable_choice)
						*writable = gui_get_check(fb, ID_FB_WRITE);
					chosen = done = TRUE;
				}
				break;
			case ID_FB_CANCEL:
				done = TRUE;
				break;
			}
		}
	}
	gui_close(fb);
	/* Clicks on the main window while the browser was open are dropped */
	while (gui_event(gui, &ev))
		;
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
	show_row(row);
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
	show_row(row);
}

/* An "At start" or "Writable" box was clicked; on is its new state */
static void check_pressed(int row, BOOL on)
{
	if (row < DRIVES)
	{
		if (!drives[row].path[0])
		{
			set_msg("Put an image in the drive first");
			gui_set_check(gui, ID_CHECK + row, FALSE);
			return;
		}
		at_start[row] = on;
		at_start_changed[row] = TRUE;
		set_msg(at_start[row] ? "Save to have this disk in the drive at start" :
			"Save to start without a disk in this drive");
	}
	else
	{
		scsi[row - DRIVES].rw = on;
		scsi_changed[row - DRIVES] = TRUE;
		set_msg("Save to use it from the next start");
	}
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
	if (scsi_changed[0] && !gui_ask(gui, "Hard disk unit 0 is usually the disk the Amiga\nstarts from. Change it?", "Change|Cancel"))
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

/* ---------------------------------------------------------------------- */
/* USB sticks (USB0: to USB3:)                                            */

#define USB_UNITS		4
#define ACTION_USB_EJECT	0x5503	/* L:a314usb's eject packet */
#define USB_REFRESH_SECS	2

#define USB_NONE	0		/* no such drive */
#define USB_EMPTY	1
#define USB_STICK	2

static int usb_state[USB_UNITS] = { -1, -1, -1, -1 };	/* -1: not shown yet */
static char usb_name[USB_UNITS][32];
static BOOL usb_in_use[USB_UNITS];
static struct MsgPort *usb_mp = NULL;		/* packet replies */
static struct MsgPort *usb_tmp = NULL;		/* the refresh timer */
static struct timerequest *usb_treq = NULL;
static BOOL usb_timer_open = FALSE, usb_timer_pending = FALSE;

/* Sends a DOS packet and waits for it (DoPkt() is 2.0 and later) */
static LONG send_packet(struct MsgPort *port, LONG type, LONG arg1, LONG *res2)
{
	struct StandardPacket *sp;
	LONG res1;

	sp = AllocMem(sizeof(struct StandardPacket), MEMF_PUBLIC | MEMF_CLEAR);
	if (!sp)
	{
		*res2 = ERROR_NO_FREE_STORE;
		return DOSFALSE;
	}
	sp->sp_Msg.mn_Node.ln_Name = (char *)&sp->sp_Pkt;
	sp->sp_Pkt.dp_Link = &sp->sp_Msg;
	sp->sp_Pkt.dp_Port = usb_mp;
	sp->sp_Pkt.dp_Type = type;
	sp->sp_Pkt.dp_Arg1 = arg1;
	PutMsg(port, &sp->sp_Msg);
	WaitPort(usb_mp);
	GetMsg(usb_mp);
	res1 = sp->sp_Pkt.dp_Res1;
	*res2 = sp->sp_Pkt.dp_Res2;
	FreeMem(sp, sizeof(struct StandardPacket));
	return res1;
}

/* The handler of USBn:, or NULL if there is no such drive (without
   AmigaDOS asking for a volume of that name) */
static struct MsgPort *usb_port(int n)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old = me->pr_WindowPtr;
	char name[8];
	struct MsgPort *port;

	sprintf(name, "USB%d:", n);
	me->pr_WindowPtr = (APTR)-1;
	port = DeviceProc((STRPTR)name);
	me->pr_WindowPtr = old;
	return port;
}

/* What is in each USB drive, shown if it changed */
static void refresh_usb(void)
{
	struct InfoData *id = AllocMem(sizeof(struct InfoData), MEMF_PUBLIC | MEMF_CLEAR);
	int n;

	if (!id || !usb_mp)
	{
		if (id)
			FreeMem(id, sizeof(struct InfoData));
		return;
	}
	for (n = 0; n < USB_UNITS; n++)
	{
		struct MsgPort *port = usb_port(n);
		int state = USB_NONE;
		char name[32] = "";
		LONG res2;

		if (port && send_packet(port, ACTION_DISK_INFO, MKBADDR(id), &res2))
		{
			if (id->id_DiskType == ID_NO_DISK_PRESENT || !id->id_VolumeNode)
				state = USB_EMPTY;
			else
			{
				struct DeviceList *vol;
				UBYTE *b;
				int len;
				state = USB_STICK;
				Forbid();
				vol = (struct DeviceList *)BADDR(id->id_VolumeNode);
				b = (UBYTE *)BADDR(vol->dl_Name);
				len = b[0] < 31 ? b[0] : 31;
				memcpy(name, b + 1, len);
				name[len] = 0;
				Permit();
				usb_in_use[n] = id->id_InUse != DOSFALSE;
			}
		}
		if (state != usb_state[n] || strcmp(name, usb_name[n]))
		{
			usb_state[n] = state;
			strcpy(usb_name[n], name);
			gui_set_text(gui, ID_USB + n, state == USB_STICK ? name :
				state == USB_EMPTY ? "No stick" : "Not installed");
			gui_disable(gui, ID_USB_EJECT + n, state != USB_STICK);
		}
	}
	FreeMem(id, sizeof(struct InfoData));
}

static void start_usb_timer(void)
{
	if (usb_timer_open && !usb_timer_pending)
	{
		usb_treq->tr_node.io_Command = TR_ADDREQUEST;
		usb_treq->tr_time.tv_secs = USB_REFRESH_SECS;
		usb_treq->tr_time.tv_micro = 0;
		SendIO((struct IORequest *)usb_treq);
		usb_timer_pending = TRUE;
	}
}

static void usb_eject(int n)
{
	struct MsgPort *port;
	char text[120];
	LONG res2;

	refresh_usb();
	if (usb_state[n] != USB_STICK || !(port = usb_port(n)))
		return;
	if (usb_in_use[n])
	{
		sprintf(text, "Files or drawers on %s (USB%d:)\nare still open. Eject it anyway?", usb_name[n], n);
		if (!gui_ask(gui, text, "Eject|Cancel"))
			return;
	}
	if (send_packet(port, ACTION_USB_EJECT, 0, &res2))
	{
		sprintf(text, "%s can be pulled out of the Pi now", usb_name[n]);
		set_msg(text);
	}
	else if (res2 == ERROR_OBJECT_IN_USE)
	{
		sprintf(text, "%s is in use on the Pi, and was not ejected", usb_name[n]);
		set_msg(text);
	}
	else
		set_msg("Unable to eject the stick");
	refresh_usb();
}

static void open_usb(void)
{
	usb_mp = CreatePort(NULL, 0);
	usb_tmp = CreatePort(NULL, 0);
	if (usb_tmp)
		usb_treq = (struct timerequest *)CreateExtIO(usb_tmp, sizeof(struct timerequest));
	if (usb_treq && OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)usb_treq, 0) == 0)
		usb_timer_open = TRUE;
}

static void close_usb(void)
{
	if (usb_timer_pending)
	{
		AbortIO((struct IORequest *)usb_treq);
		WaitIO((struct IORequest *)usb_treq);
	}
	if (usb_timer_open)
		CloseDevice((struct IORequest *)usb_treq);
	if (usb_treq)
		DeleteExtIO((struct IORequest *)usb_treq);
	if (usb_tmp)
		DeletePort(usb_tmp);
	if (usb_mp)
		DeletePort(usb_mp);
}

static void handle_event(struct GuiEvent *ev)
{
	int id = ev->id;

	if (ev->type == GUI_EV_CLOSE)
	{
		restore_drives();
		quit = TRUE;
	}
	else if (id >= ID_IMAGE)
		;
	else if (id >= ID_USB_EJECT && id < ID_USB_EJECT + USB_UNITS)
		usb_eject(id - ID_USB_EJECT);
	else if (id >= ID_CHANGE && id < ID_CHANGE + ROWS)
		change_pressed(id - ID_CHANGE);
	else if (id >= ID_EJECT && id < ID_EJECT + ROWS)
		eject_pressed(id - ID_EJECT);
	else if (id >= ID_CHECK && id < ID_CHECK + ROWS)
		check_pressed(id - ID_CHECK, ev->value != 0);
	else if (id == ID_SAVE)
	{
		if (save_settings())
			quit = TRUE;
	}
	else if (id == ID_USE)
	{
		if (!unsaved() || gui_ask(gui, "Start-up and hard disk changes are only kept\nwith Save. Close without saving them?",
			"Close|Cancel"))
			quit = TRUE;
	}
	else if (id == ID_CANCEL || id == ID_QUIT)
	{
		restore_drives();
		quit = TRUE;
	}
	else if (id == ID_RESTORE)
	{
		int i;
		restore_drives();
		for (i = 0; i < DRIVES; i++)
			drives[i] = drives_orig[i];
		load_state();
		show_all();
		set_msg("The drives and settings are as they were");
	}
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
		report("The A314 software (a314.device) is not installed");
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
	if (!load_state())
	{
		report("Unable to read the disk settings from the Pi");
		rc = 20;
		goto cleanup;
	}

	gui = gui_open("A314 Disks Preferences", items, sizeof(items) / sizeof(items[0]), menus);
	if (!gui)
	{
		report("Unable to open the window");
		rc = 20;
		goto cleanup;
	}
	gui_set_text(gui, ID_HEAD1, "Disk drives (a314disk.device):");
	gui_set_text(gui, ID_HEAD2, "At start");
	gui_set_text(gui, ID_HEAD3, "Hard disks (a314scsi.device), from the next start:");
	gui_set_text(gui, ID_HEAD4, "Writable");
	gui_set_text(gui, ID_HEAD5, "USB memory sticks in the Pi:");
	show_all();
	open_usb();
	refresh_usb();
	start_usb_timer();

	while (!quit)
	{
		struct GuiEvent ev;
		ULONG tsig = usb_tmp ? 1UL << usb_tmp->mp_SigBit : 0;
		ULONG sigs = Wait(gui_signal(gui) | tsig);
		if ((sigs & tsig) && GetMsg(usb_tmp))
		{
			usb_timer_pending = FALSE;
			refresh_usb();
			start_usb_timer();
		}
		while (!quit && gui_event(gui, &ev))
			handle_event(&ev);
	}

cleanup:
	close_usb();
	gui_close(gui);
	gui = NULL;
	if (a314_open)
	{
		disconnect();
		CloseDevice((struct IORequest *)cmsg);
	}
	if (timer_open)
		CloseDevice((struct IORequest *)treq);
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
