/*
 * a314serial.device - serial ports on the Raspberry Pi, used from the
 * Amiga through the A314.
 *
 * It works like serial.device (see devices/serial.h and the serial.device
 * autodoc), so programs that let you choose the serial device, such as
 * terminal programs, can use it. Each unit is one Pi serial port, chosen
 * with Prefs/A314/Serial, which stores the port's name in the environment
 * variable A314/SerialUnit<n> (unit 0 to 3). Without it, or with "auto",
 * a unit uses the first free port.
 *
 * Each open unit is a connection to the piserial service on the Pi; see
 * piserial.py for the messages. Only the commands of serial.device are
 * known; the modem lines can be read (SDCMD_QUERY) but not set.
 *
 * Requests are passed to a worker process, which talks to a314.device.
 * The worker is a process, rather than a task, so that it can read the
 * environment variables. It works under Forbid(), except while it reads
 * them, so that AbortIO() (which runs in the caller's task) can take
 * requests off its lists safely.
 *
 * Written for and with Kickstart 1.3 and later.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <devices/serial.h>
#include <devices/timer.h>
#include <intuition/preferences.h>
#include <libraries/dos.h>
#include <libraries/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

#include "../a314device/a314.h"

#define SysBase (*(struct ExecBase **)4)

struct DosLibrary *DOSBase;
struct IntuitionBase *IntuitionBase;

#define DEVICE_NAME	"a314serial.device"
#define SERVICE_NAME	"piserial"

#define UNIT_COUNT	4
#define WORKER_PRI	5
#define WORKER_STACK	4096

/* Must match piserial.py */
#define MAX_PACKET	252		/* a type byte and 251 data bytes */
#define MAX_DATA	(MAX_PACKET - 1)
#define TX_WINDOW	4096		/* Amiga data the Pi hasn't SENT yet */

#define MIN_RBUFLEN	64
#define MAX_RBUFLEN	65536
#define OPEN_TIMEOUT	5		/* seconds */
#define ERR_LINE	SerErr_LineErr	/* the connection to the Pi is gone */

/* Signals of the worker process. They are given to its ports before it
   runs, and it reserves them when it starts. */
#define SIGB_IO		30		/* requests from begin_io() */
#define SIGB_CTL	29		/* open and close */
#define SIGB_A314	28		/* replies from a314.device */
#define SIGB_TIMER	27

/* Unit states */
#define U_CLOSED	0
#define U_STARTING	1		/* Open() is waiting for the worker */
#define U_CONNECTING	2		/* A314_CONNECT sent */
#define U_OPENING	3		/* OPEN sent to the Pi */
#define U_OPEN		4
#define U_DEAD		5		/* the connection was lost */
#define U_CLOSING	6		/* waiting for a314.device to finish */

/* io_Status bits for SDCMD_QUERY: modem lines are active low */
#define ST_DSR		(1 << 3)
#define ST_CTS		(1 << 4)
#define ST_CD		(1 << 5)

struct SerParams
{
	ULONG ctl_char, rbuf_len, ext_flags, baud, brk_time;
	struct IOTArray term_array;
	UBYTE read_len, write_len, stop_bits, ser_flags;
};

/* Asks the worker to open or close a unit; sent from Open() and Close() */
struct CtlMsg
{
	struct Message msg;
	BOOL open;
	struct IOExtSer *ior;
	LONG error;
};

struct SerUnit
{
	UBYTE number;
	volatile UBYTE state;
	UWORD open_count;
	UBYTE generation;
	BOOL shared;
	struct CtlMsg *ctl;		/* open or close being done */
	UWORD timeout;			/* seconds left to open */

	struct A314_IORequest read_ior, write_ior, reset_ior;
	BOOL read_pending, write_pending, reset_pending;
	BOOL open_sent;

	struct SerParams params;
	struct SerParams defaults;	/* from Preferences, for CMD_RESET */
	char port[128];			/* the Pi port name, or "auto" */

	UBYTE *ring;			/* received data */
	ULONG ring_size, ring_head;
	volatile ULONG ring_count;
	ULONG credit;			/* granted to the Pi, not yet used */

	ULONG in_flight;		/* sent to the Pi, not yet SENT */
	struct List reads;		/* CMD_READ */
	struct List out;		/* CMD_WRITE, SDCMD_SETPARAMS, SDCMD_BREAK */
	BOOL set_waiting;		/* a SET was sent, OK or ERR not yet back */
	BOOL internal_set;		/* ... by CMD_RESET */
	BOOL need_clear, need_default_set;
	BOOL stopped;			/* CMD_STOP */
	volatile UWORD status;		/* for SDCMD_QUERY */

	UBYTE in_buf[MAX_PACKET + 4];
	UBYTE out_buf[MAX_PACKET];
};

struct SerialDevice
{
	struct Library lib;
	BPTR seg_list;
	struct Library *a314_base;
	struct A314_IORequest a314_ior;	/* keeps a314.device open */
	struct Process *worker;
	struct MsgPort io_port, ctl_port, a314_port;
	struct timerequest timer_req;
	BOOL timer_open, timer_pending;
	struct SerUnit units[UNIT_COUNT];
};

const char device_name[] = DEVICE_NAME;
const char id_string[] = DEVICE_NAME " 1.0 (25.11.2026)";
static const char service_name[] = SERVICE_NAME;

/* The device is loaded once, so the worker finds it here */
static struct SerialDevice *the_device;

extern void worker_seglist();

/* ---------------------------------------------------------------------- */
/* Small helpers (there is no C library)                                  */

