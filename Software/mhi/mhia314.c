/*
 * mhia314.library - an MHI driver that has the Raspberry Pi decode MPEG
 * audio, through the A314.
 *
 * MHI is the interface that players such as AmigaAMP use for MPEG audio
 * decoders (see the MHI developer archive on Aminet, by Paul Qureshi and
 * Thomas Wenzel). The player queues buffers of MPEG data; this driver
 * copies them into a ring buffer in A314 memory, from which the mhi
 * service on the Pi (mhi.py) reads them as its decoder needs them. A
 * buffer is given back to the player when the Pi has taken all of it.
 * The decoded audio is played through PiAudio.
 *
 * The MHI definitions used here are written out below rather than taken
 * from the developer archive's libraries/mhi.h, so that nothing else is
 * needed to build this.
 *
 * Calls from the player only change the decoder's state and wake the
 * worker process, which does the work and talks to a314.device. Both work
 * under Forbid() while they touch the shared state. One decoder can be
 * allocated at a time (the Pi plays one stream).
 *
 * Kickstart 1.3 or later.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <libraries/dos.h>
#include <libraries/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../a314device/a314.h"
#include "../a314device/proto_a314.h"

#define SysBase (*(struct ExecBase **)4)

struct DosLibrary *DOSBase;
struct Library *A314Base;

#define LIBRARY_NAME	"mhia314.library"
#define SERVICE_NAME	"mhi"

/* From libraries/mhi.h (MHI developer archive 1.2) */
#define MHIF_PLAYING		0
#define MHIF_STOPPED		1
#define MHIF_OUT_OF_DATA	2
#define MHIF_PAUSED		3
#define MHIF_FALSE		0
#define MHIF_TRUE		1
#define MHIQ_DECODER_NAME	1000
#define MHIQ_DECODER_VERSION	1001
#define MHIQ_AUTHOR		1002
#define MHIQ_IS_HARDWARE	1010
#define MHIQ_IS_68K		1011
#define MHIQ_IS_PPC		1012
#define MHIQ_CAPABILITIES	0
#define MHIQ_MPEG1		1
#define MHIQ_MPEG2		2
#define MHIQ_MPEG25		3
#define MHIQ_MPEG4		4
#define MHIQ_LAYER1		10
#define MHIQ_LAYER2		11
#define MHIQ_LAYER3		12
#define MHIQ_VARIABLE_BITRATE	20
#define MHIQ_JOINT_STEREO	21
#define MHIQ_BASS_CONTROL	30
#define MHIQ_TREBLE_CONTROL	31
#define MHIQ_MID_CONTROL	32
#define MHIQ_PREFACTOR_CONTROL	33
#define MHIQ_5_BAND_EQ		34
#define MHIQ_10_BAND_EQ		35
#define MHIQ_VOLUME_CONTROL	40
#define MHIQ_PANNING_CONTROL	41
#define MHIQ_CROSSMIXING	42
#define MHIP_COUNT		14	/* parameters 0 (volume) to 13 (band 9) */

#define RING_SIZE	16384		/* MPEG data on its way to the Pi */
#define COPY_CHUNK	1024		/* copied per pass of the worker (under Forbid) */
#define WORKER_PRI	5
#define WORKER_STACK	4096
#define OPEN_TIMEOUT	5		/* seconds */

/* Worker signals, given to its ports before it runs */
#define SIGB_CMD	30		/* the player changed something (no port) */
#define SIGB_CTL	29		/* allocate or free a decoder */
#define SIGB_A314	28
#define SIGB_TIMER	27

/* A buffer queued by the player */
struct QNode
{
	struct MinNode node;
	UBYTE *buffer;
	ULONG size;
	ULONG copied;			/* into the ring */
	ULONG taken;			/* by the Pi */
};

/* Asks the worker to connect or disconnect */
struct CtlMsg
{
	struct Message msg;
	BOOL alloc;
	BOOL ok;
};

