/*
 * a314sound.datatype - plays sounds that the Amiga has no datatype for
 * (MP3, FLAC and Ogg Vorbis/Opus) by having the Raspberry Pi decode them,
 * through the A314.
 *
 * It is a subclass of sound.datatype, so every program that uses datatypes
 * (MultiView and many others) can play these formats. The descriptors in
 * DEVS:DataTypes recognise the files and name this class.
 *
 * When an object is made, the file is sent to the datatypes service on the
 * Pi (datatypes.py), which decodes it into 8-bit samples for Paula, at
 * most 22050 a second, and sends them back. With sound.datatype V44 or
 * later (Workbench 3.5, 3.9, 3.2) sounds are stereo; with an older one
 * (3.0, 3.1) mono. A sound longer than the free memory allows is cut
 * short; long music is better played with an MHI player through
 * mhia314.library.
 *
 * Datatypes need Kickstart/Workbench 3.0 or later.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <graphics/gfxbase.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/soundclass.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/datatypes.h>

#include "../a314device/a314.h"
#include "../a314device/proto_a314.h"
#include "a314dt.h"

#define SysBase (*(struct ExecBase **)4)

struct DosLibrary *DOSBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *DataTypesBase;
struct Library *A314Base;

#define CLASS_NAME	"a314sound.datatype"

#define RATE		22050		/* samples a second, at most */
#define MAX_CHANNEL	(16 * 1024 * 1024)	/* bytes for each channel, at most */
#define PAL_CLOCK	3546895
#define NTSC_CLOCK	3579545

struct ClassBase
{
	struct Library lib;
	BPTR seg_list;
	struct Library *super_base;	/* sound.datatype */
	struct IClass *cl;
	struct A314_IORequest a314_ior;	/* keeps a314.device open */
	BOOL a314_open;
};

const char library_name[] = CLASS_NAME;
const char id_string[] = CLASS_NAME " 1.0 (25.11.2026)";

static ULONG set_attrs(Object *obj, ULONG tag1, ...)
{
	return SetDTAttrsA(obj, NULL, NULL, (struct TagItem *)&tag1);
}

static ULONG get_attrs(Object *obj, ULONG tag1, ...)
{
	return GetDTAttrsA(obj, (struct TagItem *)&tag1);
}

