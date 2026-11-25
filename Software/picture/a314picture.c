/*
 * a314picture.datatype - shows pictures that the Amiga has no datatype
 * for (WebP, AVIF, HEIC, JPEG XL and TIFF) by having the Raspberry Pi
 * decode them, through the A314.
 *
 * It is a subclass of picture.datatype, so every program that uses
 * datatypes (MultiView and many others) can show these formats. The
 * descriptors in DEVS:DataTypes recognise the files and name this class.
 *
 * When an object is made, the file is sent to the picture service on the
 * Pi (picture.py) through a buffer in A314 memory; the Pi decodes it and
 * sends the pixels back the same way. With picture.datatype V43 or later
 * (Workbench 3.5, 3.9, 3.2) they are 24-bit, and picture.datatype fits
 * them to the screen; with an older one (3.0, 3.1) the Pi reduces them to
 * at most 256 colours.
 *
 * Pictures larger than the size in the environment variable
 * A314/PictureSize ("<width> <height>", default 1280 1024) are made
 * smaller on the Pi, to save memory on the Amiga.
 *
 * Datatypes need Kickstart/Workbench 3.0 or later.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <graphics/gfx.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/datatypes.h>

#include "../a314device/a314.h"
#include "../a314device/proto_a314.h"

#define SysBase (*(struct ExecBase **)4)

struct DosLibrary *DOSBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *DataTypesBase;
struct Library *A314Base;

#define CLASS_NAME	"a314picture.datatype"
#define SERVICE_NAME	"picture"

#define BUF_SIZE	32768		/* A314 buffer, for the file and pixels */
#define DEFAULT_W	1280
#define DEFAULT_H	1024
#define WAIT_SECONDS	10
#define DECODE_SECONDS	60		/* large pictures take a while */

struct ClassBase
{
	struct Library lib;
	BPTR seg_list;
	struct Library *super_base;	/* picture.datatype */
	struct IClass *cl;
	struct A314_IORequest a314_ior;	/* keeps a314.device open */
	BOOL a314_open;
};

const char library_name[] = CLASS_NAME;
const char id_string[] = CLASS_NAME " 1.0 (25.11.2026)";
static const char service_name[] = SERVICE_NAME;

/* ---------------------------------------------------------------------- */
/* Helpers (there is no C library)                                        */

/* Calls a BOOPSI dispatcher hook (a0 hook, a2 object, a1 message), as
   amiga.lib's DoSuperMethodA() and CoerceMethodA() do; the amiga.lib for
   Kickstart 1.3 doesn't have them. */
ULONG call_hook(__reg("a0") struct Hook *hook, __reg("a2") Object *obj, __reg("a1") Msg msg) =
	"\tmove.l\ta3,-(a7)\n\tmove.l\t8(a0),a3\n\tjsr\t(a3)\n\tmove.l\t(a7)+,a3";

static ULONG DoSuperMethodA(struct IClass *cl, Object *obj, Msg msg)
{
	return call_hook(&cl->cl_Super->cl_Dispatcher, obj, msg);
}

static ULONG CoerceMethodA(struct IClass *cl, Object *obj, Msg msg)
{
	return call_hook(&cl->cl_Dispatcher, obj, msg);
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

/* Reads a number and moves past it and the spaces after it */
static ULONG get_num(const char **pp)
{
	const char *p = *pp;
	ULONG n = 0;
	while (*p >= '0' && *p <= '9')
		n = (n << 3) + (n << 1) + (*p++ - '0');
	while (*p == ' ')
		p++;
	*pp = p;
	return n;
}

static BOOL starts_with(const char *p, const char *word, const char **rest)
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

struct Conn
{
	struct MsgPort *port;
	struct A314_IORequest *read, *write, *reset;
	struct timerequest *timer;
	ULONG socket;
	BOOL connected, read_pending, write_pending, timer_open;
	UBYTE in[256];
	char line[256];
	UBYTE out[64];
};

static void a314_cmd(struct Conn *c, struct A314_IORequest *ior, UWORD cmd, UBYTE *buf, WORD len)
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
static BOOL wait_io(struct Conn *c, struct IORequest *ior, ULONG secs)
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

static void start_read(struct Conn *c)
{
	a314_cmd(c, c->read, A314_READ, c->in, sizeof(c->in) - 1);
	c->read_pending = TRUE;
}

static BOOL conn_open(struct ClassBase *base, struct Conn *c)
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
	c->read->a314_Request.io_Device = base->a314_ior.a314_Request.io_Device;
	c->read->a314_Request.io_Unit = base->a314_ior.a314_Request.io_Unit;
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

static void conn_close(struct Conn *c)
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
}

