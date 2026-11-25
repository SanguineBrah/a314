/*
 * a314gui - see a314gui.h.
 *
 * Two ways of making the same window: with gadtools.library (Workbench
 * 2.04 and later), or from plain Intuition gadgets (Workbench 1.3). The
 * state of every item is kept here, so that the gadgets can be made again
 * at any time; gadtools.library before V39 can't change a cycle gadget's
 * choices, so then all gadgets are made again.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <devices/inputevent.h>
#include <graphics/text.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a314gui.h"

#define MAX_ITEMS	48
#define TEXT_LEN	160
#define MAX_MENUS	24
#define CHECK_W		26	/* the size of a gadtools checkbox */
#define CHECK_H		11

extern struct IntuitionBase *IntuitionBase;
extern struct GfxBase *GfxBase;
struct Library *GadToolsBase = NULL;
static int gadtools_users = 0;		/* windows open with it */

struct Item
{
	struct GuiItem it;
	struct Gadget *gad;		/* the gadget (gadtools), or the 1.3 one */
	int px, py, pw, ph;		/* the box, in the window */
	char shortcut;			/* lower case, or 0 */
	char text[TEXT_LEN];		/* TEXT, LABEL and STRING contents */
	char label[64];			/* the label without '_' (1.3) */
	LONG number;
	BOOL check, disabled;
	const char **choices;		/* CYCLE */
	int nchoices, active;
	const char **lines;		/* LIST */
	int nlines, top, selected;
	LONG level, max_level;		/* SLIDER */
	struct Node *nodes;		/* the lines, for gadtools */
	struct List list;

	/* Workbench 1.3: the gadgets and their borders */
	struct Gadget g, scroller;
	struct StringInfo si;
	struct PropInfo prop;
	struct Image knob;
	struct Border light, dark;
	SHORT light_xy[6], dark_xy[6];
	struct IntuiText itext;
	char undo[TEXT_LEN];
};

struct Gui
{
	struct Window *win;
	struct Screen *scr;		/* locked public screen (gadtools) */
	APTR vi;
	struct Gadget *glist;
	struct Menu *menu;
	struct TextAttr *ta;
	struct TextFont *font;
	BOOL gt;
	int cw, rh, step, mx, my;	/* grid */
	UBYTE text_pen, shine_pen, shadow_pen, fill_pen, fill_text_pen;
	const char *title;
	struct Item *drag_area;		/* the AREA the mouse button went down in */
	int n;
	struct Item items[MAX_ITEMS];
};

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };
static const char *no_choices[] = { "", NULL };

static struct Item *find(struct Gui *gui, UWORD id)
{
	int i;
	for (i = 0; i < gui->n; i++)
		if (gui->items[i].it.id == id)
			return &gui->items[i];
	return NULL;
}

/* The label without '_', and its shortcut */
static void parse_label(struct Item *t)
{
	const char *s = t->it.label;
	char *d = t->label;
	t->shortcut = 0;
	if (!s)
	{
		*d = 0;
		return;
	}
	while (*s && d < t->label + sizeof(t->label) - 1)
	{
		if (*s == '_' && s[1])
		{
			char c = s[1];
			t->shortcut = (c >= 'A' && c <= 'Z') ? c + 32 : c;
			s++;
			continue;
		}
		*d++ = *s++;
	}
	*d = 0;
}

static int text_width(struct Gui *gui, const char *s)
{
	struct RastPort rp;
	InitRastPort(&rp);
	SetFont(&rp, gui->font);
	return TextLength(&rp, (STRPTR)s, strlen(s));
}

/* ---------------------------------------------------------------------- */
/* gadtools.library                                                       */

static void free_nodes(struct Item *t)
{
	if (t->nodes)
		free(t->nodes);
	t->nodes = NULL;
	NewList(&t->list);
}

static void make_nodes(struct Item *t)
{
	int i;
	free_nodes(t);
	if (t->nlines > 0)
	{
		t->nodes = (struct Node *)calloc(t->nlines, sizeof(struct Node));
		if (!t->nodes)
			return;
		for (i = 0; i < t->nlines; i++)
		{
			t->nodes[i].ln_Name = (char *)t->lines[i];
			AddTail(&t->list, &t->nodes[i]);
		}
	}
}

/* Keeps what the user typed, before the gadgets are made again */
static void gt_save_strings(struct Gui *gui)
{
	int i;
	for (i = 0; i < gui->n; i++)
	{
		struct Item *t = &gui->items[i];
		if (!t->gad)
			continue;
		if (t->it.kind == GUI_STRING)
		{
			strncpy(t->text, (char *)((struct StringInfo *)t->gad->SpecialInfo)->Buffer, TEXT_LEN - 1);
			t->text[TEXT_LEN - 1] = 0;
		}
		else if (t->it.kind == GUI_INTEGER)
			t->number = ((struct StringInfo *)t->gad->SpecialInfo)->LongInt;
	}
}