/* Loads the sound into obj; returns 0, or an error for IoErr() */
static LONG load(struct ClassBase *base, Object *obj)
{
	struct DtJob *job = NULL;
	struct Conn *c;
	struct VoiceHeader *vh = NULL;
	BYTE *samples[2] = { NULL, NULL };
	ULONG source = 0, maxbytes, rate, channels, length, period, wanted, i;
	BPTR fh = 0;
	BOOL v44 = base->super_base->lib_Version >= 44;
	/* Before V40 (Workbench 3.0) the samples have to be in chip memory */
	ULONG memtype = base->super_base->lib_Version >= 40 ? MEMF_PUBLIC : MEMF_CHIP;
	ULONG received = 0;
	const char *p;
	char *q;
	LONG err;

	get_attrs(obj, DTA_SourceType, (ULONG)&source, DTA_Handle, (ULONG)&fh, TAG_DONE);
	if (source != DTST_FILE || !fh)
		return ERROR_OBJECT_WRONG_TYPE;
	if (v44)
		get_attrs(obj, SDTA_VoiceHeader, (ULONG)&vh, TAG_DONE);

	/* The file, to the Pi */
	err = dt_start(&job, &base->a314_ior.a314_Request, fh);
	if (err)
		goto out;
	c = &job->conn;
	err = DTERROR_INVALID_DATA;

	/* At most half of the largest free block for each channel */
	wanted = v44 ? 2 : 1;
	maxbytes = AvailMem(memtype | MEMF_LARGEST) >> wanted;	/* shift: no division */
	if (maxbytes > MAX_CHANNEL)
		maxbytes = MAX_CHANNEL;
	if (maxbytes < 1024)
	{
		err = ERROR_NO_FREE_STORE;
		goto out;
	}
	q = put_num(put_str((char *)c->out, "SOUND "), RATE);
	*q++ = ' ';
	q = put_num(q, wanted);
	*q++ = ' ';
	q = put_num(q, maxbytes);
	*q++ = ' ';
	q = put_num(q, (GfxBase->DisplayFlags & PAL) ? PAL_CLOCK : NTSC_CLOCK);
	if (!send_line(c, q) || !(p = get_line(c, DECODE_SECONDS)))
		goto out;
	if (starts_with(p, "ERR", &p))
	{
		err = err_code(p);
		goto out;
	}
	if (!starts_with(p, "SINFO", &p))
		goto out;
	rate = get_num(&p);
	channels = get_num(&p);
	length = get_num(&p);
	period = get_num(&p);
	if (rate < 1000 || rate > 65535 || channels < 1 || channels > wanted ||
		length == 0 || length > maxbytes || period < 124 || period > 65535)
		goto out;
	/* Before V44 the length has to be even; the last byte isn't played */
	if (!v44 && (length & 1))
		length--;
	if (length == 0)
		goto out;

	for (i = 0; i < channels; i++)
	{
		/* Freed by sound.datatype with FreeVec() (SDTA_FreeSampleData) */
		samples[i] = AllocVec(length + 1, memtype | MEMF_CLEAR);
		if (!samples[i])
		{
			err = ERROR_NO_FREE_STORE;
			goto out;
		}
	}

	for (;;)
	{
		ULONG ch, off, n;
		if (!send_line(c, put_str((char *)c->out, "NEXT")) || !(p = get_line(c, WAIT_SECONDS)))
			goto out;
		if (starts_with(p, "DONE", &p))
			break;
		if (!starts_with(p, "PCM", &p))
			goto out;
		ch = get_num(&p);
		off = get_num(&p);
		n = get_num(&p);
		/* Written so that nothing can overflow. A last odd byte (dropped
		   above before V44) may still come: the buffers have room. */
		if (ch >= channels || n == 0 || n > BUF_SIZE || n > length + 1 || off > length + 1 - n)
			goto out;
		ReadMemA314(samples[ch] + off, job->a314buf, n);
		received += n;
	}
	/* Everything, or the sound would end in whatever was in memory */
	if (received < channels * length)
		goto out;

	if (vh)
	{
		vh->vh_OneShotHiSamples = length;
		vh->vh_RepeatHiSamples = 0;
		vh->vh_SamplesPerHiCycle = 0;
		vh->vh_SamplesPerSec = rate;
		vh->vh_Octaves = 1;
		vh->vh_Compression = CMP_NONE;
		vh->vh_Volume = Unity;
	}
	if (v44 && channels == 2)
		set_attrs(obj, SDTA_LeftSample, (ULONG)samples[0], SDTA_RightSample, (ULONG)samples[1],
			SDTA_SampleLength, length, SDTA_SamplesPerSec, rate,
			SDTA_Volume, 64, SDTA_Cycles, 1, TAG_DONE);
	else if (v44)
		set_attrs(obj, SDTA_Sample, (ULONG)samples[0], SDTA_SampleLength, length,
			SDTA_SamplesPerSec, rate, SDTA_Volume, 64, SDTA_Cycles, 1, TAG_DONE);
	else
		set_attrs(obj, SDTA_Sample, (ULONG)samples[0], SDTA_SampleLength, length,
			SDTA_Period, period, SDTA_Volume, 64, SDTA_Cycles, 1, TAG_DONE);
	samples[0] = samples[1] = NULL;		/* the object has them now */
	err = 0;

out:
	for (i = 0; i < 2; i++)
		if (samples[i])
			FreeVec(samples[i]);
	dt_end(job);
	return err;
}

/* ---------------------------------------------------------------------- */
/* The class                                                              */

static ULONG dispatcher(__reg("a0") struct IClass *cl, __reg("a2") Object *o, __reg("a1") Msg msg)
{
	struct ClassBase *base = (struct ClassBase *)cl->cl_UserData;

	if (msg->MethodID == OM_NEW)
	{
		Object *obj = (Object *)do_super_method(cl, o, msg);
		if (obj)
		{
			LONG err = load(base, obj);
			if (err)
			{
				ULONG dispose = OM_DISPOSE;
				coerce_method(cl, obj, (Msg)&dispose);
				SetIoErr(err);
				obj = NULL;
			}
		}
		return (ULONG)obj;
	}
	return do_super_method(cl, o, msg);
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
	base->super_base = OpenLibrary("datatypes/sound.datatype", 39);
	if (!DOSBase || !IntuitionBase || !GfxBase || !DataTypesBase || !base->super_base)
		goto fail;

	base->a314_ior.a314_Request.io_Message.mn_Length = sizeof(struct A314_IORequest);
	if (OpenDevice(A314_NAME, 0, (struct IORequest *)&base->a314_ior, 0))
		goto fail;
	base->a314_open = TRUE;
	A314Base = (struct Library *)base->a314_ior.a314_Request.io_Device;

	base->cl = MakeClass(CLASS_NAME, SOUNDDTCLASS, NULL, 0, 0);
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