/* The one decoder */
struct Decoder
{
	struct Task *player;
	ULONG player_sig;
	BOOL allocated;
	volatile UBYTE status;		/* MHIF_... */

	struct MinList queue;		/* being played */
	struct MinList empty;		/* given back, for MHIGetEmpty() */

	/* What the player asked for, not yet passed on */
	BOOL want_stop;
	BOOL want_play;			/* TRUE play, FALSE pause */
	BOOL play_sent;			/* the last PLAY/PAUSE sent was PLAY */
	BOOL play_changed;
	UBYTE params[MHIP_COUNT];
	UWORD params_dirty;		/* a bit per parameter */

	/* Connection and ring */
	BOOL connected, connecting, closing;
	struct CtlMsg *ctl;
	UBYTE generation;
	UWORD timeout;
	ULONG ring;			/* A314 address */
	ULONG wpos;			/* where the next byte goes */
	ULONG used;			/* in the ring, not yet taken */
	ULONG unsent;			/* copied, not yet announced with DATA */
	BOOL ring_sent;
	BOOL stop_pending;		/* STOP sent, STOPPED not yet back */
	BOOL dry;			/* the Pi has decoded everything sent */

	struct A314_IORequest read_ior, write_ior, reset_ior;
	BOOL read_pending, write_pending, reset_pending;
	UBYTE in_buf[256];
	UBYTE out_buf[64];
};

struct MhiBase
{
	struct Library lib;
	BPTR seg_list;
	struct A314_IORequest a314_ior;	/* keeps a314.device open */
	struct Process *worker;
	struct MsgPort ctl_port, a314_port, timer_port;
	struct timerequest timer_req;
	BOOL timer_open, timer_pending;
	struct Decoder dec;
};

const char library_name[] = LIBRARY_NAME;
const char id_string[] = LIBRARY_NAME " 1.0 (25.11.2026)";
static const char service_name[] = SERVICE_NAME;
static const char capabilities[] = "audio/mpeg{audio/mp1,audio/mp2,audio/mp3}";
static const char decoder_name[] = "A314 (Raspberry Pi, libmpg123)";
static const char decoder_version[] = "1.0";
static const char author[] = "A314 project";

static struct MhiBase *the_base;

extern void worker_seglist();

/* ---------------------------------------------------------------------- */
/* Helpers (there is no C library)                                        */

static void new_list(struct MinList *l)
{
	l->mlh_Head = (struct MinNode *)&l->mlh_Tail;
	l->mlh_Tail = NULL;
	l->mlh_TailPred = (struct MinNode *)&l->mlh_Head;
}

static void init_port(struct MsgPort *mp, UBYTE sig_bit)
{
	mp->mp_Node.ln_Type = NT_MSGPORT;
	mp->mp_Flags = PA_SIGNAL;
	mp->mp_SigBit = sig_bit;
	mp->mp_SigTask = NULL;
	new_list((struct MinList *)&mp->mp_MsgList);
}

static char *put_str(char *dst, const char *s)
{
	while (*s)
		*dst++ = *s++;
	return dst;
}

/* Without division, which on the 68000 would need the C library */
static char *put_num(char *dst, ULONG n)
{
	static const ULONG powers[] = { 1000000000, 100000000, 10000000, 1000000,
		100000, 10000, 1000, 100, 10, 1 };
	BOOL started = FALSE;
	int i;
	for (i = 0; i < 10; i++)
	{
		char digit = '0';
		while (n >= powers[i])
		{
			n -= powers[i];
			digit++;
		}
		if (digit != '0' || started || i == 9)
		{
			*dst++ = digit;
			started = TRUE;
		}
	}
	return dst;
}

static ULONG get_num(const UBYTE *p, const UBYTE *end)
{
	ULONG n = 0;
	while (p < end && *p >= '0' && *p <= '9')
		n = (n << 3) + (n << 1) + (*p++ - '0');
	return n;
}