static BOOL send_line(struct Conn *c, char *end)
{
	a314_cmd(c, c->write, A314_WRITE, c->out, end - (char *)c->out);
	if (!wait_io(c, (struct IORequest *)c->write, WAIT_SECONDS))
	{
		c->write_pending = TRUE;	/* conn_close() waits for it */
		return FALSE;
	}
	return c->write->a314_Request.io_Error == A314_WRITE_OK;
}

/* The next line from the Pi, in c->line; NULL on an error or time-out */
static const char *get_line(struct Conn *c, ULONG secs)
{
	ULONG n;
	if (!c->read_pending || !wait_io(c, (struct IORequest *)c->read, secs))
		return NULL;
	c->read_pending = FALSE;
	if (c->read->a314_Request.io_Error != A314_READ_OK)
		return NULL;
	n = c->read->a314_Length;
	CopyMem(c->in, c->line, n);
	c->line[n] = '\0';
	start_read(c);
	return c->line;
}

/* ---------------------------------------------------------------------- */
/* Loading                                                                */

static ULONG set_attrs(Object *obj, ULONG tag1, ...)
{
	return SetDTAttrsA(obj, NULL, NULL, (struct TagItem *)&tag1);
}

static ULONG get_attrs(Object *obj, ULONG tag1, ...)
{
	return GetDTAttrsA(obj, (struct TagItem *)&tag1);
}

/* The largest size wanted, from A314/PictureSize */
static void wanted_size(ULONG *w, ULONG *h)
{
	char buf[32];
	const char *p = buf;
	*w = DEFAULT_W;
	*h = DEFAULT_H;
	if (GetVar("A314/PictureSize", buf, sizeof(buf), 0) > 0)
	{
		ULONG a = get_num(&p);
		ULONG b = get_num(&p);
		if (a >= 16 && b >= 16)
		{
			*w = a;
			*h = b;
		}
	}
}

/* Planar bitmap rows from chunky pixels (for picture.datatype V40) */
static void chunky_to_planar(struct BitMap *bm, UBYTE *src, ULONG y, ULONG rows, ULONG width)
{
	ULONG r, x;
	int p;
	for (r = 0; r < rows; r++, y++)
	{
		UBYTE *line = src + r * width;
		for (x = 0; x < width; x += 8)
		{
			ULONG offset = y * bm->BytesPerRow + (x >> 3);
			UBYTE bits[8];
			int i;
			for (p = 0; p < 8; p++)
				bits[p] = 0;
			for (i = 0; i < 8 && x + i < width; i++)
			{
				UBYTE c = line[x + i];
				UBYTE mask = 0x80 >> i;
				for (p = 0; p < 8; p++)
					if (c & (1 << p))
						bits[p] |= mask;
			}
			for (p = 0; p < bm->Depth; p++)
				bm->Planes[p][offset] = bits[p];
		}
	}
}