static BOOL gt_make_gadgets(struct Gui *gui)
{
	struct Gadget *g;
	struct NewGadget ng;
	int i;

	gui->glist = NULL;
	g = CreateContext(&gui->glist);
	for (i = 0; i < gui->n && g; i++)
	{
		struct Item *t = &gui->items[i];
		memset(&ng, 0, sizeof(ng));
		ng.ng_LeftEdge = t->px;
		ng.ng_TopEdge = t->py;
		ng.ng_Width = t->pw;
		ng.ng_Height = t->ph;
		ng.ng_GadgetText = (UBYTE *)t->it.label;
		ng.ng_TextAttr = gui->ta;
		ng.ng_GadgetID = t->it.id;
		ng.ng_VisualInfo = gui->vi;
		ng.ng_Flags = PLACETEXT_LEFT;

		if (t->it.kind == GUI_AREA)
		{
			t->gad = NULL;	/* drawn by the tool, in a bevelled box */
			continue;
		}
		switch (t->it.kind)
		{
		case GUI_BUTTON:
			ng.ng_Flags = PLACETEXT_IN;
			g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', GA_Disabled, t->disabled, TAG_DONE);
			break;
		case GUI_TEXT:
			g = CreateGadget(TEXT_KIND, g, &ng, GT_Underscore, '_', GTTX_Text, (ULONG)t->text,
				GTTX_CopyText, TRUE, GTTX_Border, TRUE, TAG_DONE);
			break;
		case GUI_LABEL:
			ng.ng_GadgetText = NULL;
			g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)t->text, GTTX_CopyText, TRUE, TAG_DONE);
			break;
		case GUI_STRING:
			g = CreateGadget(STRING_KIND, g, &ng, GT_Underscore, '_', GTST_String, (ULONG)t->text,
				GTST_MaxChars, t->it.max > 0 && t->it.max < TEXT_LEN ? t->it.max : TEXT_LEN - 1,
				GA_Disabled, t->disabled, TAG_DONE);
			break;
		case GUI_INTEGER:
			g = CreateGadget(INTEGER_KIND, g, &ng, GT_Underscore, '_', GTIN_Number, t->number,
				GTIN_MaxChars, 10, GA_Disabled, t->disabled, TAG_DONE);
			break;
		case GUI_CHECKBOX:
			ng.ng_Flags = PLACETEXT_RIGHT;
			ng.ng_Width = CHECK_W;
			ng.ng_Height = CHECK_H;
			if (GadToolsBase->lib_Version >= 39)
				ng.ng_Height = t->ph - 2;
			else
				ng.ng_TopEdge = t->py + (t->ph - CHECK_H) / 2;
			g = CreateGadget(CHECKBOX_KIND, g, &ng, GT_Underscore, '_', GTCB_Checked, t->check,
				GTCB_Scaled, TRUE, GA_Disabled, t->disabled, TAG_DONE);
			break;
		case GUI_CYCLE:
			g = CreateGadget(CYCLE_KIND, g, &ng, GT_Underscore, '_',
				GTCY_Labels, (ULONG)(t->nchoices ? t->choices : no_choices),
				GTCY_Active, t->active, GA_Disabled, t->disabled, TAG_DONE);
			break;
		case GUI_SLIDER:
			g = CreateGadget(SLIDER_KIND, g, &ng, GT_Underscore, '_', GTSL_Min, 0,
				GTSL_Max, t->max_level, GTSL_Level, t->level, GA_RelVerify, TRUE,
				GA_Immediate, TRUE, GA_Disabled, t->disabled, TAG_DONE);
			break;
		case GUI_LIST:
			ng.ng_Flags = PLACETEXT_ABOVE;
			make_nodes(t);
			g = CreateGadget(LISTVIEW_KIND, g, &ng, GT_Underscore, '_', GTLV_Labels, (ULONG)&t->list,
				GTLV_ShowSelected, 0, GTLV_Selected, t->selected < 0 ? ~0 : t->selected,
				GTLV_Top, t->top, GTLV_MakeVisible, t->selected < 0 ? 0 : t->selected,
				LAYOUTA_Spacing, 1, TAG_DONE);
			break;
		}
		t->gad = g;
	}
	return g != NULL;
}

/* The boxes of the AREA items */
static void gt_draw_areas(struct Gui *gui)
{
	int i;
	for (i = 0; i < gui->n; i++)
	{
		struct Item *t = &gui->items[i];
		if (t->it.kind == GUI_AREA)
			DrawBevelBox(gui->win->RPort, t->px, t->py, t->pw, t->ph,
				GTBB_Recessed, TRUE, GT_VisualInfo, (ULONG)gui->vi, TAG_DONE);
	}
}

static void gt_remake(struct Gui *gui)
{
	gt_save_strings(gui);
	RemoveGList(gui->win, gui->glist, -1);
	FreeGadgets(gui->glist);
	if (gt_make_gadgets(gui))
	{
		AddGList(gui->win, gui->glist, -1, -1, NULL);
		RefreshGList(gui->glist, gui->win, NULL, -1);
		GT_RefreshWindow(gui->win, NULL);
	}
}

/* ---------------------------------------------------------------------- */
/* Workbench 1.3: plain Intuition gadgets                                 */

/* A raised (or, with recessed, sunken) box w x h as a pair of borders */
static void bevel(struct Gui *gui, struct Item *t, int x, int y, int w, int h, BOOL recessed)
{
	SHORT *l = t->light_xy, *d = t->dark_xy;
	l[0] = 0;	l[1] = h - 1;	l[2] = 0;	l[3] = 0;	l[4] = w - 1;	l[5] = 0;
	d[0] = 1;	d[1] = h - 1;	d[2] = w - 1;	d[3] = h - 1;	d[4] = w - 1;	d[5] = 1;
	memset(&t->light, 0, sizeof(t->light));
	memset(&t->dark, 0, sizeof(t->dark));
	t->light.LeftEdge = t->dark.LeftEdge = x;
	t->light.TopEdge = t->dark.TopEdge = y;
	t->light.FrontPen = recessed ? gui->shadow_pen : gui->shine_pen;
	t->dark.FrontPen = recessed ? gui->shine_pen : gui->shadow_pen;
	t->light.DrawMode = t->dark.DrawMode = JAM1;
	t->light.Count = t->dark.Count = 3;
	t->light.XY = l;
	t->dark.XY = d;
	t->light.NextBorder = &t->dark;
}

static void draw_box(struct Gui *gui, int x, int y, int w, int h, BOOL recessed)
{
	struct RastPort *rp = gui->win->RPort;
	SetAPen(rp, recessed ? gui->shadow_pen : gui->shine_pen);
	Move(rp, x, y + h - 1);
	Draw(rp, x, y);
	Draw(rp, x + w - 1, y);
	SetAPen(rp, recessed ? gui->shine_pen : gui->shadow_pen);
	Move(rp, x + 1, y + h - 1);
	Draw(rp, x + w - 1, y + h - 1);
	Draw(rp, x + w - 1, y + 1);
}

static void fill(struct Gui *gui, int pen, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	SetAPen(gui->win->RPort, pen);
	RectFill(gui->win->RPort, x, y, x + w - 1, y + h - 1);
}

/* Text, cut to fit max pixels */
static void put_text(struct Gui *gui, int pen, int x, int y, const char *s, int max)
{
	struct RastPort *rp = gui->win->RPort;
	int n = strlen(s);
	while (n > 0 && TextLength(rp, (STRPTR)s, n) > max)
		n--;
	SetAPen(rp, pen);
	SetDrMd(rp, JAM1);
	Move(rp, x, y + rp->TxBaseline);
	Text(rp, (STRPTR)s, n);
}

static void old_draw_label(struct Gui *gui, struct Item *t)
{
	int w;
	if (!t->label[0])
		return;
	if (t->it.kind == GUI_CHECKBOX)
		put_text(gui, gui->text_pen, t->px + 26 + 8, t->py + (t->ph - 8) / 2, t->label, 400);
	else if (t->it.kind == GUI_LIST)
		put_text(gui, gui->text_pen, t->px, t->py - gui->rh + 3, t->label, t->pw);
	else if (t->it.kind != GUI_BUTTON)
	{
		w = text_width(gui, t->label);
		put_text(gui, gui->text_pen, t->px - 8 - w, t->py + (t->ph - 8) / 2, t->label, w);
	}
}