static BOOL starts_with(const UBYTE *p, const UBYTE *end, const char *word, const UBYTE **rest)
{
	while (*word)
	{
		if (p >= end || *p != *word)
			return FALSE;
		p++;
		word++;
	}
	if (p < end && *p != ' ')
		return FALSE;
	*rest = p < end ? p + 1 : p;
	return TRUE;
}

static void wake_player(struct Decoder *d)
{
	if (d->player)
		Signal(d->player, d->player_sig);
}

/* ---------------------------------------------------------------------- */
/* Talking to a314.device (worker, under Forbid)                          */

static void a314_cmd(struct Decoder *d, struct A314_IORequest *ior, UWORD cmd, UBYTE *buf, WORD len)
{
	ior->a314_Request.io_Command = cmd;
	ior->a314_Request.io_Error = 0;
	ior->a314_Socket = (ULONG)d + d->generation;
	ior->a314_Buffer = (STRPTR)buf;
	ior->a314_Length = len;
	SendIO((struct IORequest *)ior);
}

static void send_line(struct Decoder *d, char *end)
{
	a314_cmd(d, &d->write_ior, A314_WRITE, d->out_buf, end - (char *)d->out_buf);
	d->write_pending = TRUE;
}

static void start_read(struct Decoder *d)
{
	if (d->connected && !d->read_pending && !d->closing)
	{
		a314_cmd(d, &d->read_ior, A314_READ, d->in_buf, sizeof(d->in_buf));
		d->read_pending = TRUE;
	}
}

static void reset_socket(struct Decoder *d)
{
	if (!d->reset_pending)
	{
		a314_cmd(d, &d->reset_ior, A314_RESET, NULL, 0);
		d->reset_pending = TRUE;
	}
}

/* ---------------------------------------------------------------------- */
/* The stream (worker, under Forbid)                                      */

/* Copies a chunk of queued data into the ring, if there is room. Only
   one chunk per pass, so that Forbid() isn't held long; the worker wakes
   itself again if there is more to do. Nothing is copied between a Stop
   and the Pi's STOPPED, as the ring then starts again. */
static void fill_ring(struct Decoder *d)
{
	struct QNode *q;
	ULONG n;

	if (d->stop_pending || d->want_stop || !d->ring_sent || !d->connected || d->used >= RING_SIZE)
		return;
	for (q = (struct QNode *)d->queue.mlh_Head; q->node.mln_Succ; q = (struct QNode *)q->node.mln_Succ)
		if (q->copied < q->size)
			break;
	if (!q->node.mln_Succ)
		return;
	n = q->size - q->copied;
	if (n > RING_SIZE - d->used)
		n = RING_SIZE - d->used;
	if (n > RING_SIZE - d->wpos)
		n = RING_SIZE - d->wpos;
	if (n > COPY_CHUNK)
		n = COPY_CHUNK;
	WriteMemA314(d->ring + d->wpos, q->buffer + q->copied, n);
	q->copied += n;
	d->wpos += n;
	if (d->wpos == RING_SIZE)
		d->wpos = 0;
	d->used += n;
	d->unsent += n;
	SetSignal(1UL << SIGB_CMD, 1UL << SIGB_CMD);	/* maybe more */
}

/* The Pi has read n bytes: give back the buffers it has finished */
static void taken(struct Decoder *d, ULONG n)
{
	struct QNode *q;
	BOOL gave = FALSE;

	if (d->stop_pending)
		return;		/* from before the STOP: those buffers are gone */
	d->used = n > d->used ? 0 : d->used - n;
	while (n && (q = (struct QNode *)d->queue.mlh_Head)->node.mln_Succ)
	{
		ULONG k = q->copied - q->taken;
		if (k > n)
			k = n;
		q->taken += k;
		n -= k;
		if (q->taken == q->size)
		{
			Remove((struct Node *)q);
			AddTail((struct List *)&d->empty, (struct Node *)q);
			gave = TRUE;
		}
		else if (k == 0)
			break;
	}
	if (gave)
		wake_player(d);
}

