/*
 * a314gui - the windows of the A314 tools (the Prefs/A314 editors and the
 * like), described once and built the way the system's own tools are.
 *
 * On Workbench 2.04 and later the gadgets are made with gadtools.library,
 * in the screen's font, with menus and keyboard shortcuts, so that a tool
 * looks and works like a built-in preferences editor. On Workbench 1.3,
 * which has no gadtools.library, the same window is made from plain
 * Intuition gadgets in topaz 8, with the same 3D look.
 *
 * A window is a list of items placed on a grid: x and w in columns (the
 * width of an average character of the font), y and h in rows (one gadget
 * high). An item's label goes to its left (to its right for a checkbox;
 * inside for a button). In a label, '_' marks the keyboard shortcut.
 */

#ifndef A314GUI_H
#define A314GUI_H

#include <exec/types.h>
#include <intuition/intuition.h>

/* Kinds of item */
#define GUI_BUTTON	1
#define GUI_TEXT	2	/* read-only text in a recessed box */
#define GUI_STRING	3
#define GUI_INTEGER	4
#define GUI_CHECKBOX	5
#define GUI_CYCLE	6	/* one of several choices */
#define GUI_LIST	7	/* a scrolling list to choose from */
#define GUI_LABEL	8	/* plain text, set with gui_set_text() */

struct GuiItem
{
	UWORD kind;
	UWORD id;		/* reported in events; unique, not 0 */
	WORD x, y, w, h;	/* grid position and size (h only for lists) */
	const char *label;
	LONG max;		/* longest text of a string gadget */
};

/* Menus (2.04 and later): a list ending with GUI_MENU_END. A GUI_MENU_ITEM
   gives an event with its id; key is its Right-Amiga shortcut, or 0. */
#define GUI_MENU_END	0
#define GUI_MENU_TITLE	1
#define GUI_MENU_ITEM	2
#define GUI_MENU_BAR	3

struct GuiMenu
{
	UBYTE type;
	char key;
	UWORD id;
	const char *label;
};

/* Events */
#define GUI_EV_NONE	0
#define GUI_EV_CLOSE	1	/* the close gadget, or Esc */
#define GUI_EV_GADGET	2	/* id: the item; value: see gui_wait() */
#define GUI_EV_MENU	3	/* id: the menu item */

struct GuiEvent
{
	UWORD type;
	UWORD id;
	LONG value;
};

struct Gui;

/* Opens a window on the Workbench (the default public screen), centred.
   Returns NULL if it can't. */
struct Gui *gui_open(const char *title, const struct GuiItem *items, int nitems,
	const struct GuiMenu *menus);
void gui_close(struct Gui *gui);

/* The signal to Wait() for, and the next event (FALSE when there are no
   more). value is the choice for a cycle gadget, the line for a list, 1 or
   0 for a checkbox. */
ULONG gui_signal(struct Gui *gui);
BOOL gui_event(struct Gui *gui, struct GuiEvent *ev);

/* Changing items */
void gui_set_text(struct Gui *gui, UWORD id, const char *text);	/* TEXT, LABEL, STRING */
void gui_set_number(struct Gui *gui, UWORD id, LONG n);		/* INTEGER */
void gui_set_check(struct Gui *gui, UWORD id, BOOL on);		/* CHECKBOX */
void gui_set_cycle(struct Gui *gui, UWORD id, const char **labels, int n, int active);
void gui_set_list(struct Gui *gui, UWORD id, const char **lines, int n, int selected);
void gui_disable(struct Gui *gui, UWORD id, BOOL off);

/* Reading items */
const char *gui_get_text(struct Gui *gui, UWORD id);		/* STRING */
LONG gui_get_number(struct Gui *gui, UWORD id);			/* INTEGER */
BOOL gui_get_check(struct Gui *gui, UWORD id);
int gui_get_cycle(struct Gui *gui, UWORD id);

/* A message in a requester, with one button; the window may be NULL (or
   closed), then it goes to the Workbench screen */
void gui_message(struct Gui *gui, const char *text);

/* A question in a requester with two buttons, e.g. "Connect|Cancel";
   TRUE for the first */
BOOL gui_ask(struct Gui *gui, const char *text, const char *buttons);

/* Puts the cursor in a STRING or INTEGER item (2.0 and later) */
void gui_activate(struct Gui *gui, UWORD id);

/* The window, for a tool that needs it (e.g. for its own requesters) */
struct Window *gui_window(struct Gui *gui);

#endif
