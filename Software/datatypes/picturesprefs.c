/*
 * Pictures - the A314 picture settings (Prefs/A314/Pictures).
 *
 * Sets the largest picture a314picture.datatype asks the Pi for: larger
 * pictures are made smaller on the Pi, to save memory on the Amiga. The
 * setting is the environment variable A314/PictureSize ("<width>
 * <height>"); Use sets it in ENV:, Save also in ENVARC:, as the
 * Workbench's own preferences editors do.
 *
 * Only uses Kickstart/Workbench 1.3 functions (the datatype itself needs
 * 3.0).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
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

#define DEFAULT_W	1280
#define DEFAULT_H	1024
#define MIN_SIZE	16
#define MAX_SIZE	8192

#define INNER_W		400
#define INNER_H		104
#define FIELD_X		208
#define FIELD_W		64
#define WIDTH_Y		30
#define HEIGHT_Y	46
#define BUTTON_Y	84
#define BUTTON_W	96
#define BUTTON_H	14

#define GID_WIDTH	1
#define GID_HEIGHT	2
#define GID_SAVE	10
#define GID_USE		11
#define GID_CANCEL	12

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;

/* Set by the startup code when started from Workbench */
extern struct WBStartup *_WBenchMsg;

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };
static struct TextFont *font = NULL;
static struct Window *win = NULL;
static struct RastPort *rp;
static int ox, oy;
static BOOL quit = FALSE;

static char width_buf[8], height_buf[8], undo_buf[8];
static struct StringInfo width_info = { (UBYTE *)width_buf, (UBYTE *)undo_buf, 0, sizeof(width_buf) };
static struct StringInfo height_info = { (UBYTE *)height_buf, (UBYTE *)undo_buf, 0, sizeof(height_buf) };
static SHORT field_xy[] = { -2,-2, FIELD_W+1,-2, FIELD_W+1,9, -2,9, -2,-2 };
static struct Border field_border = { 0, 0, 1, 0, JAM1, 5, field_xy, NULL };
static SHORT button_xy[] = { 0,0, BUTTON_W-1,0, BUTTON_W-1,BUTTON_H-1, 0,BUTTON_H-1, 0,0 };
static struct Border button_border = { 0, 0, 1, 0, JAM1, 5, button_xy, NULL };
static struct Gadget width_gadget, height_gadget, buttons[3];
static struct IntuiText button_texts[3];
static const char *button_labels[3] = { "Save", "Use", "Cancel" };

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
/* The setting                                                            */

static void load_setting(LONG *w, LONG *h)
{
	char buf[32];
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old = me->pr_WindowPtr;
	BPTR fh;
	LONG n;

	*w = DEFAULT_W;
	*h = DEFAULT_H;
	me->pr_WindowPtr = (APTR)-1;
	fh = Open((STRPTR)"ENV:A314/PictureSize", MODE_OLDFILE);
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
	lock = CreateDir((STRPTR)path);
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

static LONG field_value(struct StringInfo *si)
{
	LONG v = atol((char *)si->Buffer);
	return v < MIN_SIZE ? MIN_SIZE : v > MAX_SIZE ? MAX_SIZE : v;
}

static BOOL store_setting(BOOL save)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR old = me->pr_WindowPtr;
	LONG w = field_value(&width_info), h = field_value(&height_info);
	BOOL ok;

	me->pr_WindowPtr = (APTR)-1;
	ok = write_setting("ENV:", w, h);
	if (save)
		ok = write_setting("ENVARC:", w, h) && ok;
	me->pr_WindowPtr = old;
	return ok;
}

/* ---------------------------------------------------------------------- */
/* The window                                                             */

static void init_gadgets(void)
{
	int i;

	memset(&width_gadget, 0, sizeof(width_gadget));
	width_gadget.LeftEdge = ox + FIELD_X;
	width_gadget.TopEdge = oy + WIDTH_Y;
	width_gadget.Width = FIELD_W;
	width_gadget.Height = 8;
	width_gadget.Flags = GFLG_GADGHCOMP;
	width_gadget.Activation = GACT_RELVERIFY | GACT_LONGINT;
	width_gadget.GadgetType = GTYP_STRGADGET;
	width_gadget.GadgetRender = &field_border;
	width_gadget.SpecialInfo = &width_info;
	width_gadget.GadgetID = GID_WIDTH;

	height_gadget = width_gadget;
	height_gadget.TopEdge = oy + HEIGHT_Y;
	height_gadget.SpecialInfo = &height_info;
	height_gadget.GadgetID = GID_HEIGHT;

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
		/* Save on the left, Cancel on the right, as in the Workbench's
		   preferences editors */
		g->LeftEdge = ox + 8 + i * (INNER_W - 16 - BUTTON_W) / 2;
		g->TopEdge = oy + BUTTON_Y;
		g->Width = BUTTON_W;
		g->Height = BUTTON_H;
		g->Flags = GFLG_GADGHCOMP;
		g->Activation = GACT_RELVERIFY;
		g->GadgetType = GTYP_BOOLGADGET;
		g->GadgetRender = &button_border;
		g->GadgetText = t;
		g->GadgetID = GID_SAVE + i;
	}
}

static void print_at(int x, int y, const char *s)
{
	Move(rp, ox + x, oy + y + font->tf_Baseline);
	Text(rp, (STRPTR)s, strlen(s));
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
	nw.Title = (UBYTE *)"A314 Pictures";
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
	AddGadget(win, &width_gadget, -1);
	AddGadget(win, &height_gadget, -1);
	for (i = 0; i < 3; i++)
		AddGadget(win, &buttons[i], -1);
	RefreshGadgets(win->FirstGadget, win, NULL);

	SetAPen(rp, 1);
	print_at(8, 6, "Pictures larger than this are made");
	print_at(8, 16, "smaller by the Pi before they are shown:");
	print_at(48, WIDTH_Y, "Largest width");
	print_at(48, HEIGHT_Y, "Largest height");
	print_at(FIELD_X + FIELD_W + 8, WIDTH_Y, "pixels");
	print_at(FIELD_X + FIELD_W + 8, HEIGHT_Y, "pixels");
	print_at(8, 64, "(Pictures from WebP, AVIF, HEIC, JPEG XL, TIFF)");
	return TRUE;
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
			quit = TRUE;
		else if (cls == IDCMP_GADGETUP)
		{
			switch (g->GadgetID)
			{
			case GID_WIDTH:
				ActivateGadget(&height_gadget, win, NULL);
				break;
			case GID_SAVE:
			case GID_USE:
				if (store_setting(g->GadgetID == GID_SAVE))
					quit = TRUE;
				else
					report("Unable to store the setting");
				break;
			case GID_CANCEL:
				quit = TRUE;
				break;
			}
		}
	}
}

int main(void)
{
	LONG w, h;
	int rc = 0;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
	GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 0);
	if (!IntuitionBase || !GfxBase)
	{
		rc = 20;
		goto cleanup;
	}
	load_setting(&w, &h);
	sprintf(width_buf, "%ld", w);
	sprintf(height_buf, "%ld", h);
	width_info.LongInt = w;
	height_info.LongInt = h;

	font = OpenFont(&topaz8);
	if (!font || !open_window())
	{
		report("Unable to open the window");
		rc = 20;
		goto cleanup;
	}
	while (!quit)
	{
		Wait(1L << win->UserPort->mp_SigBit);
		handle_window();
	}

cleanup:
	if (win)
		CloseWindow(win);
	win = NULL;
	if (font)
		CloseFont(font);
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	return rc;
}