static void handle_line(struct Decoder *d, const UBYTE *p, const UBYTE *end)
{
	const UBYTE *rest;

	if (starts_with(p, end, "TAKEN", &rest))
		taken(d, get_num(rest, end));
	else if (starts_with(p, end, "DRY", &rest))
	{
		d->dry = TRUE;
		/* Out of data only if nothing is waiting. The player is told, as
		   it may be waiting for the end of the stream; only on the change,
		   so that a player that calls MHIPlay at OUT_OF_DATA isn't woken
		   over and over while it has nothing to queue. */
		if (d->status == MHIF_PLAYING && !d->unsent && !d->used &&
			!((struct QNode *)d->queue.mlh_Head)->node.mln_Succ)
		{
			d->status = MHIF_OUT_OF_DATA;
			wake_player(d);
		}
	}
	else if (starts_with(p, end, "STOPPED", &rest))
	{
		d->stop_pending = FALSE;
		d->wpos = d->used = d->unsent = 0;
		d->dry = FALSE;
	}
	/* PIPE and ERR are only informative */
}

/* Sends the next line to the Pi, if a314.device has taken the last one */
static void send_next(struct Decoder *d)
{
	char *p = (char *)d->out_buf;
	int i;

	if (!d->connected || d->write_pending || d->closing)
		return;

	if (!d->ring_sent)
	{
		p = put_num(put_str(p, "RING "), d->ring);
		*p++ = ' ';
		d->ring_sent = TRUE;
		send_line(d, put_num(p, RING_SIZE));
		return;
	}
	if (d->want_stop)
	{
		d->want_stop = FALSE;
		d->stop_pending = TRUE;
		d->play_sent = FALSE;
		send_line(d, put_str(p, "STOP"));
		return;
	}
	if (d->params_dirty)
	{
		for (i = 0; i < MHIP_COUNT; i++)
			if (d->params_dirty & (1 << i))
				break;
		d->params_dirty &= ~(1 << i);
		p = put_num(put_str(p, "PARAM "), i);
		*p++ = ' ';
		send_line(d, put_num(p, d->params[i]));
		return;
	}
	if (d->unsent && !d->stop_pending)
	{
		ULONG n = d->unsent;
		d->unsent = 0;
		d->dry = FALSE;
		send_line(d, put_num(put_str(p, "DATA "), n));
		return;
	}
	if (d->play_changed)
	{
		d->play_changed = FALSE;
		d->play_sent = d->want_play;
		send_line(d, put_str(p, d->want_play ? "PLAY" : "PAUSE"));
		return;
	}
}

/* ---------------------------------------------------------------------- */
/* Connecting and disconnecting (worker)                                  */

static void start_timer(struct MhiBase *base)
{
	if (base->timer_open && !base->timer_pending)
	{
		base->timer_req.tr_time.tv_secs = 1;
		base->timer_req.tr_time.tv_micro = 0;
		SendIO((struct IORequest *)&base->timer_req);
		base->timer_pending = TRUE;
	}
}

/* Answers the CtlMsg once a314.device has returned everything */
static void check_done(struct Decoder *d)
{
	if (!d->ctl || d->read_pending || d->write_pending || d->reset_pending)
		return;
	if (d->closing || !d->connected)
	{
		d->ctl->ok = d->ctl->alloc ? FALSE : TRUE;
		d->connected = d->connecting = d->closing = FALSE;
	}
	else
		return;
	ReplyMsg(&d->ctl->msg);
	d->ctl = NULL;
}

