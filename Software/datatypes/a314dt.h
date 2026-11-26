/*
 * The parts that the A314 datatypes (a314picture.datatype and
 * a314sound.datatype) share: the connection to the datatypes service on
 * the Pi, and sending it the file. See datatypes.py for the messages.
 */

#ifndef A314DT_H
#define A314DT_H

#include <exec/types.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>

#include "../a314device/a314.h"

#define SERVICE_NAME	"datatypes"
#define BUF_SIZE	32768		/* A314 buffer, for the file and the data */
#define MAX_FILE	(64 * 1024 * 1024)	/* as datatypes.py */
#define WAIT_SECONDS	20
#define DECODE_SECONDS	60		/* large files take a while */

struct Conn
{
	struct MsgPort *port;
	struct A314_IORequest *read, *write, *reset;
	struct timerequest *timer;
	ULONG socket;
	BOOL connected, read_pending, write_pending, timer_open;
	BOOL timed_out;			/* the Pi didn't answer something in time */
	UBYTE in[256];
	char line[256];
	UBYTE out[64];
};

/* One file on its way through the Pi */
struct DtJob
{
	struct Conn conn;
	ULONG a314buf;			/* A314 address of the buffer */
	UBYTE *tmp;			/* BUF_SIZE bytes of Amiga memory */
	BOOL buf_given;			/* the Pi knows the buffer's address */
};

/* Calls a BOOPSI dispatcher hook (a0 hook, a2 object, a1 message), as
   amiga.lib's DoSuperMethodA() and CoerceMethodA() do; the amiga.lib for
   Kickstart 1.3 doesn't have them. */
ULONG call_hook(__reg("a0") struct Hook *hook, __reg("a2") Object *obj, __reg("a1") Msg msg) =
	"\tmove.l\ta3,-(a7)\n\tmove.l\t8(a0),a3\n\tjsr\t(a3)\n\tmove.l\t(a7)+,a3";

static ULONG do_super_method(struct IClass *cl, Object *obj, Msg msg)
{
	return call_hook(&cl->cl_Super->cl_Dispatcher, obj, msg);
}

static ULONG coerce_method(struct IClass *cl, Object *obj, Msg msg)
{
	return call_hook(&cl->cl_Dispatcher, obj, msg);
}

ULONG str_len(const char *s);
char *put_str(char *dst, const char *s);
char *put_num(char *dst, ULONG n);
ULONG get_num(const char **pp);
BOOL starts_with(const char *p, const char *word, const char **rest);
BOOL send_line(struct Conn *c, char *end);
const char *get_line(struct Conn *c, ULONG secs);

/* Connects to the Pi and sends it the file; *job is set even on an error,
   and must be given to dt_end(). Returns 0, or an error for IoErr(). */
LONG dt_start(struct DtJob **job, struct IORequest *a314, BPTR fh);

/* Ends the connection and frees what dt_start() allocated */
void dt_end(struct DtJob *job);

/* The error in an ERR line (after "ERR"), never 0 */
LONG err_code(const char *p);

#endif