static int list_rows(struct Gui *gui, struct Item *t)
{
	return (t->ph - 4) / 9;
}

static void old_draw_item(struct Gui *gui, struct Item *t)
{
	int i, rows;

	switch (t->it.kind)
	{
	case GUI_TEXT:
		fill(gui, 0, t->px + 1, t->py + 1, t->pw - 2, t->ph - 2);
		draw_box(gui, t->px, t->py, t->pw, t->ph, TRUE);
		put_text(gui, gui->text_pen, t->px + 6, t->py + (t->ph - 8) / 2, t->text, t->pw - 12);
		break;
	case GUI_LABEL:
		fill(gui, 0, t->px, t->py, t->pw, t->ph);
		put_text(gui, gui->text_pen, t->px, t->py + (t->ph - 8) / 2, t->text, t->pw);
		break;
	case GUI_CHECKBOX:
		fill(gui, 0, t->px + 2, t->py + 2, 22, t->ph - 4);
		if (t->check)
		{
			/* A tick */
			struct RastPort *rp = gui->win->RPort;
			int y = t->py + t->ph / 2;
			SetAPen(rp, gui->text_pen);
			Move(rp, t->px + 7, y);
			Draw(rp, t->px + 11, y + 2);
			Draw(rp, t->px + 19, y - 3);
			Move(rp, t->px + 8, y);
			Draw(rp, t->px + 12, y + 2);
			Draw(rp, t->px + 20, y - 3);
		}
		break;
	case GUI_CYCLE:
		fill(gui, 0, t->px + 2, t->py + 2, t->pw - 4, t->ph - 4);
		/* The cycle mark: a bar and an arrow, then the choice */
		{
			struct RastPort *rp = gui->win->RPort;
			int y = t->py + 3, x = t->px + 6;
			SetAPen(rp, gui->text_pen);
			Move(rp, x + 6, y);
			Draw(rp, x + 2, y);
			Draw(rp, x + 2, y + t->ph - 7);
			Draw(rp, x + 6, y + t->ph - 7);
			Move(rp, x + 4, y + 2);
			Draw(rp, x + 8, y + 2);
			SetAPen(rp, gui->shadow_pen);
			Move(rp, t->px + 18, t->py + 2);
			Draw(rp, t->px + 18, t->py + t->ph - 3);
			SetAPen(rp, gui->shine_pen);
			Move(rp, t->px + 19, t->py + 2);
			Draw(rp, t->px + 19, t->py + t->ph - 3);
		}
		if (t->nchoices > 0)
			put_text(gui, gui->text_pen, t->px + 26, t->py + (t->ph - 8) / 2,
				t->choices[t->active], t->pw - 32);
		break;
	case GUI_AREA:
		draw_box(gui, t->px, t->py, t->pw, t->ph, TRUE);
		break;
	case GUI_LIST:
		rows = list_rows(gui, t);
		fill(gui, 0, t->px + 2, t->py + 2, t->pw - 20, t->ph - 4);
		for (i = 0; i < rows && t->top + i < t->nlines; i++)
		{
			int y = t->py + 2 + i * 9;
			BOOL sel = t->top + i == t->selected;
			if (sel)
				fill(gui, gui->fill_pen, t->px + 2, y, t->pw - 20, 9);
			put_text(gui, sel ? gui->fill_text_pen : gui->text_pen, t->px + 4, y + 1,
				t->lines[t->top + i], t->pw - 24);
		}
		break;
	}
}

static void old_update_scroller(struct Gui *gui, struct Item *t)
{
	int rows = list_rows(gui, t), total = t->nlines > rows ? t->nlines : rows;
	UWORD body = (UWORD)((ULONG)MAXBODY * rows / total);
	UWORD pot = t->nlines > rows ? (UWORD)((ULONG)MAXPOT * t->top / (t->nlines - rows)) : 0;
	/* ModifyProp(), not NewModifyProp(), which 1.3 doesn't have */
	ModifyProp(&t->scroller, gui->win, NULL, t->prop.Flags, 0, pot, MAXBODY, body);
}