static void handle_a314_reply(struct Decoder *d, struct A314_IORequest *ior)
{
	BYTE err = ior->a314_Request.io_Error;

	switch (ior->a314_Request.io_Command)
	{
	case A314_CONNECT:
		d->read_pending = FALSE;
		d->connecting = FALSE;
		if (err == A314_CONNECT_OK && !d->closing)
		{
			d->connected = TRUE;
			start_read(d);
			ReplyMsg(&d->ctl->msg);	/* ok was set TRUE */
			d->ctl = NULL;
		}
		else
			d->closing = TRUE;
		break;

	case A314_READ:
		d->read_pending = FALSE;
		if (err == A314_READ_OK)
		{
			UBYTE *p = d->in_buf;
			handle_line(d, p, p + d->read_ior.a314_Length);
			start_read(d);
		}
		else if (!d->closing)
		{
			/* The Pi went away: give everything back, and report it as
			   stopped */
			d->closing = TRUE;
			d->connected = FALSE;
			d->status = MHIF_STOPPED;
			while (d->queue.mlh_Head->mln_Succ)
				AddTail((struct List *)&d->empty, RemHead((struct List *)&d->queue));
			wake_player(d);
			reset_socket(d);
		}
		break;

	case A314_WRITE:
		d->write_pending = FALSE;
		break;

	case A314_RESET:
		d->reset_pending = FALSE;
		break;
	}
}

static void start_alloc(struct MhiBase *base, struct Decoder *d, struct CtlMsg *cm)
{
	d->ctl = cm;
	cm->ok = TRUE;
	d->connected = d->closing = FALSE;
	d->connecting = TRUE;
	d->ring_sent = d->stop_pending = d->dry = FALSE;
	d->wpos = d->used = d->unsent = 0;
	d->generation++;
	d->timeout = OPEN_TIMEOUT;
	a314_cmd(d, &d->read_ior, A314_CONNECT, (UBYTE *)service_name, sizeof(service_name) - 1);
	d->read_pending = TRUE;
	start_timer(base);
}

static void start_free(struct Decoder *d, struct CtlMsg *cm)
{
	d->ctl = cm;
	d->closing = TRUE;
	if (d->connected || d->connecting)
		reset_socket(d);
	d->connected = FALSE;
}

/* Once a second while connecting */
static void tick(struct MhiBase *base)
{
	struct Decoder *d = &base->dec;
	if (d->connecting && d->ctl)
	{
		if (--d->timeout == 0)
		{
			d->closing = TRUE;
			reset_socket(d);
		}
		else
			start_timer(base);
	}
}

void worker_run()
{
	struct MhiBase *base = the_base;
	struct Decoder *d = &base->dec;
	struct Process *me = (struct Process *)FindTask(NULL);
	struct Message *msg;

	me->pr_WindowPtr = (APTR)-1;

	init_port(&base->timer_port, SIGB_TIMER);
	base->timer_port.mp_SigTask = &me->pr_Task;
	base->timer_req.tr_node.io_Message.mn_ReplyPort = &base->timer_port;
	base->timer_req.tr_node.io_Message.mn_Length = sizeof(struct timerequest);
	base->timer_open = OpenDevice(TIMERNAME, UNIT_VBLANK, (struct IORequest *)&base->timer_req, 0) == 0;
	base->timer_req.tr_node.io_Command = TR_ADDREQUEST;

	for (;;)
	{
		ULONG sigs = Wait((1UL << SIGB_CMD) | (1UL << SIGB_CTL) | (1UL << SIGB_A314) | (1UL << SIGB_TIMER));

		Forbid();
		if (sigs & (1UL << SIGB_CTL))
			while ((msg = GetMsg(&base->ctl_port)))
			{
				struct CtlMsg *cm = (struct CtlMsg *)msg;
				if (cm->alloc)
				{
					if (base->timer_open)
						start_alloc(base, d, cm);
					else
					{
						cm->ok = FALSE;
						ReplyMsg(msg);
					}
				}
				else
					start_free(d, cm);
			}
		if (sigs & (1UL << SIGB_A314))
			while ((msg = GetMsg(&base->a314_port)))
				handle_a314_reply(d, (struct A314_IORequest *)msg);
		if ((sigs & (1UL << SIGB_TIMER)) && GetMsg(&base->timer_port))
		{
			base->timer_pending = FALSE;
			tick(base);
		}
		fill_ring(d);
		send_next(d);
		check_done(d);
		Permit();
	}
}

/* ---------------------------------------------------------------------- */
/* MHI functions (called by the player)                                   */

static void wake_worker(struct MhiBase *base)
{
	Signal(&base->worker->pr_Task, 1UL << SIGB_CMD);
}

