/*
 * Pictures - the A314 picture settings (Prefs/A314/Pictures).
 *
 * Sets the largest picture a314picture.datatype asks the Pi for: larger
 * pictures are made smaller on the Pi, to save memory on the Amiga. The
 * setting is the environment variable A314/PictureSize ("<width>
 * <height>"); Use sets it in ENV:, Save also in ENVARC:, as the
 * Workbench's own preferences editors do.
 *
 * The window is made by a314gui (gadtools.library on 2.04 and later).
 * Only needs Kickstart/Workbench 1.3 (the datatype itself needs 3.0).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <libraries/dos.h>
#include <libraries/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../prefs/a314gui.h"

#define DEFAULT_W	1280
#define DEFAULT_H	1024
#define MIN_SIZE	16
#define MAX_SIZE	8192

/* Gadgets and menu items */
#define ID_TEXT1	1
#define ID_TEXT2	2
#define ID_WIDTH	3
#define ID_HEIGHT	4
#define ID_PIXELS1	5
#define ID_PIXELS2	6
#define ID_SAVE		10
#define ID_USE		11
#define ID_CANCEL	12
#define ID_QUIT		20
#define ID_DEFAULTS	21
#define ID_LAST_SAVED	22
#define ID_RESTORE	23

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;

/* Set by the startup code when started from Workbench */
extern struct WBStartup *_WBenchMsg;

static struct Gui *gui = NULL;
static BOOL quit = FALSE;

static const struct GuiItem items[] =
{
	{ GUI_LABEL, ID_TEXT1, 0, 0, 50, 0, NULL, 0 },
	{ GUI_LABEL, ID_TEXT2, 0, 1, 50, 0, NULL, 0 },
	{ GUI_INTEGER, ID_WIDTH, 18, 2, 10, 0, "Largest _width", 0 },
	{ GUI_LABEL, ID_PIXELS1, 29, 2, 8, 0, NULL, 0 },
	{ GUI_INTEGER, ID_HEIGHT, 18, 3, 10, 0, "Largest _height", 0 },
	{ GUI_LABEL, ID_PIXELS2, 29, 3, 8, 0, NULL, 0 },
	{ GUI_BUTTON, ID_SAVE, 0, 5, 12, 0, "_Save", 0 },
	{ GUI_BUTTON, ID_USE, 19, 5, 12, 0, "_Use", 0 },
	{ GUI_BUTTON, ID_CANCEL, 38, 5, 12, 0, "_Cancel", 0 },
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
/* The setting                                                            */

/* Reads the setting from a directory (ENV: or ENVARC:); the defaults if
   it isn't there */
static void load_setting(const char *dir, LONG *w, LONG *h)
{
	char buf[32], path[48];
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old = me->pr_WindowPtr;
	BPTR fh;
	LONG n;

	*w = DEFAULT_W;
	*h = DEFAULT_H;
	sprintf(path, "%sA314/PictureSize", dir);
	me->pr_WindowPtr = (APTR)-1;
	fh = Open((STRPTR)path, MODE_OLDFILE);
	me->pr_WindowPtr = old;
	if (!fh)
		return;
	n = Read(fh, buf, sizeof(buf) - 1);
	Close(fh);
	if (n > 0)
	{
		LONG a, b;
		buf[n] = '\0';
		if (sscanf(buf, "%ld %ld", &a, &b) == 2 && a >= MIN_SIZE && b >= MIN_SIZE)
		{
			*w = a;
			*h = b;
		}
	}
}

static BOOL write_setting(const char *dir, LONG w, LONG h)
{
	char path[64], value[32];
	BPTR fh, lock;
	LONG n;

	sprintf(path, "%sA314", dir);
	lock = CreateDir((STRPTR)path);	/* fails harmlessly if it exists */
	if (lock)
		UnLock(lock);
	sprintf(path, "%sA314/PictureSize", dir);
	n = sprintf(value, "%ld %ld", w, h);
	fh = Open((STRPTR)path, MODE_NEWFILE);
	if (!fh)
		return FALSE;
	if (Write(fh, value, n) != n)
	{
		Close(fh);
		return FALSE;
	}
	Close(fh);
	return TRUE;
}

static LONG limit(LONG v)
{
	return v < MIN_SIZE ? MIN_SIZE : v > MAX_SIZE ? MAX_SIZE : v;
}

/* Use: ENV: only. Save: ENV: and ENVARC:. */
static BOOL store_setting(BOOL save)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old = me->pr_WindowPtr;
	LONG w = limit(gui_get_number(gui, ID_WIDTH)), h = limit(gui_get_number(gui, ID_HEIGHT));
	BOOL ok;

	me->pr_WindowPtr = (APTR)-1;
	ok = write_setting("ENV:", w, h);
	if (save)
		ok = write_setting("ENVARC:", w, h) && ok;
	me->pr_WindowPtr = old;
	return ok;
}

static void show(LONG w, LONG h)
{
	gui_set_number(gui, ID_WIDTH, w);
	gui_set_number(gui, ID_HEIGHT, h);
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	LONG start_w, start_h, w, h;
	int rc = 0;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
	GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 0);
	if (!IntuitionBase || !GfxBase)
	{
		rc = 20;
		goto cleanup;
	}

	load_setting("ENV:", &start_w, &start_h);
	gui = gui_open("A314 Pictures Preferences", items, sizeof(items) / sizeof(items[0]), menus);
	if (!gui)
	{
		report("Unable to open the window");
		rc = 20;
		goto cleanup;
	}
	gui_set_text(gui, ID_TEXT1, "Larger pictures are made smaller by the Pi");
	gui_set_text(gui, ID_TEXT2, "(WebP, AVIF, HEIC, JPEG XL and TIFF pictures).");
	gui_set_text(gui, ID_PIXELS1, "pixels");
	gui_set_text(gui, ID_PIXELS2, "pixels");
	show(start_w, start_h);

	while (!quit)
	{
		struct GuiEvent ev;
		ULONG sig = Wait(gui_signal(gui) | SIGBREAKF_CTRL_C);
		if (sig & SIGBREAKF_CTRL_C)
			quit = TRUE;
		while (!quit && gui_event(gui, &ev))
		{
			if (ev.type == GUI_EV_CLOSE)
				quit = TRUE;
			else switch (ev.id)
			{
			case ID_SAVE:
			case ID_USE:
				if (store_setting(ev.id == ID_SAVE))
					quit = TRUE;
				else
					report("Unable to store the setting");
				break;
			case ID_CANCEL:
			case ID_QUIT:
				quit = TRUE;
				break;
			case ID_DEFAULTS:
				show(DEFAULT_W, DEFAULT_H);
				break;
			case ID_LAST_SAVED:
				load_setting("ENVARC:", &w, &h);
				show(w, h);
				break;
			case ID_RESTORE:
				show(start_w, start_h);
				break;
			}
		}
	}

cleanup:
	gui_close(gui);
	gui = NULL;
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	return rc;
}