static void old_make_gadgets(struct Gui *gui)
{
	int i;
	for (i = 0; i < gui->n; i++)
	{
		struct Item *t = &gui->items[i];
		struct Gadget *g = &t->g;
		memset(g, 0, sizeof(*g));
		g->LeftEdge = t->px;
		g->TopEdge = t->py;
		g->Width = t->pw;
		g->Height = t->ph;
		g->GadgetID = t->it.id;
		g->GadgetType = GTYP_BOOLGADGET;
		g->Flags = GFLG_GADGHCOMP;
		g->Activation = GACT_RELVERIFY;
		t->gad = NULL;

		switch (t->it.kind)
		{
		case GUI_BUTTON:
			bevel(gui, t, 0, 0, t->pw, t->ph, FALSE);
			g->GadgetRender = &t->light;
			memset(&t->itext, 0, sizeof(t->itext));
			t->itext.FrontPen = gui->text_pen;
			t->itext.DrawMode = JAM1;
			t->itext.ITextFont = &topaz8;
			t->itext.IText = (UBYTE *)t->label;
			t->itext.LeftEdge = (t->pw - 8 * (int)strlen(t->label)) / 2;
			t->itext.TopEdge = (t->ph - 8) / 2;
			g->GadgetText = &t->itext;
			if (t->disabled)
				g->Flags |= GFLG_DISABLED;
			t->gad = g;
			break;
		case GUI_CYCLE:
			bevel(gui, t, 0, 0, t->pw, t->ph, FALSE);
			g->GadgetRender = &t->light;
			t->gad = g;
			break;
		case GUI_CHECKBOX:
			g->Width = 26;
			bevel(gui, t, 0, 0, 26, t->ph, FALSE);
			g->GadgetRender = &t->light;
			g->Flags = GFLG_GADGHNONE;
			g->Activation = GACT_RELVERIFY | GACT_TOGGLESELECT;
			if (t->check)
				g->Flags |= GFLG_SELECTED;
			t->gad = g;
			break;
		case GUI_STRING:
		case GUI_INTEGER:
			/* The text area inside a sunken box */
			g->LeftEdge = t->px + 6;
			g->TopEdge = t->py + (t->ph - 8) / 2;
			g->Width = t->pw - 12;
			g->Height = 8;
			bevel(gui, t, -6, -(t->ph - 8) / 2, t->pw, t->ph, TRUE);
			g->GadgetRender = &t->light;
			g->GadgetType = GTYP_STRGADGET;
			g->Flags = GFLG_GADGHCOMP;
			memset(&t->si, 0, sizeof(t->si));
			if (t->it.kind == GUI_INTEGER)
			{
				g->Activation |= GACT_LONGINT;
				sprintf(t->text, "%ld", t->number);
				t->si.LongInt = t->number;
			}
			t->si.Buffer = (UBYTE *)t->text;
			t->si.UndoBuffer = (UBYTE *)t->undo;
			t->si.MaxChars = t->it.kind == GUI_INTEGER ? 11 :
				(t->it.max > 0 && t->it.max < TEXT_LEN ? t->it.max + 1 : TEXT_LEN);
			g->SpecialInfo = &t->si;
			t->gad = g;
			break;
		case GUI_SLIDER:
			/* A proportional gadget; 1.3 draws its box */
			memset(&t->prop, 0, sizeof(t->prop));
			g->GadgetType = GTYP_PROPGADGET;
			g->Activation = GACT_RELVERIFY | GACT_IMMEDIATE | GACT_FOLLOWMOUSE;
			g->GadgetRender = &t->knob;
			g->SpecialInfo = &t->prop;
			t->prop.Flags = AUTOKNOB | FREEHORIZ;
			t->prop.HorizBody = t->max_level > 0 ? (UWORD)(MAXBODY / (t->max_level + 1)) : MAXBODY;
			t->prop.HorizPot = t->max_level > 0 ? (UWORD)((ULONG)MAXPOT * t->level / t->max_level) : 0;
			t->prop.VertBody = MAXBODY;
			t->gad = g;
			break;
		case GUI_LIST:
			/* The lines (clicked on: a GADGETDOWN), and a scroller */
			g->Width = t->pw - 18;
			g->Flags = GFLG_GADGHNONE;
			g->Activation = GACT_IMMEDIATE;
			bevel(gui, t, 0, 0, t->pw - 18, t->ph, TRUE);
			g->GadgetRender = &t->light;
			t->gad = g;
			memset(&t->scroller, 0, sizeof(t->scroller));
			memset(&t->prop, 0, sizeof(t->prop));
			t->scroller.LeftEdge = t->px + t->pw - 16;
			t->scroller.TopEdge = t->py;
			t->scroller.Width = 16;
			t->scroller.Height = t->ph;
			t->scroller.Flags = GFLG_GADGHCOMP;
			t->scroller.Activation = GACT_RELVERIFY | GACT_IMMEDIATE | GACT_FOLLOWMOUSE;
			t->scroller.GadgetType = GTYP_PROPGADGET;
			t->scroller.GadgetRender = &t->knob;
			t->scroller.SpecialInfo = &t->prop;
			t->scroller.GadgetID = t->it.id;
			t->prop.Flags = AUTOKNOB | FREEVERT;
			t->prop.VertBody = MAXBODY;
			t->prop.HorizBody = MAXBODY;
			break;
		}
	}
	for (i = 0; i < gui->n; i++)
	{
		struct Item *t = &gui->items[i];
		if (t->gad)
			AddGadget(gui->win, t->gad, -1);
		if (t->it.kind == GUI_LIST)
			AddGadget(gui->win, &t->scroller, -1);
	}
	RefreshGadgets(gui->win->FirstGadget, gui->win, NULL);
	for (i = 0; i < gui->n; i++)
	{
		old_draw_label(gui, &gui->items[i]);
		old_draw_item(gui, &gui->items[i]);
		if (gui->items[i].it.kind == GUI_LIST)
			old_update_scroller(gui, &gui->items[i]);
	}
}

/* ---------------------------------------------------------------------- */