/* Loads the picture into obj; returns 0, or an error for IoErr() */
static LONG load(struct ClassBase *base, struct IClass *cl, Object *obj)
{
	struct Conn conn, *c = &conn;
	struct BitMapHeader *bmh = NULL;
	struct BitMap *bm = NULL;
	ULONG source = 0, maxw, maxh, w, h, size, sent;
	BPTR fh = 0;
	ULONG a314buf = INVALID_A314_ADDRESS;
	UBYTE *tmp = NULL;
	BOOL v43 = base->super_base->lib_Version >= 43;
	const char *p;
	char *q;
	LONG err = DTERROR_INVALID_DATA;
	ULONG i;

	{
		UBYTE *b = (UBYTE *)c;
		for (i = 0; i < sizeof(*c); i++)
			b[i] = 0;
	}

	get_attrs(obj, DTA_SourceType, (ULONG)&source, DTA_Handle, (ULONG)&fh,
		PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE);
	if (source != DTST_FILE || !fh || !bmh)
		return ERROR_OBJECT_WRONG_TYPE;

	Seek(fh, 0, OFFSET_END);
	size = Seek(fh, 0, OFFSET_BEGINNING);
	if ((LONG)size <= 0)
		return DTERROR_NOT_ENOUGH_DATA;

	tmp = AllocVec(BUF_SIZE, MEMF_PUBLIC);
	a314buf = AllocMemA314(BUF_SIZE);
	if (!tmp || a314buf == INVALID_A314_ADDRESS)
	{
		err = ERROR_NO_FREE_STORE;
		goto out;
	}
	if (!conn_open(base, c))
	{
		err = DTERROR_NOT_AVAILABLE;	/* no Pi, or no picture service */
		goto out;
	}

	/* The file */
	q = put_num(put_str((char *)c->out, "BUF "), a314buf);
	*q++ = ' ';
	if (!send_line(c, put_num(q, BUF_SIZE)) ||
		!send_line(c, put_num(put_str((char *)c->out, "FILE "), size)))
		goto out;
	for (sent = 0; sent < size; )
	{
		LONG n = Read(fh, tmp, BUF_SIZE);
		if (n <= 0)
		{
			err = IoErr() ? IoErr() : DTERROR_NOT_ENOUGH_DATA;
			goto out;
		}
		WriteMemA314(a314buf, tmp, n);
		if (!send_line(c, put_num(put_str((char *)c->out, "DATA "), n)) ||
			!(p = get_line(c, WAIT_SECONDS)) || !starts_with(p, "TAKEN", &p))
			goto out;
		sent += n;
	}

	/* Decoding */
	wanted_size(&maxw, &maxh);
	q = put_num(put_str((char *)c->out, "DECODE "), maxw);
	*q++ = ' ';
	q = put_num(q, maxh);
	q = put_str(q, v43 ? " rgb" : " pal 256");
	if (!send_line(c, q) || !(p = get_line(c, DECODE_SECONDS)))
		goto out;
	if (starts_with(p, "ERR", &p))
	{
		err = get_num(&p);
		goto out;
	}
	if (!starts_with(p, "INFO", &p))
		goto out;
	w = get_num(&p);
	h = get_num(&p);
	if (!w || !h || w > 32767 || h > 32767)
		goto out;

	bmh->bmh_Left = bmh->bmh_Top = 0;
	bmh->bmh_Width = bmh->bmh_PageWidth = w;
	bmh->bmh_Height = bmh->bmh_PageHeight = h;
	bmh->bmh_Masking = mskNone;
	bmh->bmh_Compression = cmpNone;
	bmh->bmh_XAspect = bmh->bmh_YAspect = 1;

	if (v43)
	{
		set_attrs(obj, PDTA_SourceMode, PMODE_V43, TAG_DONE);
		bmh->bmh_Depth = 24;
	}
	else
	{
		bmh->bmh_Depth = 8;
		bm = AllocBitMap(w, h, 8, BMF_CLEAR, NULL);
		if (!bm)
		{
			err = ERROR_NO_FREE_STORE;
			goto out;
		}
	}

	/* The palette (V40) and the rows */
	for (;;)
	{
		if (!send_line(c, put_str((char *)c->out, "NEXT")) || !(p = get_line(c, WAIT_SECONDS)))
			goto out;
		if (starts_with(p, "DONE", &p))
			break;
		if (starts_with(p, "PAL", &p) && !v43)
		{
			ULONG n = get_num(&p);
			struct ColorRegister *cr = NULL;
			ULONG *cregs = NULL;
			if (n > 256)
				goto out;
			ReadMemA314(tmp, a314buf, n * 3);
			set_attrs(obj, PDTA_NumColors, n, TAG_DONE);
			get_attrs(obj, PDTA_ColorRegisters, (ULONG)&cr, PDTA_CRegs, (ULONG)&cregs, TAG_DONE);
			if (!cr || !cregs)
				goto out;
			for (i = 0; i < n; i++)
			{
				cr[i].red = tmp[3 * i];
				cr[i].green = tmp[3 * i + 1];
				cr[i].blue = tmp[3 * i + 2];
				cregs[3 * i] = tmp[3 * i] * 0x01010101;
				cregs[3 * i + 1] = tmp[3 * i + 1] * 0x01010101;
				cregs[3 * i + 2] = tmp[3 * i + 2] * 0x01010101;
			}
		}
		else if (starts_with(p, "ROWS", &p))
		{
			ULONG y = get_num(&p);
			ULONG n = get_num(&p);
			ULONG row = v43 ? w * 3 : w;
			if (y + n > h || n * row > BUF_SIZE)
				goto out;
			ReadMemA314(tmp, a314buf, n * row);
			if (v43)
			{
				struct pdtBlitPixelArray m;
				m.MethodID = PDTM_WRITEPIXELARRAY;
				m.pbpa_PixelData = tmp;
				m.pbpa_PixelFormat = PBPAFMT_RGB;
				m.pbpa_PixelArrayMod = row;
				m.pbpa_Left = 0;
				m.pbpa_Top = y;
				m.pbpa_Width = w;
				m.pbpa_Height = n;
				DoSuperMethodA(cl, obj, (Msg)&m);
			}
			else
				chunky_to_planar(bm, tmp, y, n, w);
		}
		else
			goto out;
	}

	if (!v43)
	{
		set_attrs(obj, PDTA_BitMap, (ULONG)bm, PDTA_ModeID, 0, TAG_DONE);
		bm = NULL;	/* the object has it now */
	}
	set_attrs(obj, DTA_NominalHoriz, w, DTA_NominalVert, h, TAG_DONE);
	err = 0;

out:
	if (bm)
		FreeBitMap(bm);
	conn_close(c);
	if (a314buf != INVALID_A314_ADDRESS)
		FreeMemA314(a314buf, BUF_SIZE);
	if (tmp)
		FreeVec(tmp);
	return err;
}

