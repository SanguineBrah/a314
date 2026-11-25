/*
 * The parts that the A314 datatypes share; see a314dt.h.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <datatypes/datatypes.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "a314dt.h"
#include "../a314device/proto_a314.h"

#define SysBase (*(struct ExecBase **)4)

extern struct DosLibrary *DOSBase;

static const char service_name[] = SERVICE_NAME;
/* ---------------------------------------------------------------------- */
/* Helpers (there is no C library)                                        */


ULONG str_len(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return p - s;
}

char *put_str(char *dst, const char *s)
{
	while (*s)
		*dst++ = *s++;
	return dst;
}

/* Without division, which on the 68000 would need the C library */
char *put_num(char *dst, ULONG n)
{
	const ULONG powers[] = { 1000000000, 100000000, 10000000, 1000000,
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

/* Reads a number and moves past it and the spaces after it */
ULONG get_num(const char **pp)
{
	const char *p = *pp;
	ULONG n = 0;
	int digits = 0;
	while (*p >= '0' && *p <= '9')
	{
		/* At most 9 digits; more makes a number too large to be valid */
		if (++digits > 9)
			n = 0x7fffffff;
		else
			n = (n << 3) + (n << 1) + (*p - '0');
		p++;
	}
	while (*p == ' ')
		p++;
	*pp = p;
	return n;
}

BOOL starts_with(const char *p, const char *word, const char **rest)
{
	while (*word)
		if (*p++ != *word++)
			return FALSE;
	if (*p != ' ' && *p != '\0')
		return FALSE;
	while (*p == ' ')
		p++;
	*rest = p;
	return TRUE;
}

/* ---------------------------------------------------------------------- */
/* A connection to the picture service, used from the caller's task       */



void a314_cmd(struct Conn *c, struct A314_IORequest *ior, UWORD cmd, UBYTE *buf, WORD len)
{
	ior->a314_Request.io_Command = cmd;
	ior->a314_Request.io_Error = 0;
	ior->a314_Socket = c->socket;
	ior->a314_Buffer = (STRPTR)buf;
	ior->a314_Length = len;
	SendIO((struct IORequest *)ior);
}

/* Waits for ior, for up to secs seconds; FALSE on a time-out (ior is
   then still pending) */
BOOL wait_io(struct Conn *c, struct IORequest *ior, ULONG secs)
{
	c->timer->tr_node.io_Command = TR_ADDREQUEST;
	c->timer->tr_time.tv_secs = secs;
	c->timer->tr_time.tv_micro = 0;
	SendIO((struct IORequest *)c->timer);
	for (;;)
	{
		if (CheckIO(ior))
		{
			WaitIO(ior);
			AbortIO((struct IORequest *)c->timer);
			WaitIO((struct IORequest *)c->timer);
			return TRUE;
		}
		if (CheckIO((struct IORequest *)c->timer))
		{
			WaitIO((struct IORequest *)c->timer);
			return FALSE;
		}
		Wait(1UL << c->port->mp_SigBit);
	}
}

void start_read(struct Conn *c)
{
	a314_cmd(c, c->read, A314_READ, c->in, sizeof(c->in) - 1);
	c->read_pending = TRUE;
}

BOOL conn_open(struct IORequest *a314, struct Conn *c)
{
	struct timeval tv;

	c->port = CreateMsgPort();
	if (!c->port)
		return FALSE;
	c->read = (struct A314_IORequest *)CreateIORequest(c->port, sizeof(struct A314_IORequest));
	c->write = (struct A314_IORequest *)CreateIORequest(c->port, sizeof(struct A314_IORequest));
	c->reset = (struct A314_IORequest *)CreateIORequest(c->port, sizeof(struct A314_IORequest));
	c->timer = (struct timerequest *)CreateIORequest(c->port, sizeof(struct timerequest));
	if (!c->read || !c->write || !c->reset || !c->timer)
		return FALSE;
	if (OpenDevice(TIMERNAME, UNIT_VBLANK, (struct IORequest *)c->timer, 0))
		return FALSE;
	c->timer_open = TRUE;

	/* The same open a314.device, through requests of our own */
	c->read->a314_Request.io_Device = a314->io_Device;
	c->read->a314_Request.io_Unit = a314->io_Unit;
	c->write->a314_Request.io_Device = c->reset->a314_Request.io_Device = c->read->a314_Request.io_Device;
	c->write->a314_Request.io_Unit = c->reset->a314_Request.io_Unit = c->read->a314_Request.io_Unit;

	/* A socket number of our own: the request's address and the time */
	c->timer->tr_node.io_Command = TR_GETSYSTIME;
	DoIO((struct IORequest *)c->timer);
	tv = c->timer->tr_time;
	c->socket = (ULONG)c->read ^ (tv.tv_micro << 8) ^ tv.tv_secs;

	a314_cmd(c, c->read, A314_CONNECT, (UBYTE *)service_name, sizeof(service_name) - 1);
	if (!wait_io(c, (struct IORequest *)c->read, WAIT_SECONDS))
	{
		a314_cmd(c, c->reset, A314_RESET, NULL, 0);
		WaitIO((struct IORequest *)c->reset);
		WaitIO((struct IORequest *)c->read);
		return FALSE;
	}
	if (c->read->a314_Request.io_Error != A314_CONNECT_OK)
		return FALSE;
	c->connected = TRUE;
	start_read(c);
	return TRUE;
}

/* Returns FALSE if the Pi didn't answer something in time */
BOOL conn_close(struct Conn *c)
{
	if (c->connected)
	{
		a314_cmd(c, c->reset, A314_RESET, NULL, 0);
		WaitIO((struct IORequest *)c->reset);
		if (c->read_pending)
			WaitIO((struct IORequest *)c->read);
		if (c->write_pending)
			WaitIO((struct IORequest *)c->write);
	}
	if (c->timer_open)
		CloseDevice((struct IORequest *)c->timer);
	if (c->timer)
		DeleteIORequest((struct IORequest *)c->timer);
	if (c->reset)
		DeleteIORequest((struct IORequest *)c->reset);
	if (c->write)
		DeleteIORequest((struct IORequest *)c->write);
	if (c->read)
		DeleteIORequest((struct IORequest *)c->read);
	if (c->port)
		DeleteMsgPort(c->port);
	return !c->timed_out;
}

BOOL send_line(struct Conn *c, char *end)
{
	a314_cmd(c, c->write, A314_WRITE, c->out, end - (char *)c->out);
	if (!wait_io(c, (struct IORequest *)c->write, WAIT_SECONDS))
	{
		c->write_pending = TRUE;	/* conn_close() waits for it */
		c->timed_out = TRUE;
		return FALSE;
	}
	return c->write->a314_Request.io_Error == A314_WRITE_OK;
}

/* The next line from the Pi, in c->line; NULL on an error or time-out */
const char *get_line(struct Conn *c, ULONG secs)
{
	ULONG n;
	if (!c->read_pending)
		return NULL;
	if (!wait_io(c, (struct IORequest *)c->read, secs))
	{
		c->timed_out = TRUE;
		return NULL;
	}
	c->read_pending = FALSE;
	if (c->read->a314_Request.io_Error != A314_READ_OK)
		return NULL;
	n = c->read->a314_Length;
	CopyMem(c->in, c->line, n);
	c->line[n] = '\0';
	start_read(c);
	return c->line;
}

LONG err_code(const char *p)
{
	LONG err = get_num(&p);
	return err ? err : DTERROR_INVALID_DATA;
}

LONG dt_start(struct DtJob **jobp, struct IORequest *a314, BPTR fh)
{
	struct DtJob *job;
	struct Conn *c;
	ULONG size, sent;
	const char *p;
	char *q;

	/* Not on the caller's stack, which may be small */
	*jobp = job = AllocVec(sizeof(struct DtJob), MEMF_PUBLIC | MEMF_CLEAR);
	if (!job)
		return ERROR_NO_FREE_STORE;
	job->a314buf = INVALID_A314_ADDRESS;
	c = &job->conn;

	Seek(fh, 0, OFFSET_END);
	size = Seek(fh, 0, OFFSET_BEGINNING);
	if ((LONG)size <= 0)
		return DTERROR_NOT_ENOUGH_DATA;
	if (size > MAX_FILE)
		return ERROR_NO_FREE_STORE;

	job->tmp = AllocVec(BUF_SIZE, MEMF_PUBLIC);
	job->a314buf = AllocMemA314(BUF_SIZE);
	if (!job->tmp || job->a314buf == INVALID_A314_ADDRESS)
		return ERROR_NO_FREE_STORE;
	if (!conn_open(a314, c))
		return DTERROR_NOT_AVAILABLE;	/* no Pi, or no datatypes service */

	q = put_num(put_str((char *)c->out, "BUF "), job->a314buf);
	*q++ = ' ';
	job->buf_given = TRUE;
	if (!send_line(c, put_num(q, BUF_SIZE)) ||
		!send_line(c, put_num(put_str((char *)c->out, "FILE "), size)))
		return DTERROR_INVALID_DATA;
	for (sent = 0; sent < size; )
	{
		LONG n = Read(fh, job->tmp, BUF_SIZE);
		if (n <= 0)
			return IoErr() ? IoErr() : DTERROR_NOT_ENOUGH_DATA;
		WriteMemA314(job->a314buf, job->tmp, n);
		if (!send_line(c, put_num(put_str((char *)c->out, "DATA "), n)) ||
			!(p = get_line(c, WAIT_SECONDS)))
			return DTERROR_INVALID_DATA;
		if (starts_with(p, "ERR", &p))
			return err_code(p);
		if (!starts_with(p, "TAKEN", &p))
			return DTERROR_INVALID_DATA;
		sent += n;
	}
	return 0;
}

void dt_end(struct DtJob *job)
{
	if (!job)
		return;
	/* After a time-out the Pi may still write into the buffer (a request
	   it hadn't answered yet), so then it is left allocated: 32 KB lost,
	   rather than other memory overwritten. */
	if (!conn_close(&job->conn) && job->buf_given)
		job->a314buf = INVALID_A314_ADDRESS;
	if (job->a314buf != INVALID_A314_ADDRESS)
		FreeMemA314(job->a314buf, BUF_SIZE);
	if (job->tmp)
		FreeVec(job->tmp);
	FreeVec(job);
}