static BOOL ask_worker(struct MhiBase *base, BOOL alloc)
{
	struct MsgPort port;
	struct CtlMsg cm;

	init_port(&port, SIGB_SINGLE);
	port.mp_SigTask = FindTask(NULL);
	cm.msg.mn_Node.ln_Type = NT_MESSAGE;
	cm.msg.mn_ReplyPort = &port;
	cm.msg.mn_Length = sizeof(cm);
	cm.alloc = alloc;
	cm.ok = FALSE;

	SetSignal(0, SIGF_SINGLE);
	PutMsg(&base->ctl_port, &cm.msg);
	while (!GetMsg(&port))
		Wait(SIGF_SINGLE);
	return cm.ok;
}

static void free_nodes(struct MinList *l)
{
	struct Node *n;
	while ((n = RemHead((struct List *)l)))
		FreeMem(n, sizeof(struct QNode));
}

static APTR alloc_decoder(__reg("a6") struct MhiBase *base, __reg("a0") struct Task *task,
	__reg("d0") ULONG signal)
{
	struct Decoder *d = &base->dec;
	int i;

	Forbid();
	if (d->allocated)
	{
		Permit();
		return NULL;
	}
	d->allocated = TRUE;
	Permit();

	d->ring = AllocMemA314(RING_SIZE);
	if (d->ring == INVALID_A314_ADDRESS)
	{
		d->allocated = FALSE;
		return NULL;
	}
	new_list(&d->queue);
	new_list(&d->empty);
	d->status = MHIF_STOPPED;
	d->want_stop = d->want_play = d->play_sent = d->play_changed = FALSE;
	for (i = 0; i < MHIP_COUNT; i++)
		d->params[i] = i == 0 ? 100 : i == 2 ? 0 : 50;
	d->params_dirty = 0;
	d->player = NULL;		/* not woken while connecting */

	if (!ask_worker(base, TRUE))
	{
		FreeMemA314(d->ring, RING_SIZE);
		d->allocated = FALSE;
		return NULL;
	}
	Forbid();
	d->player = task;
	d->player_sig = signal;
	Permit();
	return d;
}

static void free_decoder(__reg("a6") struct MhiBase *base, __reg("a3") struct Decoder *d)
{
	if (!d || !d->allocated)
		return;
	Forbid();
	d->player = NULL;
	Permit();
	ask_worker(base, FALSE);
	Forbid();
	free_nodes(&d->queue);
	free_nodes(&d->empty);
	Permit();
	FreeMemA314(d->ring, RING_SIZE);
	d->allocated = FALSE;
}

static BOOL queue_buffer(__reg("a6") struct MhiBase *base, __reg("a3") struct Decoder *d,
	__reg("a0") APTR buffer, __reg("d0") ULONG size)
{
	struct QNode *q;

	if (!buffer || !size || !(q = AllocMem(sizeof(struct QNode), MEMF_PUBLIC)))
		return FALSE;
	q->buffer = buffer;
	q->size = size;
	q->copied = q->taken = 0;
	Forbid();
	if (!d->connected)
	{
		Permit();
		FreeMem(q, sizeof(struct QNode));
		return FALSE;
	}
	AddTail((struct List *)&d->queue, (struct Node *)q);
	if (d->status == MHIF_OUT_OF_DATA)
		d->status = MHIF_PLAYING;
	Permit();
	wake_worker(base);
	return TRUE;
}

static APTR get_empty(__reg("a6") struct MhiBase *base, __reg("a3") struct Decoder *d)
{
	struct QNode *q;
	APTR buffer = NULL;

	Forbid();
	q = (struct QNode *)RemHead((struct List *)&d->empty);
	Permit();
	if (q)
	{
		buffer = q->buffer;
		FreeMem(q, sizeof(struct QNode));
	}
	return buffer;
}

/* ULONG, so that all of d0 is set (players may test all of it) */
static ULONG get_status(__reg("a6") struct MhiBase *base, __reg("a3") struct Decoder *d)
{
	return d->status;
}