struct Gui *gui_open(const char *title, const struct GuiItem *items, int nitems,
	const struct GuiMenu *menus)
{
	struct Gui *gui;
	int i, maxx = 0, maxy = 0, inner_w, inner_h, bl, br, bt, bb, sw, sh;

	if (nitems > MAX_ITEMS)
		return NULL;
	gui = (struct Gui *)calloc(1, sizeof(struct Gui));
	if (!gui)
		return NULL;
	gui->title = title;
	gui->n = nitems;
	for (i = 0; i < nitems; i++)
	{
		struct Item *t = &gui->items[i];
		t->it = items[i];
		t->selected = -1;
		NewList(&t->list);
		parse_label(t);
	}

	if (!GadToolsBase && IntuitionBase->LibNode.lib_Version >= 37)
		GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37);
	gui->gt = GadToolsBase != NULL;
	if (gui->gt)
		gadtools_users++;

	if (gui->gt)
	{
		struct DrawInfo *dri;
		gui->scr = LockPubScreen(NULL);
		if (!gui->scr)
			goto fail;
		gui->vi = GetVisualInfo(gui->scr, TAG_DONE);
		gui->ta = gui->scr->Font;
		dri = GetScreenDrawInfo(gui->scr);
		if (dri)
		{
			gui->text_pen = dri->dri_Pens[TEXTPEN];
			gui->shine_pen = dri->dri_Pens[SHINEPEN];
			gui->shadow_pen = dri->dri_Pens[SHADOWPEN];
			gui->fill_pen = dri->dri_Pens[FILLPEN];
			gui->fill_text_pen = dri->dri_Pens[FILLTEXTPEN];
			FreeScreenDrawInfo(gui->scr, dri);
		}
		bl = gui->scr->WBorLeft;
		br = gui->scr->WBorRight;
		bb = gui->scr->WBorBottom;
		bt = gui->scr->WBorTop + gui->scr->Font->ta_YSize + 1;
		sw = gui->scr->Width;
		sh = gui->scr->Height;
	}
	else
	{
		struct Screen wb;
		if (!GetScreenData((APTR)&wb, sizeof(wb), WBENCHSCREEN, NULL))
			goto fail;
		gui->ta = &topaz8;
		gui->text_pen = 1;
		gui->shine_pen = 1;
		gui->shadow_pen = 2;
		gui->fill_pen = 3;
		gui->fill_text_pen = 2;
		bl = wb.WBorLeft;
		br = wb.WBorRight;
		bb = wb.WBorBottom;
		bt = wb.WBorTop + wb.Font->ta_YSize + 1;
		sw = wb.Width;
		sh = wb.Height;
	}
	gui->font = OpenFont(gui->ta);
	if (!gui->font)
		goto fail;

	/* The grid, from the font: rows with gaps between them, closer
	   together if the window wouldn't fit on the screen (as on a 1.3
	   Workbench, 200 or 256 lines high) */
	gui->cw = (text_width(gui, "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ") + 52) / 53;
	gui->mx = 10;
	gui->my = 6;
	{
		int rows = 0, gap, avail = sh - bt - bb - 2 * gui->my;
		for (i = 0; i < nitems; i++)
		{
			int end = items[i].y + ((items[i].kind == GUI_LIST || items[i].kind == GUI_AREA) &&
				items[i].h > 0 ? items[i].h : 1);
			if (end > rows)
				rows = end;
		}
		gui->rh = gui->font->tf_YSize + 6;
		for (gap = 4; gap > 0 && rows * (gui->rh + gap) - gap > avail; gap--)
			;
		if (rows * (gui->rh + gap) - gap > avail)
		{
			gui->rh = gui->font->tf_YSize + 4;
			gap = 1;
		}
		gui->step = gui->rh + gap;
	}
	for (i = 0; i < nitems; i++)
	{
		struct Item *t = &gui->items[i];
		int right;
		t->px = gui->mx + t->it.x * gui->cw;
		t->py = gui->my + t->it.y * gui->step;
		t->pw = t->it.w * gui->cw;
		t->ph = (t->it.kind == GUI_LIST || t->it.kind == GUI_AREA) && t->it.h > 0 ?
			t->it.h * gui->step - (gui->step - gui->rh) : gui->rh;
		right = t->px + t->pw;
		if (t->it.kind == GUI_CHECKBOX)
			right = t->px + 26 + 8 + text_width(gui, t->label);
		if (right > maxx)
			maxx = right;
		if (t->py + t->ph > maxy)
			maxy = t->py + t->ph;
	}
	inner_w = maxx + gui->mx;
	inner_h = maxy + gui->my;

	if (gui->gt)
	{
		/* gadtools places gadgets from the window's outer corner */
		for (i = 0; i < nitems; i++)
		{
			gui->items[i].px += bl;
			gui->items[i].py += bt;
		}
		if (!gt_make_gadgets(gui))
			goto fail;
		gui->win = OpenWindowTags(NULL,
			WA_Left, (sw - inner_w - bl - br) / 2,
			WA_Top, (sh - inner_h - bt - bb) / 3,
			WA_InnerWidth, inner_w,
			WA_InnerHeight, inner_h,
			WA_Title, (ULONG)title,
			WA_PubScreen, (ULONG)gui->scr,
			WA_Gadgets, (ULONG)gui->glist,
			WA_IDCMP, BUTTONIDCMP | CHECKBOXIDCMP | CYCLEIDCMP | LISTVIEWIDCMP |
				STRINGIDCMP | INTEGERIDCMP | TEXTIDCMP | SLIDERIDCMP | IDCMP_CLOSEWINDOW |
				IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY | IDCMP_RAWKEY | IDCMP_MENUPICK |
				IDCMP_MOUSEBUTTONS,
			WA_DragBar, TRUE,
			WA_DepthGadget, TRUE,
			WA_CloseGadget, TRUE,
			WA_Activate, TRUE,
			WA_SmartRefresh, TRUE,
			WA_NewLookMenus, TRUE,
			TAG_DONE);
		if (!gui->win)
			goto fail;
		GT_RefreshWindow(gui->win, NULL);
		gt_draw_areas(gui);

		if (menus)
		{
			static struct NewMenu nm[MAX_MENUS + 1];
			int k;
			for (k = 0; k < MAX_MENUS && menus[k].type != GUI_MENU_END; k++)
			{
				nm[k].nm_Type = menus[k].type == GUI_MENU_TITLE ? NM_TITLE : NM_ITEM;
				nm[k].nm_Label = menus[k].type == GUI_MENU_BAR ? NM_BARLABEL : (STRPTR)menus[k].label;
				nm[k].nm_CommKey = menus[k].key ? (STRPTR)&menus[k].key : NULL;
				nm[k].nm_Flags = 0;
				nm[k].nm_MutualExclude = 0;
				nm[k].nm_UserData = (APTR)(ULONG)menus[k].id;
			}
			memset(&nm[k], 0, sizeof(nm[k]));
			nm[k].nm_Type = NM_END;
			gui->menu = CreateMenus(nm, TAG_DONE);
			if (gui->menu && LayoutMenus(gui->menu, gui->vi, GTMN_NewLookMenus, TRUE, TAG_DONE))
				SetMenuStrip(gui->win, gui->menu);
		}
	}
	else
	{
		struct NewWindow nw;
		memset(&nw, 0, sizeof(nw));
		nw.Width = inner_w + bl + br;
		nw.Height = inner_h + bt + bb;
		nw.LeftEdge = (sw - nw.Width) / 2;
		nw.TopEdge = (sh - nw.Height) / 3;
		if (nw.LeftEdge < 0 || nw.TopEdge < 0)
			nw.LeftEdge = nw.TopEdge = 0;
		nw.DetailPen = 0;
		nw.BlockPen = 1;
		nw.IDCMPFlags = IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_GADGETDOWN |
			IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS | IDCMP_VANILLAKEY;
		nw.Flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
			WFLG_SMART_REFRESH | WFLG_NOCAREREFRESH | WFLG_ACTIVATE | WFLG_RMBTRAP;
		nw.Title = (UBYTE *)title;
		nw.Type = WBENCHSCREEN;
		gui->win = OpenWindow(&nw);
		if (!gui->win)
			goto fail;
		SetFont(gui->win->RPort, gui->font);
		/* The items were placed inside the borders of this window */
		for (i = 0; i < nitems; i++)
		{
			gui->items[i].px += gui->win->BorderLeft;
			gui->items[i].py += gui->win->BorderTop;
		}
		old_make_gadgets(gui);
	}
	return gui;

fail:
	gui_close(gui);
	return NULL;
}

void gui_close(struct Gui *gui)
{
	int i;
	if (!gui)
		return;
	if (gui->win)
	{
		if (gui->menu)
			ClearMenuStrip(gui->win);
		CloseWindow(gui->win);
	}
	if (gui->gt)
	{
		if (gui->menu)
			FreeMenus(gui->menu);
		FreeGadgets(gui->glist);
		if (gui->vi)
			FreeVisualInfo(gui->vi);
		if (gui->scr)
			UnlockPubScreen(NULL, gui->scr);
	}
	for (i = 0; i < gui->n; i++)
		free_nodes(&gui->items[i]);
	if (gui->font)
		CloseFont(gui->font);
	if (gui->gt && --gadtools_users == 0)
	{
		CloseLibrary(GadToolsBase);
		GadToolsBase = NULL;
	}
	free(gui);
}