static void new_list(struct List *l)
{
	l->lh_Head = (struct Node *)&l->lh_Tail;
	l->lh_Tail = NULL;
	l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static void init_port(struct MsgPort *mp, UBYTE sig_bit)
{
	mp->mp_Node.ln_Type = NT_MSGPORT;
	mp->mp_Flags = PA_SIGNAL;
	mp->mp_SigBit = sig_bit;
	mp->mp_SigTask = NULL;
	new_list(&mp->mp_MsgList);
}

static ULONG str_len(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return p - s;
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

/* Reads a decimal number at *p and moves *p past it and one space */
static ULONG get_num(const UBYTE **p, const UBYTE *end)
{
	ULONG n = 0;
	while (*p < end && **p >= '0' && **p <= '9')
		n = (n << 3) + (n << 1) + (*(*p)++ - '0');
	if (*p < end && **p == ' ')
		(*p)++;
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

static void reply(struct IORequest *ior, BYTE error)
{
	ior->io_Error = error;
	ReplyMsg(&ior->io_Message);
}

/* ---------------------------------------------------------------------- */
/* Settings                                                               */

/* The defaults, from Preferences, as serial.device takes them */
static void default_params(struct SerParams *sp)
{
	static const ULONG bauds[] = { 110, 300, 1200, 2400, 4800, 9600, 19200, 31250 };
	struct Preferences prefs;
	UBYTE parity, handshake, bits;

	sp->ctl_char = SER_DEFAULT_CTLCHAR;
	sp->rbuf_len = 512;
	sp->ext_flags = 0;
	sp->baud = 9600;
	sp->brk_time = 250000;
	sp->term_array.TermArray0 = 0;
	sp->term_array.TermArray1 = 0;
	sp->read_len = sp->write_len = 8;
	sp->stop_bits = 1;
	sp->ser_flags = 0;

	if (!IntuitionBase || !GetPrefs(&prefs, sizeof(prefs)))
		return;
	if (prefs.BaudRate < sizeof(bauds) / sizeof(bauds[0]))
		sp->baud = bauds[prefs.BaudRate];
	bits = 8 - (prefs.SerRWBits >> 4);
	if (bits >= 5 && bits <= 8)
		sp->read_len = sp->write_len = bits;
	sp->stop_bits = (prefs.SerStopBuf >> 4) == 1 ? 2 : 1;
	if ((prefs.SerStopBuf & 0x0f) <= 7)
		sp->rbuf_len = 512UL << (prefs.SerStopBuf & 0x0f);
	parity = prefs.SerParShk >> 4;
	handshake = prefs.SerParShk & 0x0f;
	if (parity != SPARITY_NONE)
		sp->ser_flags |= SERF_PARTY_ON;
	if (parity == SPARITY_ODD || parity == SPARITY_MARK)
		sp->ser_flags |= SERF_PARTY_ODD;
	if (parity == SPARITY_MARK || parity == SPARITY_SPACE)
		sp->ext_flags |= SEXTF_MSPON | (parity == SPARITY_MARK ? SEXTF_MARK : 0);
	if (handshake == SHSHAKE_RTS)
		sp->ser_flags |= SERF_7WIRE | SERF_XDISABLED;
	else if (handshake == SHSHAKE_NONE)
		sp->ser_flags |= SERF_XDISABLED;
}

static void params_from_ior(struct SerParams *sp, struct IOExtSer *ior)
{
	sp->ctl_char = ior->io_CtlChar;
	sp->rbuf_len = ior->io_RBufLen;
	sp->ext_flags = ior->io_ExtFlags;
	sp->baud = ior->io_Baud;
	sp->brk_time = ior->io_BrkTime;
	sp->term_array = ior->io_TermArray;
	sp->read_len = ior->io_ReadLen;
	sp->write_len = ior->io_WriteLen;
	sp->stop_bits = ior->io_StopBits;
	sp->ser_flags = ior->io_SerFlags;
}

static void params_to_ior(struct SerParams *sp, struct IOExtSer *ior)
{
	ior->io_CtlChar = sp->ctl_char;
	ior->io_RBufLen = sp->rbuf_len;
	ior->io_ExtFlags = sp->ext_flags;
	ior->io_Baud = sp->baud;
	ior->io_BrkTime = sp->brk_time;
	ior->io_TermArray = sp->term_array;
	ior->io_ReadLen = sp->read_len;
	ior->io_WriteLen = sp->write_len;
	ior->io_StopBits = sp->stop_bits;
	ior->io_SerFlags = sp->ser_flags;
}

static BOOL params_valid(struct SerParams *sp)
{
	return sp->read_len >= 5 && sp->read_len <= 8 && sp->write_len == sp->read_len &&
		sp->stop_bits >= 1 && sp->stop_bits <= 2 &&
		sp->rbuf_len >= MIN_RBUFLEN && sp->baud > 0;
}

/* "<baud> <bits> <parity> <stop> <handshake>", as piserial.py wants them */
static char *put_params(char *p, struct SerParams *sp)
{
	char parity = 'n';
	/* Mark/space parity implies parity on */
	if (sp->ext_flags & SEXTF_MSPON)
		parity = (sp->ext_flags & SEXTF_MARK) ? 'm' : 's';
	else if (sp->ser_flags & SERF_PARTY_ON)
		parity = (sp->ser_flags & SERF_PARTY_ODD) ? 'o' : 'e';
	p = put_num(p, sp->baud);
	*p++ = ' ';
	p = put_num(p, sp->read_len);
	*p++ = ' ';
	*p++ = parity;
	*p++ = ' ';
	p = put_num(p, sp->stop_bits);
	*p++ = ' ';
	if (sp->ser_flags & SERF_7WIRE)
		p = put_str(p, "rts");
	else if (!(sp->ser_flags & (SERF_XDISABLED | SERF_RAD_BOOGIE)))	/* RAD_BOOGIE implies no XON/XOFF */
		p = put_str(p, "xon");
	else
		p = put_str(p, "none");
	return p;
}

/* ---------------------------------------------------------------------- */
/* Talking to a314.device (worker only)                                   */

static void a314_cmd(struct SerUnit *u, struct A314_IORequest *ior, UWORD cmd, UBYTE *buf, WORD len)
{
	ior->a314_Request.io_Command = cmd;
	ior->a314_Request.io_Error = 0;
	ior->a314_Socket = (ULONG)u + u->generation;
	ior->a314_Buffer = (STRPTR)buf;
	ior->a314_Length = len;
	SendIO((struct IORequest *)ior);
}

static void start_read(struct SerUnit *u)
{
	if (!u->read_pending && (u->state == U_OPENING || u->state == U_OPEN))
	{
		a314_cmd(u, &u->read_ior, A314_READ, u->in_buf, sizeof(u->in_buf));
		u->read_pending = TRUE;
	}
}

static void send_line(struct SerUnit *u, char *end)
{
	a314_cmd(u, &u->write_ior, A314_WRITE, u->out_buf, end - (char *)u->out_buf);
	u->write_pending = TRUE;
}

/* Ends the connection; the unit is finished when a314.device has
   returned all its requests (see check_closed()). */
static void reset_socket(struct SerUnit *u)
{
	if (!u->reset_pending)
	{
		a314_cmd(u, &u->reset_ior, A314_RESET, NULL, 0);
		u->reset_pending = TRUE;
	}
}

/* ---------------------------------------------------------------------- */
/* Requests (worker only, under Forbid)                                   */

static BOOL in_list(struct List *l, struct Node *n);

static void fail_list(struct List *l, BYTE error)
{
	struct IORequest *ior;
	while ((ior = (struct IORequest *)RemHead(l)))
		reply(ior, error);
}

/* The head of the out list, if it can still be taken off (a SET or BREAK
   that was sent has to wait for the Pi) */
static BOOL head_in_progress(struct SerUnit *u)
{
	struct IOStdReq *head = (struct IOStdReq *)u->out.lh_Head;
	return u->out.lh_Head->ln_Succ && u->set_waiting && !u->internal_set &&
		head->io_Command == SDCMD_SETPARAMS;
}

/* CMD_FLUSH: everything waiting is aborted */
static void flush_requests(struct SerUnit *u)
{
	struct IORequest *keep = NULL;
	if (head_in_progress(u))
		keep = (struct IORequest *)RemHead(&u->out);
	fail_list(&u->reads, IOERR_ABORTED);
	fail_list(&u->out, IOERR_ABORTED);
	if (keep)
		AddHead(&u->out, &keep->io_Message.mn_Node);
}

/* The connection is gone: everything fails from now on */
static void lose_connection(struct SerUnit *u)
{
	if (u->state == U_OPEN)
	{
		u->state = U_DEAD;
		u->status |= ST_CD | ST_CTS | ST_DSR;
		u->set_waiting = FALSE;
		fail_list(&u->reads, ERR_LINE);
		fail_list(&u->out, ERR_LINE);
		reset_socket(u);
	}
}

/* Where the oldest received byte is */
static ULONG ring_tail(struct SerUnit *u)
{
	return u->ring_head >= u->ring_count ? u->ring_head - u->ring_count :
		u->ring_head + u->ring_size - u->ring_count;
}

static ULONG ring_free(struct SerUnit *u)
{
	return u->ring_size - u->ring_count - u->credit;
}

/* Gives received data to the waiting reads, oldest first */
static void serve_reads(struct SerUnit *u)
{
	struct IOStdReq *ior;
	while (u->ring_count && (ior = (struct IOStdReq *)u->reads.lh_Head)->io_Message.mn_Node.ln_Succ)
	{
		UBYTE *dst = (UBYTE *)ior->io_Data;
		BOOL eof = FALSE;
		UBYTE *term = (UBYTE *)&u->params.term_array;
		/* EOF mode can be set in the request itself */
		BOOL eof_mode = (((struct IOExtSer *)ior)->io_SerFlags & SERF_EOFMODE) != 0;
		/* A length of -1 reads up to and including a NUL */
		BOOL to_nul = ior->io_Length == (ULONG)-1;

		while (u->ring_count && (to_nul || ior->io_Actual < ior->io_Length) && !eof)
		{
			UBYTE c = u->ring[ring_tail(u)];
			dst[ior->io_Actual++] = c;
			u->ring_count--;
			if (to_nul && c == 0)
				eof = TRUE;
			if (eof_mode)
			{
				int i;
				for (i = 0; i < 8; i++)
					if (c == term[i])
						eof = TRUE;
			}
		}
		if (ior->io_Actual == ior->io_Length || eof)
		{
			Remove(&ior->io_Message.mn_Node);
			reply((struct IORequest *)ior, 0);
		}
	}
}

/* Data from the Pi */
static void received(struct SerUnit *u, UBYTE *data, ULONG n)
{
	if (n > u->credit)
		n = u->credit;	/* more than allowed: can't happen */
	u->credit -= n;
	while (n--)
	{
		u->ring[u->ring_head++] = *data++;
		if (u->ring_head == u->ring_size)
			u->ring_head = 0;
		u->ring_count++;
	}
	serve_reads(u);
}

/* Makes the ring buffer bigger when SDCMD_SETPARAMS asks for it (it is
   never made smaller, as the Pi may already have credit for it) */
static BOOL grow_ring(struct SerUnit *u, ULONG size)
{
	UBYTE *ring;
	ULONG i, tail;

	if (size > MAX_RBUFLEN)
		size = MAX_RBUFLEN;
	if (size <= u->ring_size)
		return TRUE;
	if (!(ring = AllocMem(size, MEMF_PUBLIC)))
		return FALSE;
	tail = ring_tail(u);
	for (i = 0; i < u->ring_count; i++)
	{
		ring[i] = u->ring[tail++];
		if (tail == u->ring_size)
			tail = 0;
	}
	FreeMem(u->ring, u->ring_size);
	u->ring = ring;
	u->ring_size = size;
	u->ring_head = u->ring_count;	/* < size, as size grew */
	return TRUE;
}

/* Sends the next packet, if a314.device isn't busy with the last one */
static void send_next(struct SerUnit *u)
{
	char *p = (char *)u->out_buf;
	struct IOExtSer *ior;

	if (u->write_pending)
		return;

	if (u->state == U_OPENING && !u->open_sent)
	{
		*p++ = 'C';
		p = put_str(p, "OPEN ");
		p = put_str(p, u->port);
		*p++ = ' ';
		p = put_params(p, &u->params);
		*p++ = ' ';
		p = put_num(p, u->ring_size);
		u->credit = u->ring_size;
		u->open_sent = TRUE;
		send_line(u, p);
		return;
	}
	if (u->state != U_OPEN)
		return;

	if (u->need_clear)
	{
		u->need_clear = FALSE;
		send_line(u, put_str(p, "CCLEAR"));
		return;
	}

	/* More room for data from the Pi */
	if (ring_free(u) >= u->ring_size / 4)
	{
		ULONG n = ring_free(u);
		u->credit += n;
		send_line(u, put_num(put_str(p, "CCREDIT "), n));
		return;
	}

	if (u->set_waiting)
		return;

	if (u->need_default_set)
	{
		u->need_default_set = FALSE;
		u->set_waiting = u->internal_set = TRUE;
		send_line(u, put_params(put_str(p, "CSET "), &u->params));
		return;
	}

	ior = (struct IOExtSer *)u->out.lh_Head;
	if (!ior->IOSer.io_Message.mn_Node.ln_Succ)
		return;

	switch (ior->IOSer.io_Command)
	{
	case CMD_WRITE:
	{
		/* io_Offset holds the length (io_Length may be -1) */
		UBYTE *src = (UBYTE *)ior->IOSer.io_Data + ior->IOSer.io_Actual;
		ULONG n = ior->IOSer.io_Offset - ior->IOSer.io_Actual;
		BOOL done;
		ULONG i;

		if (u->stopped || u->in_flight >= TX_WINDOW)
			return;
		if (n > MAX_DATA)
			n = MAX_DATA;
		if (n > TX_WINDOW - u->in_flight)
			n = TX_WINDOW - u->in_flight;
		*p++ = 'D';
		done = ior->IOSer.io_Actual + n == ior->IOSer.io_Offset;
		for (i = 0; i < n; )
		{
			UBYTE c = src[i++];
			*p++ = c;
			if (ior->io_SerFlags & SERF_EOFMODE)
			{
				/* In EOF mode a write ends after a terminator */
				UBYTE *term = (UBYTE *)&u->params.term_array;
				int j;
				for (j = 0; j < 8; j++)
					if (c == term[j])
						break;
				if (j < 8)
				{
					done = TRUE;
					break;
				}
			}
		}
		ior->IOSer.io_Actual += i;
		u->in_flight += i;
		send_line(u, p);
		if (done)
		{
			Remove(&ior->IOSer.io_Message.mn_Node);
			reply((struct IORequest *)ior, 0);
		}
		return;
	}

	case SDCMD_SETPARAMS:
	{
		struct SerParams sp;
		params_from_ior(&sp, ior);
		u->set_waiting = TRUE;
		u->internal_set = FALSE;
		send_line(u, put_params(put_str(p, "CSET "), &sp));
		return;
	}

	case SDCMD_BREAK:
		Remove(&ior->IOSer.io_Message.mn_Node);
		u->status |= IO_STATF_WROTEBREAK;
		send_line(u, put_num(put_str(p, "CBREAK "), u->params.brk_time));
		reply((struct IORequest *)ior, 0);
		return;
	}
}

/* A reply line from the Pi */
static void handle_line(struct SerUnit *u, const UBYTE *p, const UBYTE *end)
{
	const UBYTE *rest;

	if (starts_with(p, end, "SENT", &rest))
	{
		ULONG n = get_num(&rest, end);
		u->in_flight = n > u->in_flight ? 0 : u->in_flight - n;
	}
	else if (starts_with(p, end, "LINES", &rest))
	{
		UWORD st = u->status & ~(ST_CTS | ST_DSR | ST_CD);
		if (!get_num(&rest, end))
			st |= ST_CTS;
		if (!get_num(&rest, end))
			st |= ST_DSR;
		if (!get_num(&rest, end))
			st |= ST_CD;
		u->status = st;
	}
	else if (starts_with(p, end, "GONE", &rest))
		u->status |= ST_CTS | ST_DSR | ST_CD;
	else if (starts_with(p, end, "OPENED", &rest) && u->state == U_OPENING)
	{
		u->state = U_OPEN;
		ReplyMsg(&u->ctl->msg);
		u->ctl = NULL;
	}
	else if (starts_with(p, end, "OK", &rest) || starts_with(p, end, "ERR", &rest))
	{
		BYTE error = *p == 'E' ? (BYTE)get_num(&rest, end) : 0;

		if (u->state == U_OPENING && *p == 'E')
		{
			/* OPEN failed. Close the connection, then tell Open().
			   32 is "no such port". */
			u->ctl->error = error && error != 32 ? error : IOERR_OPENFAIL;
			u->state = U_CLOSING;
			reset_socket(u);
		}
		else if (u->set_waiting)
		{
			u->set_waiting = FALSE;
			if (!u->internal_set)
			{
				struct IOExtSer *ior = (struct IOExtSer *)RemHead(&u->out);
				if (ior)
				{
					/* After a CMD_RESET, the defaults stay */
					if (!error && !u->need_default_set)
					{
						params_from_ior(&u->params, ior);
						if (!grow_ring(u, u->params.rbuf_len))
							error = SerErr_BufErr;
					}
					reply((struct IORequest *)ior, error);
				}
			}
		}
	}
}

static void handle_packet(struct SerUnit *u)
{
	UBYTE *p = u->in_buf;
	UBYTE *end = p + u->read_ior.a314_Length;

	if (p == end)
		return;
	if (*p == 'D' && u->state == U_OPEN)
		received(u, p + 1, end - p - 1);
	else if (*p == 'C')
		handle_line(u, p + 1, end);
}

/* A unit that is closing is done when a314.device has given back all its
   requests. */
static void check_closed(struct SerUnit *u)
{
	if (u->state != U_CLOSING || u->read_pending || u->write_pending || u->reset_pending)
		return;
	if (u->ring)
		FreeMem(u->ring, u->ring_size);
	u->ring = NULL;
	u->ring_count = 0;
	u->state = U_CLOSED;
	if (u->ctl)
	{
		ReplyMsg(&u->ctl->msg);
		u->ctl = NULL;
	}
}

static void handle_a314_reply(struct A314_IORequest *ior)
{
	struct SerUnit *u = (struct SerUnit *)ior->a314_Request.io_Message.mn_Node.ln_Name;
	BYTE err = ior->a314_Request.io_Error;

	switch (ior->a314_Request.io_Command)
	{
	case A314_CONNECT:
		u->read_pending = FALSE;
		if (u->state != U_CONNECTING)
			break;
		if (err == A314_CONNECT_OK)
		{
			u->state = U_OPENING;
			start_read(u);
		}
		else
		{
			/* No piserial service, or the socket was reset */
			u->ctl->error = IOERR_OPENFAIL;
			u->state = U_CLOSING;
		}
		break;

	case A314_READ:
		u->read_pending = FALSE;
		if (err == A314_READ_OK)
		{
			handle_packet(u);
			start_read(u);
		}
		else if (u->state == U_OPENING)
		{
			u->ctl->error = IOERR_OPENFAIL;
			u->state = U_CLOSING;
			reset_socket(u);
		}
		else
			lose_connection(u);
		break;

	case A314_WRITE:
		u->write_pending = FALSE;
		if (err != A314_WRITE_OK)
		{
			if (u->state == U_OPENING)
			{
				u->ctl->error = IOERR_OPENFAIL;
				u->state = U_CLOSING;
				reset_socket(u);
			}
			else
				lose_connection(u);
		}
		break;

	case A314_RESET:
		u->reset_pending = FALSE;
		break;
	}

	send_next(u);
	check_closed(u);
}

/* A request from begin_io() */
static void handle_request(struct IOExtSer *ior)
{
	struct SerUnit *u = (struct SerUnit *)ior->IOSer.io_Unit;
	struct SerParams sp;

	if (u->state != U_OPEN && ior->IOSer.io_Command != CMD_STOP && ior->IOSer.io_Command != CMD_START)
	{
		/* The connection is gone, but data already received is given */
		if (ior->IOSer.io_Command == CMD_READ && u->state == U_DEAD && u->ring_count)
		{
			AddTail(&u->reads, &ior->IOSer.io_Message.mn_Node);
			serve_reads(u);
			if (in_list(&u->reads, &ior->IOSer.io_Message.mn_Node))
			{
				Remove(&ior->IOSer.io_Message.mn_Node);
				reply((struct IORequest *)ior, ERR_LINE);
			}
		}
		else
			reply((struct IORequest *)ior, ERR_LINE);
		return;
	}

	switch (ior->IOSer.io_Command)
	{
	case CMD_READ:
		if (ior->IOSer.io_Length == 0)
			reply((struct IORequest *)ior, 0);
		else
		{
			AddTail(&u->reads, &ior->IOSer.io_Message.mn_Node);
			serve_reads(u);
		}
		break;

	case CMD_WRITE:
		ior->IOSer.io_Offset = ior->IOSer.io_Length == (ULONG)-1 ?
			str_len((char *)ior->IOSer.io_Data) : ior->IOSer.io_Length;
		if (ior->IOSer.io_Offset == 0)
			reply((struct IORequest *)ior, 0);
		else
			AddTail(&u->out, &ior->IOSer.io_Message.mn_Node);
		break;

	case SDCMD_SETPARAMS:
		params_from_ior(&sp, ior);
		if (!params_valid(&sp))
			reply((struct IORequest *)ior, SerErr_InvParam);
		else
			AddTail(&u->out, &ior->IOSer.io_Message.mn_Node);
		break;

	case SDCMD_BREAK:
		AddTail(&u->out, &ior->IOSer.io_Message.mn_Node);
		break;

	case CMD_CLEAR:
		u->ring_count = 0;
		u->need_clear = TRUE;
		reply((struct IORequest *)ior, 0);
		break;

	case CMD_FLUSH:
		flush_requests(u);
		reply((struct IORequest *)ior, 0);
		break;

	case CMD_RESET:
		flush_requests(u);
		u->ring_count = 0;
		u->need_clear = TRUE;
		u->stopped = FALSE;
		u->status &= ~(IO_STATF_WROTEBREAK | IO_STATF_READBREAK | IO_STATF_OVERRUN);
		u->params = u->defaults;
		params_to_ior(&u->params, ior);
		u->need_default_set = TRUE;
		reply((struct IORequest *)ior, 0);
		break;

	case CMD_STOP:
		u->stopped = TRUE;
		reply((struct IORequest *)ior, 0);
		break;

	case CMD_START:
		u->stopped = FALSE;
		reply((struct IORequest *)ior, 0);
		break;

	default:
		reply((struct IORequest *)ior, IOERR_NOCMD);
		break;
	}
	send_next(u);
}

/* Reads the port chosen for the unit (in Prefs/A314/Serial) from the
   environment variable A314/SerialUnit<n>: from ENV:, or if it isn't
   there (on Workbench 1.3, ENV: starts empty) from where it is saved.
   Outside Forbid: this uses DOS. */
static void read_port_name(struct SerUnit *u)
{
	static const char *const dirs[] = { "ENV:", "ENVARC:", "SYS:Prefs/Env-Archive/" };
	char name[48];
	BPTR fh = 0;
	LONG n = 0, i;

	for (i = 0; i < 3 && !fh; i++)
	{
		char *p = put_str(name, dirs[i]);
		p = put_str(p, "A314/SerialUnit");
		*p++ = '0' + u->number;
		*p = '\0';
		fh = Open(name, MODE_OLDFILE);
	}
	if (fh)
	{
		n = Read(fh, u->port, sizeof(u->port) - 1);
		Close(fh);
	}
	if (n < 0)
		n = 0;
	/* One word: stop at white space */
	for (i = 0; i < n; i++)
		if (u->port[i] <= ' ')
			break;
	u->port[i] = '\0';
	if (i == 0)
		put_str(u->port, "auto")[0] = '\0';
}

static void start_timer(struct SerialDevice *dev);

static void start_open(struct SerUnit *u, struct CtlMsg *cm)
{
	read_port_name(u);
	default_params(&u->params);
	u->params.ser_flags |= cm->ior->io_SerFlags & (SERF_SHARED | SERF_7WIRE);
	if (cm->ior->io_SerFlags & SERF_7WIRE)
		u->params.ser_flags |= SERF_XDISABLED;
	u->defaults = u->params;

	Forbid();
	u->ctl = cm;
	u->ring_size = u->params.rbuf_len;
	u->ring = the_device->timer_open ? AllocMem(u->ring_size, MEMF_PUBLIC) : NULL;
	if (!u->ring)
	{
		/* (Without timer.device there could be no time-out) */
		cm->error = the_device->timer_open ? SerErr_BufErr : IOERR_OPENFAIL;
		u->state = U_CLOSED;
		u->ctl = NULL;
		ReplyMsg(&cm->msg);
		Permit();
		return;
	}
	u->ring_head = u->ring_count = u->credit = u->in_flight = 0;
	u->status = ST_CTS | ST_DSR | ST_CD;
	u->open_sent = u->set_waiting = u->need_clear = u->need_default_set = u->stopped = FALSE;
	u->generation++;
	u->timeout = OPEN_TIMEOUT;
	u->state = U_CONNECTING;
	a314_cmd(u, &u->read_ior, A314_CONNECT, (UBYTE *)service_name, sizeof(service_name) - 1);
	u->read_pending = TRUE;
	start_timer(the_device);
	Permit();
}

static void start_close(struct SerUnit *u, struct CtlMsg *cm)
{
	Forbid();
	u->ctl = cm;
	u->set_waiting = FALSE;
	fail_list(&u->reads, IOERR_ABORTED);
	fail_list(&u->out, IOERR_ABORTED);
	u->state = U_CLOSING;
	reset_socket(u);
	check_closed(u);
	Permit();
}

static void start_timer(struct SerialDevice *dev)
{
	if (dev->timer_open && !dev->timer_pending)
	{
		dev->timer_req.tr_time.tv_secs = 1;
		dev->timer_req.tr_time.tv_micro = 0;
		SendIO((struct IORequest *)&dev->timer_req);
		dev->timer_pending = TRUE;
	}
}

/* Once a second while a unit is being opened */
static void tick(struct SerialDevice *dev)
{
	int i;
	BOOL again = FALSE;

	for (i = 0; i < UNIT_COUNT; i++)
	{
		struct SerUnit *u = &dev->units[i];
		if (u->state != U_CONNECTING && u->state != U_OPENING)
			continue;
		if (--u->timeout == 0)
		{
			u->ctl->error = IOERR_OPENFAIL;
			u->state = U_CLOSING;
			reset_socket(u);
			check_closed(u);
		}
		else
			again = TRUE;
	}
	if (again)
		start_timer(dev);
}

void worker_run()
{
	struct SerialDevice *dev = the_device;
	struct Process *me = (struct Process *)FindTask(NULL);
	struct Message *msg;

	/* No "insert volume" requesters if ENV: isn't there */
	me->pr_WindowPtr = (APTR)-1;

	dev->timer_req.tr_node.io_Message.mn_ReplyPort = NULL;
	{
		static struct MsgPort timer_port;
		init_port(&timer_port, SIGB_TIMER);
		timer_port.mp_SigTask = &me->pr_Task;
		dev->timer_req.tr_node.io_Message.mn_ReplyPort = &timer_port;
		dev->timer_req.tr_node.io_Message.mn_Length = sizeof(struct timerequest);
		dev->timer_req.tr_node.io_Command = TR_ADDREQUEST;
		dev->timer_open = OpenDevice(TIMERNAME, UNIT_VBLANK, (struct IORequest *)&dev->timer_req, 0) == 0;
	}

	for (;;)
	{
		ULONG sigs = Wait((1UL << SIGB_IO) | (1UL << SIGB_CTL) | (1UL << SIGB_A314) | (1UL << SIGB_TIMER));

		/* Opening reads the environment, so is done before Forbid() */
		if (sigs & (1UL << SIGB_CTL))
		{
			while ((msg = GetMsg(&dev->ctl_port)))
			{
				struct CtlMsg *cm = (struct CtlMsg *)msg;
				struct SerUnit *u = (struct SerUnit *)cm->ior->IOSer.io_Unit;
				if (cm->open)
					start_open(u, cm);
				else
					start_close(u, cm);
			}
		}

		Forbid();
		if (sigs & (1UL << SIGB_A314))
			while ((msg = GetMsg(&dev->a314_port)))
				handle_a314_reply((struct A314_IORequest *)msg);
		if (sigs & (1UL << SIGB_IO))
			while ((msg = GetMsg(&dev->io_port)))
				handle_request((struct IOExtSer *)msg);
		if (sigs & (1UL << SIGB_TIMER))
		{
			if (GetMsg(dev->timer_req.tr_node.io_Message.mn_ReplyPort))
			{
				dev->timer_pending = FALSE;
				tick(dev);
			}
		}
		Permit();
	}
}

/* ---------------------------------------------------------------------- */
/* Device functions (called by the programs that use the device)          */

/* Passes an open or close to the worker and waits for it */
static LONG ask_worker(struct SerialDevice *dev, struct IOExtSer *ior, BOOL open)
{
	struct MsgPort port;
	struct CtlMsg cm;

	init_port(&port, SIGB_SINGLE);
	port.mp_SigTask = FindTask(NULL);
	cm.msg.mn_Node.ln_Type = NT_MESSAGE;
	cm.msg.mn_ReplyPort = &port;
	cm.msg.mn_Length = sizeof(cm);
	cm.open = open;
	cm.ior = ior;
	cm.error = 0;

	SetSignal(0, SIGF_SINGLE);
	PutMsg(&dev->ctl_port, &cm.msg);
	while (!GetMsg(&port))
		Wait(SIGF_SINGLE);
	return cm.error;
}

static void begin_io(__reg("a6") struct SerialDevice *dev, __reg("a1") struct IOExtSer *ior)
{
	struct SerUnit *u = (struct SerUnit *)ior->IOSer.io_Unit;

	ior->IOSer.io_Error = 0;

	switch (ior->IOSer.io_Command)
	{
	case SDCMD_QUERY:
		ior->IOSer.io_Actual = u->ring_count;
		ior->io_Status = u->status;
		u->status &= ~(IO_STATF_WROTEBREAK | IO_STATF_READBREAK);	/* reported once */
		break;

	case CMD_READ:
	case CMD_WRITE:
	case SDCMD_SETPARAMS:
	case SDCMD_BREAK:
	case CMD_CLEAR:
	case CMD_FLUSH:
	case CMD_RESET:
	case CMD_STOP:
	case CMD_START:
		ior->IOSer.io_Actual = 0;
		ior->IOSer.io_Flags &= ~IOF_QUICK;
		PutMsg(&dev->io_port, &ior->IOSer.io_Message);
		return;

	default:
		ior->IOSer.io_Error = IOERR_NOCMD;
		break;
	}

	if (!(ior->IOSer.io_Flags & IOF_QUICK))
		ReplyMsg(&ior->IOSer.io_Message);
}

static BOOL in_list(struct List *l, struct Node *n)
{
	struct Node *x;
	for (x = l->lh_Head; x->ln_Succ; x = x->ln_Succ)
		if (x == n)
			return TRUE;
	return FALSE;
}

static ULONG abort_io(__reg("a6") struct SerialDevice *dev, __reg("a1") struct IOExtSer *ior)
{
	struct SerUnit *u = (struct SerUnit *)ior->IOSer.io_Unit;
	struct Node *n = &ior->IOSer.io_Message.mn_Node;
	ULONG result = 0;

	Forbid();
	/* Only a request that is really waiting here (in the worker's port or
	   a unit's list), and not a SET the Pi is answering */
	if (u && u >= &dev->units[0] && u < &dev->units[UNIT_COUNT] &&
		(in_list(&dev->io_port.mp_MsgList, n) || in_list(&u->reads, n) || in_list(&u->out, n)) &&
		!((struct IOExtSer *)u->out.lh_Head == ior && head_in_progress(u)))
	{
		Remove(&ior->IOSer.io_Message.mn_Node);
		reply((struct IORequest *)ior, IOERR_ABORTED);
	}
	else
		result = (ULONG)-1;
	Permit();
	return result;
}

static void open(__reg("a6") struct SerialDevice *dev, __reg("a1") struct IOExtSer *ior,
	__reg("d0") ULONG unit_number, __reg("d1") ULONG flags)
{
	struct SerUnit *u;
	LONG error;

	ior->IOSer.io_Error = IOERR_OPENFAIL;
	ior->IOSer.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
	if (unit_number >= UNIT_COUNT)
		return;
	u = &dev->units[unit_number];

	/* Exec calls this under Forbid(), so the state is checked and set
	   without being interrupted. */
	if (u->state == U_OPEN && u->shared && (ior->io_SerFlags & SERF_SHARED))
	{
		u->open_count++;
		dev->lib.lib_OpenCnt++;
		ior->IOSer.io_Unit = (struct Unit *)u;
		params_to_ior(&u->params, ior);
		ior->IOSer.io_Error = 0;
		return;
	}
	if (u->state != U_CLOSED)
	{
		ior->IOSer.io_Error = SerErr_DevBusy;
		return;
	}

	u->state = U_STARTING;
	u->open_count = 1;	/* before the worker can make the unit U_OPEN */
	u->shared = (ior->io_SerFlags & SERF_SHARED) != 0;
	ior->IOSer.io_Unit = (struct Unit *)u;
	dev->lib.lib_OpenCnt++;		/* no expunge while waiting */
	error = ask_worker(dev, ior, TRUE);
	if (error)
	{
		dev->lib.lib_OpenCnt--;
		u->open_count = 0;
		ior->IOSer.io_Unit = NULL;
		ior->IOSer.io_Error = error;
		return;
	}
	dev->lib.lib_Flags &= ~LIBF_DELEXP;
	params_to_ior(&u->params, ior);
	ior->IOSer.io_Error = 0;
}

static BPTR close(__reg("a6") struct SerialDevice *dev, __reg("a1") struct IOExtSer *ior)
{
	struct SerUnit *u = (struct SerUnit *)ior->IOSer.io_Unit;

	if (u && --u->open_count == 0)
		ask_worker(dev, ior, FALSE);
	ior->IOSer.io_Unit = (struct Unit *)-1;
	ior->IOSer.io_Device = (struct Device *)-1;
	dev->lib.lib_OpenCnt--;
	return 0;
}

/* The worker process stays, so the device isn't unloaded (as with the
   other A314 devices). */
static BPTR expunge(__reg("a6") struct SerialDevice *dev)
{
	dev->lib.lib_Flags |= LIBF_DELEXP;
	return 0;
}

static ULONG null_func(void)
{
	return 0;
}

static struct Library *init_device(__reg("a6") struct ExecBase *sys_base, __reg("a0") BPTR seg_list,
	__reg("d0") struct SerialDevice *dev)
{
	struct MsgPort *worker_port;
	int i;

	dev->seg_list = seg_list;
	dev->lib.lib_Node.ln_Type = NT_DEVICE;
	dev->lib.lib_Node.ln_Name = (char *)device_name;
	dev->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
	dev->lib.lib_Version = 1;
	dev->lib.lib_Revision = 0;
	dev->lib.lib_IdString = (APTR)id_string;
	the_device = dev;

	init_port(&dev->io_port, SIGB_IO);
	init_port(&dev->ctl_port, SIGB_CTL);
	init_port(&dev->a314_port, SIGB_A314);

	dev->a314_ior.a314_Request.io_Message.mn_ReplyPort = &dev->a314_port;
	dev->a314_ior.a314_Request.io_Message.mn_Length = sizeof(struct A314_IORequest);
	if (OpenDevice(A314_NAME, 0, (struct IORequest *)&dev->a314_ior, 0))
		goto fail;
	dev->a314_base = (struct Library *)dev->a314_ior.a314_Request.io_Device;

	DOSBase = (struct DosLibrary *)OpenLibrary(DOSNAME, 0);
	IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 0);
	if (!DOSBase)
		goto fail_a314;

	for (i = 0; i < UNIT_COUNT; i++)
	{
		struct SerUnit *u = &dev->units[i];
		u->number = i;
		new_list(&u->reads);
		new_list(&u->out);
		u->read_ior = dev->a314_ior;
		/* ln_Name finds the unit from a314.device's reply */
		u->read_ior.a314_Request.io_Message.mn_Node.ln_Name = (char *)u;
		u->write_ior = u->read_ior;
		u->reset_ior = u->read_ior;
	}

	Forbid();
	worker_port = CreateProc((char *)device_name, WORKER_PRI, ((ULONG)worker_seglist) >> 2, WORKER_STACK);
	if (worker_port)
	{
		dev->worker = (struct Process *)((char *)worker_port - sizeof(struct Task));
		dev->io_port.mp_SigTask = &dev->worker->pr_Task;
		dev->ctl_port.mp_SigTask = &dev->worker->pr_Task;
		dev->a314_port.mp_SigTask = &dev->worker->pr_Task;
		/* Reserved here rather than with AllocSignal() in the worker,
		   which would clear a signal that is already waiting. */
		dev->worker->pr_Task.tc_SigAlloc |= (1UL << SIGB_IO) | (1UL << SIGB_CTL) |
			(1UL << SIGB_A314) | (1UL << SIGB_TIMER);
	}
	Permit();
	if (!worker_port)
		goto fail_libs;
	return &dev->lib;

fail_libs:
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	CloseLibrary((struct Library *)DOSBase);
fail_a314:
	CloseDevice((struct IORequest *)&dev->a314_ior);
fail:
	FreeMem((char *)dev - dev->lib.lib_NegSize, dev->lib.lib_NegSize + dev->lib.lib_PosSize);
	return NULL;
}

static const ULONG device_vectors[] =
{
	(ULONG)open,
	(ULONG)close,
	(ULONG)expunge,
	(ULONG)null_func,
	(ULONG)begin_io,
	(ULONG)abort_io,
	(ULONG)-1,
};

const ULONG auto_init_tables[] =
{
	sizeof(struct SerialDevice),
	(ULONG)device_vectors,
	0,
	(ULONG)init_device,
};