static void play(__reg("a6") struct MhiBase *base, __reg("a3") struct Decoder *d)
{
	Forbid();
	if (d->connected)
	{
		/* Nothing to play: stays out of data, which the player sees
		   straight away (and the Pi is told only if it was paused) */
		if (d->dry && !d->unsent && !d->used && !d->queue.mlh_Head->mln_Succ)
		{
			d->status = MHIF_OUT_OF_DATA;
			if (!d->play_sent)
			{
				d->want_play = TRUE;
				d->play_changed = TRUE;
			}
		}
		else
		{
			d->want_play = TRUE;
			d->play_changed = TRUE;
			d->status = MHIF_PLAYING;
		}
	}
	Permit();
	wake_worker(base);
}

/* Pausing while paused resumes, as the MAS driver does, for players
   written for it */
static void pause(__reg("a6") struct MhiBase *base, __reg("a3") struct Decoder *d)
{
	if (d->status == MHIF_PAUSED)
	{
		play(base, d);
		return;
	}
	Forbid();
	if (d->connected && (d->status == MHIF_PLAYING || d->status == MHIF_OUT_OF_DATA))
	{
		d->want_play = FALSE;
		d->play_changed = TRUE;
		d->status = MHIF_PAUSED;
	}
	Permit();
	wake_worker(base);
}

/* Stops. Every buffer is free again at once, and none comes back
   through MHIGetEmpty(), as with the MAS driver: players may free or
   reuse their buffers straight after MHIStop(). */
static void stop(__reg("a6") struct MhiBase *base, __reg("a3") struct Decoder *d)
{
	struct Node *n;

	Forbid();
	while ((n = RemHead((struct List *)&d->queue)))
		FreeMem(n, sizeof(struct QNode));
	while ((n = RemHead((struct List *)&d->empty)))
		FreeMem(n, sizeof(struct QNode));
	d->want_stop = TRUE;
	d->dry = FALSE;
	d->want_play = FALSE;
	d->play_changed = FALSE;
	d->unsent = 0;
	d->status = MHIF_STOPPED;
	Permit();
	wake_worker(base);
}

static ULONG query(__reg("a6") struct MhiBase *base, __reg("d1") ULONG what)
{
	switch (what)
	{
	case MHIQ_CAPABILITIES:
		return (ULONG)capabilities;
	case MHIQ_DECODER_NAME:
		return (ULONG)decoder_name;
	case MHIQ_DECODER_VERSION:
		return (ULONG)decoder_version;
	case MHIQ_AUTHOR:
		return (ULONG)author;
	/* Decoded outside the Amiga, by other hardware */
	case MHIQ_IS_HARDWARE:
		return MHIF_TRUE;
	case MHIQ_MPEG1:
	case MHIQ_MPEG2:
	case MHIQ_MPEG25:
	case MHIQ_LAYER1:
	case MHIQ_LAYER2:
	case MHIQ_LAYER3:
	case MHIQ_VARIABLE_BITRATE:
	case MHIQ_JOINT_STEREO:
	case MHIQ_BASS_CONTROL:
	case MHIQ_TREBLE_CONTROL:
	case MHIQ_MID_CONTROL:
	case MHIQ_PREFACTOR_CONTROL:
	case MHIQ_5_BAND_EQ:
	case MHIQ_10_BAND_EQ:
	case MHIQ_VOLUME_CONTROL:
	case MHIQ_PANNING_CONTROL:
	case MHIQ_CROSSMIXING:
		return MHIF_TRUE;
	default:
		return MHIF_FALSE;
	}
}

static void set_param(__reg("a6") struct MhiBase *base, __reg("a3") struct Decoder *d,
	__reg("d0") UWORD param, __reg("d1") ULONG value)
{
	if (!d || param >= MHIP_COUNT)
		return;
	Forbid();
	d->params[param] = value > 100 ? 100 : value;
	d->params_dirty |= 1 << param;
	Permit();
	wake_worker(base);
}