ULONG gui_signal(struct Gui *gui)
{
	return 1L << gui->win->UserPort->mp_SigBit;
}

struct Window *gui_window(struct Gui *gui)
{
	return gui ? gui->win : NULL;
}

/* ---------------------------------------------------------------------- */
/* Events                                                                 */

static void set_event(struct GuiEvent *ev, UWORD type, UWORD id, LONG value)
{
	ev->type = type;
	ev->id = id;
	ev->value = value;
}

/* A key: Esc closes; a shortcut works its item */
static BOOL handle_key(struct Gui *gui, UWORD code, UWORD qual, struct GuiEvent *ev)
{
	char c = (char)code;
	int i;

	if (c == 27)
	{
		set_event(ev, GUI_EV_CLOSE, 0, 0);
		return TRUE;
	}
	if (c >= 'A' && c <= 'Z')
		c += 32;
	for (i = 0; i < gui->n; i++)
	{
		struct Item *t = &gui->items[i];
		if (t->shortcut != c || t->disabled)
			continue;
		switch (t->it.kind)
		{
		case GUI_BUTTON:
			set_event(ev, GUI_EV_GADGET, t->it.id, 0);
			return TRUE;
		case GUI_CHECKBOX:
			gui_set_check(gui, t->it.id, !t->check);
			set_event(ev, GUI_EV_GADGET, t->it.id, t->check);
			return TRUE;
		case GUI_CYCLE:
			if (t->nchoices > 0)
			{
				int a = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) ?
					t->active + t->nchoices - 1 : t->active + 1;
				gui_set_cycle(gui, t->it.id, t->choices, t->nchoices, a % t->nchoices);
				set_event(ev, GUI_EV_GADGET, t->it.id, t->active);
				return TRUE;
			}
			break;
		case GUI_STRING:
		case GUI_INTEGER:
			if (t->gad && IntuitionBase->LibNode.lib_Version >= 36)
				ActivateGadget(t->gad, gui->win, NULL);
			return FALSE;
		}
	}
	set_event(ev, GUI_EV_KEY, 0, (UBYTE)code);
	return TRUE;
}

static int slider_level(struct Item *t)
{
	return t->max_level > 0 ? (int)(((ULONG)t->prop.HorizPot * t->max_level + MAXPOT / 2) / MAXPOT) : 0;
}

/* An event for the mouse at (mx, my) in an area */
static void area_event(struct Item *t, int mx, int my, struct GuiEvent *ev)
{
	int x = mx - t->px, y = my - t->py;
	x = x < 0 ? 0 : x >= t->pw ? t->pw - 1 : x;
	y = y < 0 ? 0 : y >= t->ph ? t->ph - 1 : y;
	set_event(ev, GUI_EV_GADGET, t->it.id, ((LONG)x << 16) | y);
}

static BOOL old_list_click(struct Gui *gui, struct Item *t, int mx, int my, struct GuiEvent *ev)
{
	int row = (my - t->py - 2) / 9;
	if (row < 0 || row >= list_rows(gui, t) || t->top + row >= t->nlines)
		return FALSE;
	t->selected = t->top + row;
	old_draw_item(gui, t);
	set_event(ev, GUI_EV_GADGET, t->it.id, t->selected);
	return TRUE;
}

static void old_list_scrolled(struct Gui *gui, struct Item *t)
{
	int rows = list_rows(gui, t);
	int top = t->nlines > rows ? (int)(((ULONG)t->prop.VertPot * (t->nlines - rows) + MAXPOT / 2) / MAXPOT) : 0;
	if (top != t->top)
	{
		t->top = top;
		old_draw_item(gui, t);
	}
}