/* ---------------------------------------------------------------------- */
/* The class                                                              */

static ULONG dispatcher(__reg("a0") struct IClass *cl, __reg("a2") Object *o, __reg("a1") Msg msg)
{
	struct ClassBase *base = (struct ClassBase *)cl->cl_UserData;

	if (msg->MethodID == OM_NEW)
	{
		Object *obj = (Object *)DoSuperMethodA(cl, o, msg);
		if (obj)
		{
			LONG err = load(base, cl, obj);
			if (err)
			{
				ULONG dispose = OM_DISPOSE;
				CoerceMethodA(cl, obj, (Msg)&dispose);
				SetIoErr(err);
				obj = NULL;
			}
		}
		return (ULONG)obj;
	}
	return DoSuperMethodA(cl, o, msg);
}

/* ---------------------------------------------------------------------- */
/* Library functions                                                      */

static void close_all(struct ClassBase *base)
{
	if (base->a314_open)
		CloseDevice((struct IORequest *)&base->a314_ior);
	if (base->super_base)
		CloseLibrary(base->super_base);
	if (DataTypesBase)
		CloseLibrary(DataTypesBase);
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	if (DOSBase)
		CloseLibrary((struct Library *)DOSBase);
}

static struct Library *lib_open(__reg("a6") struct ClassBase *base)
{
	base->lib.lib_OpenCnt++;
	base->lib.lib_Flags &= ~LIBF_DELEXP;
	return &base->lib;
}

static BPTR lib_expunge(__reg("a6") struct ClassBase *base)
{
	BPTR seg = base->seg_list;

	if (base->lib.lib_OpenCnt)
	{
		base->lib.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	/* Not while there are objects of the class */
	RemoveClass(base->cl);
	if (!FreeClass(base->cl))
	{
		AddClass(base->cl);
		base->lib.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	close_all(base);
	Remove(&base->lib.lib_Node);
	FreeMem((char *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
	return seg;
}

static BPTR lib_close(__reg("a6") struct ClassBase *base)
{
	base->lib.lib_OpenCnt--;
	if (base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
		return lib_expunge(base);
	return 0;
}

static ULONG lib_null(void)
{
	return 0;
}

/* The datatype's class, for datatypes.library */
static struct IClass *obtain_engine(__reg("a6") struct ClassBase *base)
{
	return base->cl;
}

static struct Library *init_library(__reg("a6") struct ExecBase *sys_base, __reg("a0") BPTR seg_list,
	__reg("d0") struct ClassBase *base)
{
	base->seg_list = seg_list;
	base->lib.lib_Node.ln_Type = NT_LIBRARY;
	base->lib.lib_Node.ln_Name = (char *)library_name;
	base->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
	base->lib.lib_Version = 1;
	base->lib.lib_Revision = 0;
	base->lib.lib_IdString = (APTR)id_string;

	DOSBase = (struct DosLibrary *)OpenLibrary(DOSNAME, 39);
	IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
	GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
	DataTypesBase = OpenLibrary("datatypes.library", 39);
	base->super_base = OpenLibrary("datatypes/picture.datatype", 39);
	if (!DOSBase || !IntuitionBase || !GfxBase || !DataTypesBase || !base->super_base)
		goto fail;

	base->a314_ior.a314_Request.io_Message.mn_Length = sizeof(struct A314_IORequest);
	if (OpenDevice(A314_NAME, 0, (struct IORequest *)&base->a314_ior, 0))
		goto fail;
	base->a314_open = TRUE;
	A314Base = (struct Library *)base->a314_ior.a314_Request.io_Device;

	base->cl = MakeClass(CLASS_NAME, PICTUREDTCLASS, NULL, 0, 0);
	if (!base->cl)
		goto fail;
	base->cl->cl_Dispatcher.h_Entry = (HOOKFUNC)dispatcher;
	base->cl->cl_UserData = (ULONG)base;
	AddClass(base->cl);
	return &base->lib;

fail:
	close_all(base);
	FreeMem((char *)base - base->lib.lib_NegSize, base->lib.lib_NegSize + base->lib.lib_PosSize);
	return NULL;
}

static const ULONG library_vectors[] =
{
	(ULONG)lib_open,
	(ULONG)lib_close,
	(ULONG)lib_expunge,
	(ULONG)lib_null,
	(ULONG)obtain_engine,		/* -30 */
	(ULONG)-1,
};

const ULONG auto_init_tables[] =
{
	sizeof(struct ClassBase),
	(ULONG)library_vectors,
	0,
	(ULONG)init_library,
};
