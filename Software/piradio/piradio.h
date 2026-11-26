/*
 * PiRadio - shared declarations.
 *
 * piradio.c holds what both displays share: the connection to the Pi, what
 * is known about the radio, and the actions. panel.c draws the radio on its
 * own screen; window.c draws it in a window on the Workbench (the WINDOW
 * ToolType). A display redraws only what the "changed" flags name.
 */

#ifndef PIRADIO_H
#define PIRADIO_H

#include <exec/types.h>

#define MAX_BANDS	10
#define MAX_REGIONS	10
#define MAX_GAINS	40
#define MAX_ACTIVE	200
#define SQL_MAX		200
#define SQL_STEP	5

/* Connection states */
#define CS_DOWN		0	/* not connected, try again after a pause */
#define CS_CONNECTING	1
#define CS_UP		2
#define CS_CLOSING	3	/* reset sent, waiting for the requests */

/* What has changed since a display last drew it */
#define CH_FREQ		0x0001	/* the frequency, or the frequency being typed */
#define CH_DIAL		0x0002	/* the band, its scale or its active channels */
#define CH_BANDS	0x0004	/* the band list, the current band or region */
#define CH_LAMPS	0x0008	/* tuner, signal, audio or scan lamp */
#define CH_LEVEL	0x0010
#define CH_MSG		0x0020
#define CH_VOL		0x0040
#define CH_SQL		0x0080
#define CH_GAIN		0x0100
#define CH_SCAN		0x0200
#define CH_ALL		0x03FF

struct Band
{
	LONG lo, hi, step;
	char mode[6];
	char shortname[8];
	char name[32];
};

/* One of the two displays */
struct Display
{
	BOOL (*open)(void);
	void (*close)(void);
	ULONG (*signals)(void);		/* to wait for */
	void (*handle)(void);		/* after one of them arrived */
	void (*refresh)(void);		/* draw what has changed */
	void (*tick)(void);		/* 10 times a second */
	void (*help)(void);		/* the Help key, or NULL */
	void (*to_back)(void);		/* the W key, or NULL */
	struct Window *(*window)(void);	/* for requesters */
};

extern const struct Display panel_display, window_display;

/* What is known about the radio (piradio.c) */
extern struct Band bands[MAX_BANDS];
extern int nbands, cur_band;
extern char regions[MAX_REGIONS][32];
extern int nregions, cur_region;
extern WORD gains[MAX_GAINS];
extern int ngains;
extern LONG active[MAX_ACTIVE];	/* active channels of the current band */
extern int nactive;
extern LONG freq;
extern int sql, gain, vol, scan_dir;
extern int run_status, pipe_status, level;
extern BOOL have_state;
extern char msg[80];
extern int conn_state;
extern char entry[12];		/* frequency being typed, in MHz */
extern int entry_len;		/* -1 when not typing */
extern int blink;		/* counts ticks */
extern int changed;
extern BOOL quit;
extern BOOL ui_dragging;	/* the user is dragging the dial or a control */

/* Actions (piradio.c) */
BOOL can_tune(void);
void tune_to(LONG f);
void tune_steps(LONG n);
void select_band(int i);
void next_region(void);
void select_region(int i);
void do_seek(int dir);
void toggle_scan(int dir);
void do_sweep(void);
void set_vol(int v);
void set_sql(int s);
int gain_index(void);		/* 0 = automatic, else 1 + the gain's index */
void set_gain_index(int i);
void handle_rawkey(UWORD code, UWORD qual, APTR prev);
void handle_char(char c, UWORD qual);	/* a key as a character */
void report(const char *text);

/* Helpers */
void format_mhz(char *buf, LONG hz);
LONG scale_long(LONG a, LONG mul, LONG div);

#endif