BOOL gui_event(struct Gui *gui, struct GuiEvent *ev)
{
	struct IntuiMessage *im;

	for (;;)
	{
		ULONG cls;
		UWORD code, qual;
		APTR addr;
		int mx, my;
		BOOL got = FALSE;

		im = gui->gt ? GT_GetIMsg(gui->win->UserPort) : (struct IntuiMessage *)GetMsg(gui->win->UserPort);
		if (!im)
			return FALSE;
		cls = im->Class;
		code = im->Code;
		qual = im->Qualifier;
		addr = im->IAddress;
		mx = im->MouseX;
		my = im->MouseY;
		if (gui->gt)
			GT_ReplyIMsg(im);
		else
			ReplyMsg((struct Message *)im);

		switch (cls)
		{
		case IDCMP_CLOSEWINDOW:
			set_event(ev, GUI_EV_CLOSE, 0, 0);
			got = TRUE;
			break;
		case IDCMP_REFRESHWINDOW:
			GT_BeginRefresh(gui->win);
			gt_draw_areas(gui);
			GT_EndRefresh(gui->win, TRUE);
			break;
		case IDCMP_VANILLAKEY:
			got = handle_key(gui, code, qual, ev);
			break;
		case IDCMP_RAWKEY:
			/* Only the keys that are no characters come this way */
			if (!(code & IECODE_UP_PREFIX))
			{
				set_event(ev, GUI_EV_RAWKEY, 0, ((LONG)qual << 16) | code);
				got = TRUE;
			}
			break;
		case IDCMP_MOUSEBUTTONS:
			if (code == SELECTDOWN)
			{
				int i;
				for (i = 0; i < gui->n; i++)
				{
					struct Item *t = &gui->items[i];
					if (t->it.kind == GUI_AREA && mx >= t->px && mx < t->px + t->pw &&
						my >= t->py && my < t->py + t->ph)
					{
						gui->drag_area = t;
						ReportMouse(TRUE, gui->win);
						area_event(t, mx, my, ev);
						got = TRUE;
					}
				}
			}
			else if (code == SELECTUP && gui->drag_area)
			{
				gui->drag_area = NULL;
				ReportMouse(FALSE, gui->win);
			}
			break;
		case IDCMP_MENUPICK:
			if (code != MENUNULL && gui->menu)
			{
				struct MenuItem *item = ItemAddress(gui->menu, code);
				if (item)
				{
					set_event(ev, GUI_EV_MENU, (UWORD)(ULONG)GTMENUITEM_USERDATA(item), 0);
					got = TRUE;
				}
			}
			break;
		case IDCMP_GADGETDOWN:
		case IDCMP_GADGETUP:
		case IDCMP_MOUSEMOVE:
		{
			struct Gadget *g = (struct Gadget *)addr;
			struct Item *t;
			if (cls == IDCMP_MOUSEMOVE)
			{
				int i;
				if (gui->drag_area)
				{
					area_event(gui->drag_area, mx, my, ev);
					got = TRUE;
					break;
				}
				for (i = 0; i < gui->n && !got; i++)
				{
					t = &gui->items[i];
					if (t->it.kind == GUI_SLIDER && t->gad && (APTR)t->gad == addr && gui->gt)
					{
						/* gadtools: a slider being moved */
						t->level = code;
						set_event(ev, GUI_EV_GADGET, t->it.id, t->level);
						got = TRUE;
					}
					else if (!gui->gt && t->it.kind == GUI_SLIDER && (t->g.Flags & GFLG_SELECTED))
					{
						int l = slider_level(t);
						if (l != t->level)
						{
							t->level = l;
							set_event(ev, GUI_EV_GADGET, t->it.id, l);
							got = TRUE;
						}
					}
					else if (!gui->gt && t->it.kind == GUI_LIST && (t->scroller.Flags & GFLG_SELECTED))
						old_list_scrolled(gui, t);
				}
				break;
			}
			t = find(gui, g->GadgetID);
			if (!t)
				break;
			switch (t->it.kind)
			{
			case GUI_SLIDER:
				t->level = gui->gt ? code : slider_level(t);
				set_event(ev, GUI_EV_GADGET, t->it.id, t->level);
				got = TRUE;
				break;
			case GUI_BUTTON:
				if (cls == IDCMP_GADGETUP)
				{
					set_event(ev, GUI_EV_GADGET, t->it.id, 0);
					got = TRUE;
				}
				break;
			case GUI_CHECKBOX:
				t->check = (g->Flags & GFLG_SELECTED) != 0;
				if (!gui->gt)
					old_draw_item(gui, t);
				set_event(ev, GUI_EV_GADGET, t->it.id, t->check);
				got = TRUE;
				break;
			case GUI_CYCLE:
				if (gui->gt)
					t->active = code;
				else if (t->nchoices > 0)
				{
					t->active = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) ?
						(t->active + t->nchoices - 1) % t->nchoices : (t->active + 1) % t->nchoices;
					old_draw_item(gui, t);
				}
				set_event(ev, GUI_EV_GADGET, t->it.id, t->active);
				got = TRUE;
				break;
			case GUI_STRING:
			case GUI_INTEGER:
				if (t->it.kind == GUI_INTEGER)
					t->number = ((struct StringInfo *)g->SpecialInfo)->LongInt;
				set_event(ev, GUI_EV_GADGET, t->it.id, 0);
				got = TRUE;
				break;
			case GUI_LIST:
				if (gui->gt)
				{
					t->selected = code;
					set_event(ev, GUI_EV_GADGET, t->it.id, code);
					got = TRUE;
				}
				else if (g == &t->scroller)
					old_list_scrolled(gui, t);
				else if (cls == IDCMP_GADGETDOWN)
					got = old_list_click(gui, t, mx, my, ev);
				break;
			}
			break;
		}
		}
		if (got)
			return TRUE;
	}
}

/* ---------------------------------------------------------------------- */
/* Changing and reading items                                             */

void gui_set_text(struct Gui *gui, UWORD id, const char *text)
{
	struct Item *t = find(gui, id);
	if (!t)
		return;
	strncpy(t->text, text ? text : "", TEXT_LEN - 1);
	t->text[TEXT_LEN - 1] = 0;
	if (gui->gt)
	{
		if (t->it.kind == GUI_STRING)
			GT_SetGadgetAttrs(t->gad, gui->win, NULL, GTST_String, (ULONG)t->text, TAG_DONE);
		else
			GT_SetGadgetAttrs(t->gad, gui->win, NULL, GTTX_Text, (ULONG)t->text, TAG_DONE);
	}
	else if (t->it.kind == GUI_STRING)
		RefreshGList(t->gad, gui->win, NULL, 1);
	else
		old_draw_item(gui, t);
}

void gui_set_number(struct Gui *gui, UWORD id, LONG n)
{
	struct Item *t = find(gui, id);
	if (!t)
		return;
	t->number = n;
	if (gui->gt)
		GT_SetGadgetAttrs(t->gad, gui->win, NULL, GTIN_Number, n, TAG_DONE);
	else
	{
		sprintf(t->text, "%ld", n);
		t->si.LongInt = n;
		RefreshGList(t->gad, gui->win, NULL, 1);
	}
}

void gui_set_check(struct Gui *gui, UWORD id, BOOL on)
{
	struct Item *t = find(gui, id);
	if (!t)
		return;
	t->check = on;
	if (gui->gt)
		GT_SetGadgetAttrs(t->gad, gui->win, NULL, GTCB_Checked, on, TAG_DONE);
	else
	{
		if (on)
			t->g.Flags |= GFLG_SELECTED;
		else
			t->g.Flags &= ~GFLG_SELECTED;
		old_draw_item(gui, t);
	}
}

void gui_set_cycle(struct Gui *gui, UWORD id, const char **labels, int n, int active)
{
	struct Item *t = find(gui, id);
	BOOL same;
	if (!t)
		return;
	same = labels == t->choices && n == t->nchoices;
	t->choices = labels;
	t->nchoices = n;
	t->active = active < 0 || active >= n ? 0 : active;
	if (gui->gt)
	{
		if (same)
			GT_SetGadgetAttrs(t->gad, gui->win, NULL, GTCY_Active, t->active, TAG_DONE);
		else if (GadToolsBase->lib_Version >= 39)
			GT_SetGadgetAttrs(t->gad, gui->win, NULL,
				GTCY_Labels, (ULONG)(n ? labels : no_choices), GTCY_Active, t->active, TAG_DONE);
		else
			gt_remake(gui);		/* V37 can't change the choices */
	}
	else
		old_draw_item(gui, t);
}

