/*
 * a314picture.datatype - shows pictures that the Amiga has no datatype
 * for (WebP, AVIF, HEIC, JPEG XL and TIFF) by having the Raspberry Pi
 * decode them, through the A314.
 *
 * It is a subclass of picture.datatype, so every program that uses
 * datatypes (MultiView and many others) can show these formats. The
 * descriptors in DEVS:DataTypes recognise the files and name this class.
 *
 * When an object is made, the file is sent to the datatypes service on the
 * Pi (datatypes.py) through a buffer in A314 memory; the Pi decodes it and
 * sends the pixels back the same way. With picture.datatype V43 or later
 * (Workbench 3.5, 3.9, 3.2) they are 24-bit, and picture.datatype fits
 * them to the screen; with an older one (3.0, 3.1) the Pi reduces them to
 * at most 256 colours.
 *
 * Pictures larger than the size in the environment variable
 * A314/PictureSize ("<width> <height>", set with Prefs/A314/Pictures;
 * default 1280 1024, or 640 512 with picture.datatype V40) are made
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
#include "a314dt.h"

#define SysBase (*(struct ExecBase **)4)

struct DosLibrary *DOSBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *DataTypesBase;
struct Library *A314Base;

#define CLASS_NAME	"a314picture.datatype"

#define DEFAULT_W	1280
#define DEFAULT_H	1024
#define DEFAULT_W_V40	640		/* smaller for 3.0/3.1: chip RAM bitmap */
#define DEFAULT_H_V40	512

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

/* The largest size wanted, from A314/PictureSize (set with
   Prefs/A314/Pictures); never so wide that a row doesn't fit the buffer */
static void wanted_size(ULONG *w, ULONG *h, BOOL v43)
{
	char buf[32];
	const char *p = buf;
	ULONG maxw = v43 ? BUF_SIZE / 3 : BUF_SIZE;
	*w = v43 ? DEFAULT_W : DEFAULT_W_V40;
	*h = v43 ? DEFAULT_H : DEFAULT_H_V40;
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
	if (*w > maxw)
		*w = maxw;
	if (*h > 32767)
		*h = 32767;
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
	struct DtJob *job = NULL;
	struct Conn *c;
	struct BitMapHeader *bmh = NULL;
	struct BitMap *bm = NULL;
	ULONG source = 0, maxw, maxh, w, h, a314buf;
	BPTR fh = 0;
	UBYTE *tmp;
	BOOL v43 = base->super_base->lib_Version >= 43;
	const char *p;
	char *q;
	LONG err;
	ULONG i, bpp = v43 ? 3 : 1;

	get_attrs(obj, DTA_SourceType, (ULONG)&source, DTA_Handle, (ULONG)&fh,
		PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE);
	if (source != DTST_FILE || !fh || !bmh)
		return ERROR_OBJECT_WRONG_TYPE;

	/* The file, to the Pi */
	err = dt_start(&job, &base->a314_ior.a314_Request, fh);
	if (err)
		goto out;
	c = &job->conn;
	tmp = job->tmp;
	a314buf = job->a314buf;
	err = DTERROR_INVALID_DATA;

	/* Decoding */
	wanted_size(&maxw, &maxh, v43);
	q = put_num(put_str((char *)c->out, "DECODE "), maxw);
	*q++ = ' ';
	q = put_num(q, maxh);
	q = put_str(q, v43 ? " rgb" : " pal 256");
	if (!send_line(c, q) || !(p = get_line(c, DECODE_SECONDS)))
		goto out;
	if (starts_with(p, "ERR", &p))
	{
		err = err_code(p);
		goto out;
	}
	if (!starts_with(p, "INFO", &p))
		goto out;
	w = get_num(&p);
	h = get_num(&p);
	if (!w || !h || w > maxw || h > maxh || w * bpp > BUF_SIZE)
		goto out;

	bmh->bmh_Left = bmh->bmh_Top = 0;
	bmh->bmh_Width = bmh->bmh_PageWidth = w;
	bmh->bmh_Height = bmh->bmh_PageHeight = h;
	bmh->bmh_Masking = mskNone;
	bmh->bmh_Compression = cmpNone;
	bmh->bmh_XAspect = bmh->bmh_YAspect = 1;

	if (v43)
	{
		/* picture.datatype V47 makes its pixel buffer from the nominal
		   size, which must therefore be set with the mode, before the
		   first PDTM_WRITEPIXELARRAY (see PDTA_ObtainPixelBuffer in its
		   autodoc). Otherwise the pixels are lost, and the picture is
		   never laid out. */
		bmh->bmh_Depth = 24;
		set_attrs(obj, PDTA_SourceMode, PMODE_V43, DTA_NominalHoriz, w, DTA_NominalVert, h, TAG_DONE);
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
			/* All 256, so that no pixel can point outside the palette */
			for (i = n * 3; i < 256 * 3; i++)
				tmp[i] = 0;
			n = 256;
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
			ULONG row = w * bpp;
			/* Written so that nothing can overflow: h <= 32767 and
			   row <= BUF_SIZE (checked at INFO), so n * row fits */
			if (n == 0 || n > h || y > h - n || n * row > BUF_SIZE)
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
				/* No return value is documented for this method */
				do_super_method(cl, obj, (Msg)&m);
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
	{
		/* The file's name, as the object's name: MultiView shows it in
		   its title bar, as it does for the system's own datatypes */
		STRPTR name = NULL;
		get_attrs(obj, DTA_Name, (ULONG)&name, TAG_DONE);
		if (name)
			set_attrs(obj, DTA_ObjName, (ULONG)name, TAG_DONE);
	}
	err = 0;

out:
	if (bm)
		FreeBitMap(bm);
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
			LONG err = load(base, cl, obj);
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