/* ---------------------------------------------------------------------- */
/* Library functions                                                      */

static struct Library *lib_open(__reg("a6") struct MhiBase *base)
{
	base->lib.lib_OpenCnt++;
	base->lib.lib_Flags &= ~LIBF_DELEXP;
	return &base->lib;
}

static BPTR lib_close(__reg("a6") struct MhiBase *base)
{
	base->lib.lib_OpenCnt--;
	return 0;
}

/* The worker process stays, so the library isn't unloaded (as with the
   A314 devices). */
static BPTR lib_expunge(__reg("a6") struct MhiBase *base)
{
	base->lib.lib_Flags |= LIBF_DELEXP;
	return 0;
}

static ULONG lib_null(void)
{
	return 0;
}

static struct Library *init_library(__reg("a6") struct ExecBase *sys_base, __reg("a0") BPTR seg_list,
	__reg("d0") struct MhiBase *base)
{
	struct MsgPort *worker_port;
	struct Decoder *d = &base->dec;

	base->seg_list = seg_list;
	base->lib.lib_Node.ln_Type = NT_LIBRARY;
	base->lib.lib_Node.ln_Name = (char *)library_name;
	base->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
	base->lib.lib_Version = 1;
	base->lib.lib_Revision = 0;
	base->lib.lib_IdString = (APTR)id_string;
	the_base = base;

	init_port(&base->ctl_port, SIGB_CTL);
	init_port(&base->a314_port, SIGB_A314);

	base->a314_ior.a314_Request.io_Message.mn_ReplyPort = &base->a314_port;
	base->a314_ior.a314_Request.io_Message.mn_Length = sizeof(struct A314_IORequest);
	if (OpenDevice(A314_NAME, 0, (struct IORequest *)&base->a314_ior, 0))
		goto fail;
	A314Base = (struct Library *)base->a314_ior.a314_Request.io_Device;
	d->read_ior = base->a314_ior;
	d->write_ior = base->a314_ior;
	d->reset_ior = base->a314_ior;
	new_list(&d->queue);
	new_list(&d->empty);

	DOSBase = (struct DosLibrary *)OpenLibrary(DOSNAME, 0);
	if (!DOSBase)
		goto fail_a314;

	Forbid();
	worker_port = CreateProc((char *)library_name, WORKER_PRI, ((ULONG)worker_seglist) >> 2, WORKER_STACK);
	if (worker_port)
	{
		base->worker = (struct Process *)((char *)worker_port - sizeof(struct Task));
		base->ctl_port.mp_SigTask = &base->worker->pr_Task;
		base->a314_port.mp_SigTask = &base->worker->pr_Task;
		/* Reserved here, not with AllocSignal() in the worker, which
		   would clear a signal that is already waiting */
		base->worker->pr_Task.tc_SigAlloc |= (1UL << SIGB_CMD) | (1UL << SIGB_CTL) |
			(1UL << SIGB_A314) | (1UL << SIGB_TIMER);
	}
	Permit();
	if (!worker_port)
		goto fail_dos;
	return &base->lib;

fail_dos:
	CloseLibrary((struct Library *)DOSBase);
fail_a314:
	CloseDevice((struct IORequest *)&base->a314_ior);
fail:
	FreeMem((char *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
	return NULL;
}

static const ULONG library_vectors[] =
{
	(ULONG)lib_open,
	(ULONG)lib_close,
	(ULONG)lib_expunge,
	(ULONG)lib_null,
	(ULONG)alloc_decoder,		/* -30 */
	(ULONG)free_decoder,
	(ULONG)queue_buffer,
	(ULONG)get_empty,
	(ULONG)get_status,
	(ULONG)play,
	(ULONG)stop,
	(ULONG)pause,
	(ULONG)query,
	(ULONG)set_param,		/* -84 */
	(ULONG)-1,
};

const ULONG auto_init_tables[] =
{
	sizeof(struct MhiBase),
	(ULONG)library_vectors,
	0,
	(ULONG)init_library,
};