void gui_set_list(struct Gui *gui, UWORD id, const char **lines, int n, int selected)
{
	struct Item *t = find(gui, id);
	if (!t)
		return;
	if (gui->gt)
		GT_SetGadgetAttrs(t->gad, gui->win, NULL, GTLV_Labels, ~0, TAG_DONE);
	t->lines = lines;
	t->nlines = n;
	t->selected = selected < n ? selected : -1;
	if (t->top > n)
		t->top = 0;
	if (gui->gt)
	{
		make_nodes(t);
		GT_SetGadgetAttrs(t->gad, gui->win, NULL, GTLV_Labels, (ULONG)&t->list,
			GTLV_Selected, t->selected < 0 ? ~0 : t->selected,
			GTLV_MakeVisible, t->selected < 0 ? 0 : t->selected, TAG_DONE);
	}
	else
	{
		int rows = list_rows(gui, t);
		if (t->selected >= 0 && (t->selected < t->top || t->selected >= t->top + rows))
			t->top = t->selected - rows + 1 > 0 ? t->selected - rows + 1 : 0;
		if (t->top > (n > rows ? n - rows : 0))
			t->top = n > rows ? n - rows : 0;
		old_draw_item(gui, t);
		old_update_scroller(gui, t);
	}
}

void gui_disable(struct Gui *gui, UWORD id, BOOL off)
{
	struct Item *t = find(gui, id);
	if (!t || t->disabled == off)
		return;
	t->disabled = off;
	if (gui->gt)
		GT_SetGadgetAttrs(t->gad, gui->win, NULL, GA_Disabled, off, TAG_DONE);
	else if (t->gad)
	{
		if (off)
			OffGadget(t->gad, gui->win, NULL);
		else
			OnGadget(t->gad, gui->win, NULL);
	}
}

const char *gui_get_text(struct Gui *gui, UWORD id)
{
	struct Item *t = find(gui, id);
	if (!t)
		return "";
	if (t->it.kind == GUI_STRING && t->gad)
		return (const char *)((struct StringInfo *)t->gad->SpecialInfo)->Buffer;
	return t->text;
}

LONG gui_get_number(struct Gui *gui, UWORD id)
{
	struct Item *t = find(gui, id);
	if (!t)
		return 0;
	if (t->gad && t->it.kind == GUI_INTEGER)
		return ((struct StringInfo *)t->gad->SpecialInfo)->LongInt;
	return t->number;
}

BOOL gui_get_check(struct Gui *gui, UWORD id)
{
	struct Item *t = find(gui, id);
	return t ? t->check : FALSE;
}

int gui_get_cycle(struct Gui *gui, UWORD id)
{
	struct Item *t = find(gui, id);
	return t ? t->active : 0;
}

void gui_message(struct Gui *gui, const char *text)
{
	struct Window *w = gui ? gui->win : NULL;

	if (IntuitionBase->LibNode.lib_Version >= 36)
	{
		struct EasyStruct es;
		es.es_StructSize = sizeof(es);
		es.es_Flags = 0;
		es.es_Title = (UBYTE *)(gui && gui->title ? gui->title : "A314");
		es.es_TextFormat = (UBYTE *)"%s";
		es.es_GadgetFormat = (UBYTE *)"OK";
		EasyRequest(w, &es, NULL, (ULONG)text);
	}
	else
	{
		struct IntuiText body = { 0, 1, JAM2, 16, 8, NULL, (UBYTE *)text, NULL };
		struct IntuiText ok = { 0, 1, JAM2, 6, 3, NULL, (UBYTE *)"OK", NULL };
		AutoRequest(w, &body, NULL, &ok, 0, 0, 8 * strlen(text) + 48, 54);
	}
}

BOOL gui_ask(struct Gui *gui, const char *text, const char *buttons)
{
	struct Window *w = gui ? gui->win : NULL;

	if (IntuitionBase->LibNode.lib_Version >= 36)
	{
		struct EasyStruct es;
		es.es_StructSize = sizeof(es);
		es.es_Flags = 0;
		es.es_Title = (UBYTE *)(gui && gui->title ? gui->title : "A314");
		es.es_TextFormat = (UBYTE *)"%s";
		es.es_GadgetFormat = (UBYTE *)buttons;
		return EasyRequest(w, &es, NULL, (ULONG)text) == 1;
	}
	else
	{
		/* AutoRequest() takes two buttons: the text before and after '|' */
		static char yes[32], no[32];
		const char *bar = strchr(buttons, '|');
		struct IntuiText body = { 0, 1, JAM2, 16, 8, NULL, (UBYTE *)text, NULL };
		struct IntuiText t_yes = { 0, 1, JAM2, 6, 3, NULL, (UBYTE *)yes, NULL };
		struct IntuiText t_no = { 0, 1, JAM2, 6, 3, NULL, (UBYTE *)no, NULL };
		int n = bar ? bar - buttons : strlen(buttons);
		int width = 8 * strlen(text) + 48;
		strncpy(yes, buttons, n < 31 ? n : 31);
		yes[n < 31 ? n : 31] = 0;
		strncpy(no, bar ? bar + 1 : "Cancel", 31);
		no[31] = 0;
		return AutoRequest(w, &body, &t_yes, &t_no, 0, 0, width < 240 ? 240 : width, 54);
	}
}

void gui_activate(struct Gui *gui, UWORD id)
{
	struct Item *t = find(gui, id);
	if (t && t->gad && IntuitionBase->LibNode.lib_Version >= 36)
		ActivateGadget(t->gad, gui->win, NULL);
}

void gui_set_slider(struct Gui *gui, UWORD id, LONG level, LONG max)
{
	struct Item *t = find(gui, id);
	if (!t)
		return;
	level = level < 0 ? 0 : level > max ? max : level;
	if (level == t->level && max == t->max_level && t->gad)
		return;
	t->level = level;
	t->max_level = max;
	if (!t->gad)
		return;
	if (gui->gt)
		GT_SetGadgetAttrs(t->gad, gui->win, NULL, GTSL_Max, max, GTSL_Level, level, TAG_DONE);
	else
	{
		UWORD body = max > 0 ? (UWORD)(MAXBODY / (max + 1)) : MAXBODY;
		UWORD pot = max > 0 ? (UWORD)((ULONG)MAXPOT * level / max) : 0;
		ModifyProp(t->gad, gui->win, NULL, t->prop.Flags, pot, 0, body, MAXBODY);
	}
}

void gui_area(struct Gui *gui, UWORD id, int *x, int *y, int *w, int *h)
{
	struct Item *t = find(gui, id);
	if (!t)
	{
		*x = *y = *w = *h = 0;
		return;
	}
	/* Inside the bevel: 2 pixels at the sides, 1 at the top and bottom */
	*x = t->px + 2;
	*y = t->py + 1;
	*w = t->pw - 4;
	*h = t->ph - 2;
}
