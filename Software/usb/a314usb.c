/*
 * a314usb - the handler for USB0: to USB3:, USB memory sticks plugged into
 * the Raspberry Pi (served by the a314usb service, usb/a314usb.py).
 *
 * Based on a314fs (Copyright (c) 2018 Niklas Ekström), whose requests it
 * uses (a314fs/messages.h). What is new is that the volume comes and goes,
 * as a floppy's does: about once a second the handler asks the Pi what is
 * in its unit. When a stick is put in, its volume is added (or, if it is
 * the same stick as before, the volume that was kept for it is used again)
 * and a disk-inserted event lets Workbench know. When it is pulled out, the
 * volume stays (unmounted) while programs still have locks on it, so that
 * AmigaDOS asks for it to be put back, and is removed otherwise. With no
 * stick, the drive is empty, like a floppy drive.
 *
 * If the connection to the Pi is lost (a314d restarted), the drive is
 * empty until the handler has connected again, which it tries each second.
 *
 * A packet of type ACTION_USB_EJECT (0x5503, with no arguments) makes the
 * stick safe to pull out: the Pi closes its files, unmounts it and powers
 * it off, and the volume goes (as when a stick is pulled out). Prefs/A314/
 * Disks sends it.
 *
 * Kickstart 1.3 or later.
 */

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/nodes.h>
#include <exec/libraries.h>

#include <devices/timer.h>
#include <devices/input.h>
#include <devices/inputevent.h>

#include <libraries/dos.h>
#include <libraries/dosextens.h>
#include <libraries/filehandler.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>
#include <stdarg.h>

#include "../a314device/a314.h"
#include "../a314device/proto_a314.h"

#include "../a314fs/messages.h"

#pragma pack(push, 1)

/* The requests of the a314usb service besides a314fs's (see a314usb.py) */
#define ACTION_USB_UNIT		0x5501
#define ACTION_USB_STATUS	0x5502
#define ACTION_USB_EJECT	0x5503	/* also the packet that asks for it */

struct UsbUnitRequest
{
	short has_response;
	short type;
	short unit;
};

struct UsbStatusRequest
{
	short has_response;
	short type;
};

struct UsbStatusResponse
{
	short has_response;
	short success;
	short error_code;
	ULONG volume_id;		/* 0: no stick */
	ULONG date[3];
	UBYTE read_only;
	char name[1];			/* BCPL string */
};

/* a314fs/messages.h has no date request on upstream's main */
struct UsbSetDateRequest
{
	short has_response;
	short type;
	long key;
	long days;
	long minute;
	long tick;
	char name[1];
};

#pragma pack(pop)

// What fh_Arg1 points to: the Pi's key for the file, and the volume id
// (each insertion of a stick has its own) it was opened on
struct UsbFile
{
	long key;
	ULONG insertion;
};

// Counts the sticks put in (the Pi's volume ids start again when its
// service does): a file is on the stick that is in if its insertion is
// this one
ULONG insertion = 0;

// The Pi's key for the file of the packet being handled, from refused()
// (the packet keeps the UsbFile pointer: programs may reuse a packet, with
// fh_Arg1 in it, for every read)
long cur_file_key;

#define POLL_MICROS	1000000		/* how often the Pi is asked */

#define ID_314_DISK (('3' << 24) | ('1' << 16) | ('4' << 8))

#define REQ_RES_BUF_SIZE 600	// two 255-byte names and a header
#define BUFFER_SIZE 4096

// Not defined if using NDK13
#ifndef ACTION_IS_FILESYSTEM
#define ACTION_IS_FILESYSTEM 1027
#endif

#ifndef ACTION_EXAMINE_FH
#define ACTION_EXAMINE_FH 1034
#endif

#ifndef ACTION_SAME_LOCK
#define ACTION_SAME_LOCK 40
#endif

// Grab a reserved action type which seems to be unused
#define ACTION_UNSUPPORTED 65535

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct MsgPort *mp;

struct DeviceList *my_volume = NULL;	// the stick's volume, NULL when empty
ULONG volume_id = 0;			// the Pi's id for the stick in the unit
BOOL volume_read_only = FALSE;

char device_name[32]; // "\005USB0:"
ULONG unit = 0;

BOOL a314_open = FALSE;			// a314.device and the buffers are there
BOOL connected = FALSE;			// to the a314usb service

struct MsgPort *timer_mp;
struct timerequest *tr;			// asks the Pi what is in the unit
BOOL timer_pending = FALSE;

struct MsgPort *input_mp;
struct IOStdReq *input_ior;		// for disk-inserted and -removed events
BOOL input_open = FALSE;

void start_timer();
void poll_unit();

struct MsgPort *a314_mp;
struct A314_IORequest *a314_ior;

struct Library *A314Base;

long socket;

// Until the A314 buffer is there (and if it can't be had), requests are
// answered with failures in this one, so that nothing writes through NULL
char fallback_buffer[REQ_RES_BUF_SIZE];
char *request_buffer = fallback_buffer;
BOOL request_buffer_in_a314_memory;

// These are allocated in A314 memory.
ULONG request_buffer_address;
ULONG data_buffer_address;

void MyNewList(struct List *l)
{
    l->lh_Head = (struct Node *)&(l->lh_Tail);
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&(l->lh_Head);
}

struct MsgPort *MyCreatePort(char *name, long pri)
{
	int sigbit = AllocSignal(-1);
	if (sigbit == -1)
		return NULL;

	struct MsgPort *port = (struct MsgPort *)AllocMem(sizeof(struct MsgPort), MEMF_PUBLIC | MEMF_CLEAR);
	if (!port)
	{
		FreeSignal(sigbit);
		return NULL;
	}

	port->mp_Node.ln_Name = name;
	port->mp_Node.ln_Pri = pri;
	port->mp_Node.ln_Type = NT_MSGPORT;
	port->mp_Flags = PA_SIGNAL;
	port->mp_SigBit = sigbit;
	port->mp_SigTask = FindTask(NULL);

	if (name)
		AddPort(port);
	else
		MyNewList(&(port->mp_MsgList));

	return port;
}

struct IORequest *MyCreateExtIO(struct MsgPort *ioReplyPort, ULONG size)
{
	if (!ioReplyPort)
		return NULL;

	struct IORequest *ioReq = (struct IORequest *)AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
	if (!ioReq)
		return NULL;

	ioReq->io_Message.mn_Node.ln_Type = NT_MESSAGE;
	ioReq->io_Message.mn_Length = size;
	ioReq->io_Message.mn_ReplyPort = ioReplyPort;

	return ioReq;
}

#define DEBUG 0

#if !DEBUG
void dbg_init()
{
}

void dbg(const char* fmt, ...)
{
}
#else
struct FileHandle *dbg_log;
struct MsgPort *dbg_replyport;
struct StandardPacket *dbg_sp;
char dbg_buf[256];

void dbg_init()
{
	dbg_log = (struct FileHandle *)BADDR(Open("CON:200/0/440/256/a314fs", MODE_NEWFILE));
	dbg_replyport = MyCreatePort(NULL, 0);
	dbg_sp = (struct StandardPacket *)AllocMem(sizeof(struct StandardPacket), MEMF_PUBLIC | MEMF_CLEAR);
}

void dbg_out(int l)
{
	dbg_sp->sp_Msg.mn_Node.ln_Name = (char *)&(dbg_sp->sp_Pkt);
	dbg_sp->sp_Pkt.dp_Link = &(dbg_sp->sp_Msg);
	dbg_sp->sp_Pkt.dp_Port = dbg_replyport;
	dbg_sp->sp_Pkt.dp_Type = ACTION_WRITE;
	dbg_sp->sp_Pkt.dp_Arg1 = (long)dbg_log->fh_Arg1;
	dbg_sp->sp_Pkt.dp_Arg2 = (long)dbg_buf;
	dbg_sp->sp_Pkt.dp_Arg3 = (long)l - 1;

	PutMsg((struct MsgPort *)dbg_log->fh_Type, (struct Message *)dbg_sp);
	WaitPort(dbg_replyport);
	GetMsg(dbg_replyport);
}

void dbg(const char* fmt, ...)
{
	char numbuf[16];

	const char *p = fmt;
	char *q = dbg_buf;

	va_list args;
	va_start(args, fmt);
	while (*p != 0)
	{
		char c = *p++;
		if (c == '$')
		{
			c = *p++;
			if (c == 'b')
			{
				UBYTE x = va_arg(args, UBYTE);
				*q++ = '$';
				for (int i = 0; i < 2; i++)
				{
					int ni = (x >> ((1 - i) * 4)) & 0xf;
					*q++ = (ni >= 10) ? ('a' + (ni - 10)) : ('0' + ni);
				}
			}
			else if (c == 'w')
			{
				UWORD x = va_arg(args, UWORD);
				*q++ = '$';
				for (int i = 0; i < 4; i++)
				{
					int ni = (x >> ((3 - i) * 4)) & 0xf;
					*q++ = (ni >= 10) ? ('a' + (ni - 10)) : ('0' + ni);
				}
			}
			else if (c == 'l')
			{
				ULONG x = va_arg(args, ULONG);
				*q++ = '$';
				for (int i = 0; i < 8; i++)
				{
					int ni = (x >> ((7 - i) * 4)) & 0xf;
					*q++ = (ni >= 10) ? ('a' + (ni - 10)) : ('0' + ni);
				}
			}
			else if (c == 'S')
			{
				unsigned char *s = (unsigned char *)va_arg(args, ULONG);
				int l = *s++;
				for (int i = 0; i < l; i++)
					*q++ = *s++;
			}
			else if (c == 's')
			{
				unsigned char *s = (unsigned char *)va_arg(args, ULONG);
				while (*s)
					*q++ = *s++;
				*q++ = 0;
			}
		}
		else
		{
			*q++ = c;
		}
	}
	*q++ = 0;

	va_end(args);

	dbg_out(q - dbg_buf);
}
#endif

char *DosAllocMem(int len)
{
	long *p = (long *)AllocMem(len + 4, MEMF_PUBLIC | MEMF_CLEAR);
	if (!p)
		return NULL;
	*p++ = len;
	return((char *)p);
}

void DosFreeMem(char *p)
{
	long *lp = (long *)p;
	long len = *--lp;
	FreeMem((char *)lp, len + 4);
}

void reply_packet(struct DosPacket *dp)
{
	struct MsgPort *reply_port = dp->dp_Port;
	dp->dp_Port = mp;
	PutMsg(reply_port, dp->dp_Link);
}

// Routines for talking to the A314 driver.
LONG a314_cmd_wait(UWORD command, char *buffer, int length)
{
	a314_ior->a314_Request.io_Command = command;
	a314_ior->a314_Request.io_Error = 0;
	a314_ior->a314_Socket = socket;
	a314_ior->a314_Buffer = buffer;
	a314_ior->a314_Length = length;
	return DoIO((struct IORequest *)a314_ior);
}

LONG a314_connect(char *name)
{
	struct DateStamp ds;
	DateStamp(&ds);
	socket = ds.ds_Tick;
	return a314_cmd_wait(A314_CONNECT, name, strlen(name));
}

LONG a314_read(char *buf, int length)
{
	return a314_cmd_wait(A314_READ, buf, length);
}

LONG a314_write(char *buf, int length)
{
	return a314_cmd_wait(A314_WRITE, buf, length);
}

LONG a314_eos()
{
	return a314_cmd_wait(A314_EOS, NULL, 0);
}

LONG a314_reset()
{
	return a314_cmd_wait(A314_RESET, NULL, 0);
}

// Tells Workbench (and anything else watching) that a disk came or went
void disk_event(UBYTE ie_class)
{
	struct InputEvent ie;

	if (!input_open)
		return;
	memset(&ie, 0, sizeof(ie));
	ie.ie_Class = ie_class;
	input_ior->io_Command = IND_WRITEEVENT;
	input_ior->io_Data = (APTR)&ie;
	input_ior->io_Length = sizeof(ie);
	DoIO((struct IORequest *)input_ior);
}

#define DOS_V36 (DOSBase->dl_lib.lib_Version >= 36)

// FALSE if the list is busy: the change is then made at the next poll (a
// handler must not wait for the list, as whoever holds it may be waiting
// for this handler)
static BOOL lock_dos_list()
{
	if (DOS_V36)
		return (ULONG)AttemptLockDosList(LDF_VOLUMES | LDF_WRITE) > 1;	// some versions give 1 for failure
	Forbid();
	return TRUE;
}

static void unlock_dos_list()
{
	if (DOS_V36)
		UnLockDosList(LDF_VOLUMES | LDF_WRITE);
	else
		Permit();
}

static struct DosInfo *dos_info()
{
	struct RootNode *root = (struct RootNode *)DOSBase->dl_Root;
	return (struct DosInfo *)BADDR(root->rn_Info);
}

static BOOL same_bstr(unsigned char *a, unsigned char *b)
{
	int i;
	if (a[0] != b[0])
		return FALSE;
	for (i = 1; i <= a[0]; i++)
		if (a[i] != b[i])
			return FALSE;
	return TRUE;
}

#define MAX_PENDING 8
struct DeviceList *pending_free[MAX_PENDING];	// to free when the list is free

// Takes a volume node (with no locks) out of the device list and frees it;
// if the list is busy, at the next poll
void free_volume_node(struct DeviceList *vol)
{
	struct DosInfo *info = dos_info();
	BPTR bvol = MKBADDR(vol);
	int i;

	if (!lock_dos_list())
	{
		for (i = 0; i < MAX_PENDING; i++)
			if (!pending_free[i])
			{
				pending_free[i] = vol;
				break;
			}
		return;			// (if there is no room, the node is left)
	}
	if (DOS_V36)
		RemDosEntry((struct DosList *)vol);
	else if (info->di_DevInfo == bvol)
		info->di_DevInfo = vol->dl_Next;
	else
	{
		struct DeviceList *prev = (struct DeviceList *)BADDR(info->di_DevInfo);
		while (prev && prev->dl_Next != bvol)
			prev = (struct DeviceList *)BADDR(prev->dl_Next);
		if (prev)
			prev->dl_Next = vol->dl_Next;
	}
	unlock_dos_list();
	DosFreeMem((char *)BADDR(vol->dl_Name));
	DosFreeMem((char *)vol);
}

void free_pending()
{
	int i;
	for (i = 0; i < MAX_PENDING; i++)
		if (pending_free[i])
		{
			struct DeviceList *vol = pending_free[i];
			pending_free[i] = NULL;
			free_volume_node(vol);
		}
}

// Whether a volume node is one this handler kept for a stick that was
// pulled out (it has locks, and they are this handler's)
static BOOL kept_by_me(struct DeviceList *vol)
{
	struct FileLock *lock = (struct FileLock *)BADDR(vol->dl_Lock);
	return vol->dl_Task == NULL && lock && lock->fl_Task == mp;
}

// The stick is in the unit: its volume, a new one, or the one kept for it
// (same name and date). If the list is busy, nothing changes, and the next
// poll tries again.
void insert_volume(struct UsbStatusResponse *res)
{
	struct DosInfo *info = dos_info();
	unsigned char *name = (unsigned char *)res->name;
	struct DeviceList *vol;

	// A new node, made before the list is locked (in case it is needed)
	unsigned char *bname = (unsigned char *)DosAllocMem(name[0] + 2);
	struct DeviceList *fresh = (struct DeviceList *)DosAllocMem(sizeof(struct DeviceList));
	if (bname && fresh)
	{
		memcpy(bname, name, name[0] + 1);
		fresh->dl_Name = MKBADDR(bname);
		fresh->dl_Type = DLT_VOLUME;
		fresh->dl_Task = mp;
		fresh->dl_DiskType = ID_314_DISK;
		fresh->dl_VolumeDate.ds_Days = res->date[0];
		fresh->dl_VolumeDate.ds_Minute = res->date[1];
		fresh->dl_VolumeDate.ds_Tick = res->date[2];
	}

	if (!lock_dos_list())
		vol = NULL;
	else
	{
		for (vol = (struct DeviceList *)BADDR(info->di_DevInfo); vol;
			vol = (struct DeviceList *)BADDR(vol->dl_Next))
		{
			if (vol->dl_Type == DLT_VOLUME && vol->dl_DiskType == ID_314_DISK &&
				kept_by_me(vol) &&
				vol->dl_VolumeDate.ds_Days == res->date[0] &&
				vol->dl_VolumeDate.ds_Minute == res->date[1] &&
				vol->dl_VolumeDate.ds_Tick == res->date[2] &&
				same_bstr((unsigned char *)BADDR(vol->dl_Name), name))
			{
				vol->dl_Task = mp;
				break;
			}
		}
		if (!vol && bname && fresh)
		{
			if (DOS_V36)
			{
				if (AddDosEntry((struct DosList *)fresh))
					vol = fresh;
			}
			else
			{
				fresh->dl_Next = info->di_DevInfo;
				info->di_DevInfo = MKBADDR(fresh);
				vol = fresh;
			}
		}
		unlock_dos_list();
	}

	if (vol != fresh)
	{
		if (bname)
			DosFreeMem((char *)bname);
		if (fresh)
			DosFreeMem((char *)fresh);
	}
	if (!vol)
		return;			// tried again at the next poll

	my_volume = vol;
	volume_id = res->volume_id;
	insertion++;
	volume_read_only = res->read_only;
	disk_event(IECLASS_DISKINSERTED);
}

// The stick was pulled out (or the Pi went away): the volume is kept while
// there are locks on it, unmounted, so that AmigaDOS asks for it. FALSE if
// the list is busy (tried again at the next poll).
BOOL remove_volume()
{
	struct DeviceList *vol = my_volume;

	if (!vol)
	{
		volume_id = 0;
		return TRUE;
	}
	if (!lock_dos_list())
		return FALSE;
	vol->dl_Task = NULL;
	unlock_dos_list();
	my_volume = NULL;
	volume_id = 0;

	Forbid();
	BOOL unused = !vol->dl_Lock;
	Permit();
	if (unused)
		free_volume_node(vol);
	disk_event(IECLASS_DISKREMOVED);
	return TRUE;
}

void startup_fs_handler(struct DosPacket *dp)
{
	unsigned char *name = (unsigned char *)BADDR(dp->dp_Arg1);
	struct FileSysStartupMsg *fssm = (struct FileSysStartupMsg *)BADDR(dp->dp_Arg2);
	struct DeviceNode *node = (struct DeviceNode *)BADDR(dp->dp_Arg3);

	memcpy(device_name, name, *name + 1);
	device_name[*name + 1] = 0;
	if (fssm)
		unit = fssm->fssm_Unit;

	node->dn_Task = mp;

	dp->dp_Res1 = DOSTRUE;
	dp->dp_Res2 = 0;
	reply_packet(dp);

	dbg_init();

	dbg("ACTION_STARTUP\n");
	dbg("  device_name = $S\n", (ULONG)device_name);
	dbg("  fssm = $l\n", (ULONG)fssm);
	dbg("  node = $l\n", (ULONG)node);

	// Without these the drive just stays empty
	timer_mp = MyCreatePort(NULL, 0);
	tr = (struct timerequest *)MyCreateExtIO(timer_mp, sizeof(struct timerequest));
	if (!tr || OpenDevice(TIMERNAME, UNIT_VBLANK, (struct IORequest *)tr, 0) != 0)
	{
		dbg("Fatal error: unable to open timer.device\n");
		return;
	}

	input_mp = MyCreatePort(NULL, 0);
	input_ior = (struct IOStdReq *)MyCreateExtIO(input_mp, sizeof(struct IOStdReq));
	if (input_ior && OpenDevice("input.device", 0, (struct IORequest *)input_ior, 0) == 0)
		input_open = TRUE;

	a314_mp = MyCreatePort(NULL, 0);
	a314_ior = (struct A314_IORequest *)MyCreateExtIO(a314_mp, sizeof(struct A314_IORequest));
	if (!a314_ior || OpenDevice(A314_NAME, 0, (struct IORequest *)a314_ior, 0) != 0)
	{
		dbg("Fatal error: unable to open a314.device\n");
		start_timer();
		return;
	}

	A314Base = &(a314_ior->a314_Request.io_Device->dd_Library);

	BOOL have_buffer = FALSE;
	request_buffer = AllocMem(REQ_RES_BUF_SIZE, MEMF_A314);
	request_buffer_in_a314_memory = request_buffer != NULL;
	if (request_buffer_in_a314_memory)
		request_buffer_address = TranslateAddressA314(request_buffer);
	else
	{
		request_buffer = AllocMem(REQ_RES_BUF_SIZE, 0);
		request_buffer_address = AllocMemA314(REQ_RES_BUF_SIZE);
	}
	have_buffer = request_buffer != NULL;
	if (!request_buffer)
	{
		request_buffer = fallback_buffer;
	}

	data_buffer_address = AllocMemA314(BUFFER_SIZE);

	a314_open = have_buffer && request_buffer_address != INVALID_A314_ADDRESS &&
		data_buffer_address != INVALID_A314_ADDRESS;

	// What is in the unit now, then once a second
	poll_unit();
	start_timer();

	dbg("Startup successful\n");
}

// Sends the request in request_buffer and puts the answer there. Without a
// connection, or if it is lost, the answer is a failure with ERROR_NO_DISK,
// and the drive is empty until the handler has connected again.
void write_req_and_wait_for_res(int len)
{
	short *res = (short *)request_buffer;

	if (connected)
	{
		if (!request_buffer_in_a314_memory)
			WriteMemA314(request_buffer_address, request_buffer, len);
		ULONG buf[2] = {request_buffer_address, len};
		if (a314_write((char *)&buf[0], 8) == A314_WRITE_OK &&
			a314_read((char *)&buf[1], 4) == A314_READ_OK)
		{
			len = buf[1];
			if (!request_buffer_in_a314_memory)
				ReadMemA314(request_buffer, request_buffer_address, len);
			return;
		}
		connected = FALSE;
		a314_reset();
		remove_volume();
	}
	res[1] = 0;			// success
	res[2] = ERROR_NO_DISK;		// error_code
}

// Connects to the a314usb service and says which unit this is
BOOL connect_to_pi()
{
	if (a314_connect("a314usb") != A314_CONNECT_OK)
		return FALSE;
	connected = TRUE;

	struct UsbUnitRequest *req = (struct UsbUnitRequest *)request_buffer;
	req->has_response = 0;
	req->type = ACTION_USB_UNIT;
	req->unit = unit;
	write_req_and_wait_for_res(sizeof(struct UsbUnitRequest));
	if (!connected || !((short *)request_buffer)[1])
	{
		if (connected)
			a314_reset();
		connected = FALSE;
		return FALSE;
	}
	return TRUE;
}

// Asks the Pi what is in the unit, and adds or removes the volume
void poll_unit()
{
	if (!a314_open)
		return;
	free_pending();
	if (!connected)
	{
		// The old volume first: the service may have restarted, and its
		// ids with it
		if (!remove_volume() || !connect_to_pi())
			return;
	}

	struct UsbStatusRequest *req = (struct UsbStatusRequest *)request_buffer;
	req->has_response = 0;
	req->type = ACTION_USB_STATUS;
	write_req_and_wait_for_res(sizeof(struct UsbStatusRequest));

	struct UsbStatusResponse *res = (struct UsbStatusResponse *)request_buffer;
	if (!res->success)
		return;
	if (res->volume_id != volume_id)
	{
		if (!remove_volume())
			return;
		if (res->volume_id)
			insert_volume(res);
	}
}

void start_timer()
{
	tr->tr_node.io_Command = TR_ADDREQUEST;
	tr->tr_time.tv_secs = POLL_MICROS / 1000000;
	tr->tr_time.tv_micro = POLL_MICROS % 1000000;
	SendIO((struct IORequest *)tr);
	timer_pending = TRUE;
}

struct FileLock *create_and_add_file_lock(long key, long mode)
{
	struct FileLock *lock = (struct FileLock *)DosAllocMem(sizeof(struct FileLock));
	if (!lock)
		return NULL;

	lock->fl_Key = key;
	lock->fl_Access = mode;
	lock->fl_Task = mp;
	lock->fl_Volume = MKBADDR(my_volume);

	Forbid();
	lock->fl_Link = my_volume->dl_Lock;
	my_volume->dl_Lock = MKBADDR(lock);
	Permit();

	return lock;
}

void action_locate_object(struct DosPacket *dp)
{
	struct FileLock *parent = (struct FileLock *)BADDR(dp->dp_Arg1);
	unsigned char *name = (unsigned char *)BADDR(dp->dp_Arg2);
	long mode = dp->dp_Arg3;

	dbg("ACTION_LOCATE_OBJECT\n");
	dbg("  parent lock = $l\n", parent);
	dbg("  name = $S\n", name);
	dbg("  mode = $s\n", mode == SHARED_LOCK ? "SHARED_LOCK" : "EXCLUSIVE_LOCK");

	struct LocateObjectRequest *req = (struct LocateObjectRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = parent == NULL ? 0 : parent->fl_Key;
	req->mode = mode;

	int nlen = *name;
	memcpy(req->name, name, nlen + 1);

	write_req_and_wait_for_res(sizeof(struct LocateObjectRequest) + nlen);

	struct LocateObjectResponse *res = (struct LocateObjectResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		struct FileLock *lock = create_and_add_file_lock(res->key, mode);

		dbg("  Returning lock $l\n", lock);
		dp->dp_Res1 = MKBADDR(lock);
		dp->dp_Res2 = lock ? 0 : ERROR_NO_FREE_STORE;
	}

	reply_packet(dp);
}

void action_free_lock(struct DosPacket *dp)
{
	ULONG arg1 = dp->dp_Arg1;
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg1);

	dbg("ACTION_FREE_LOCK\n");
	dbg("  lock = $l\n", lock);

	if (lock != NULL)
	{
		struct DeviceList *vol = (struct DeviceList *)BADDR(lock->fl_Volume);

		// The Pi is told only if the stick is in; a volume that was pulled
		// out keeps its locks on the Pi until it is put back
		if (vol == my_volume)
		{
			struct FreeLockRequest *req = (struct FreeLockRequest *)request_buffer;
			req->has_response = 0;
			req->type = dp->dp_Type;
			req->key = lock->fl_Key;

			write_req_and_wait_for_res(sizeof(struct FreeLockRequest));
		}

		Forbid();
		if (vol->dl_Lock == arg1)
			vol->dl_Lock = lock->fl_Link;
		else
		{
			struct FileLock *prev = (struct FileLock *)BADDR(vol->dl_Lock);
			while (prev && prev->fl_Link != arg1)
				prev = (struct FileLock *)BADDR(prev->fl_Link);
			if (prev)
				prev->fl_Link = lock->fl_Link;
		}
		Permit();
		DosFreeMem((char *)lock);

		// The last lock on a volume that was pulled out: it can go
		if (vol != my_volume && vol->dl_Task == NULL && !vol->dl_Lock)
			free_volume_node(vol);
	}

	dp->dp_Res1 = DOSTRUE;
	dp->dp_Res2 = 0;
	reply_packet(dp);
}

void action_copy_dir(struct DosPacket *dp)
{
	struct FileLock *parent = (struct FileLock *)BADDR(dp->dp_Arg1);

	dbg("ACTION_COPY_DIR\n");
	dbg("  lock to duplicate = $l\n", parent);

	if (parent == NULL)
	{
		dp->dp_Res1 = 0;
		dp->dp_Res2 = 0;
	}
	else
	{
		struct CopyDirRequest *req = (struct CopyDirRequest *)request_buffer;
		req->has_response = 0;
		req->type = dp->dp_Type;
		req->key = parent->fl_Key;

		write_req_and_wait_for_res(sizeof(struct CopyDirRequest));

		struct CopyDirResponse *res = (struct CopyDirResponse *)request_buffer;
		if (!res->success)
		{
			dbg("  Failed, error code $l\n", (LONG)res->error_code);
			dp->dp_Res1 = DOSFALSE;
			dp->dp_Res2 = res->error_code;
		}
		else
		{
			struct FileLock *lock = create_and_add_file_lock(res->key, parent->fl_Access);

			dbg("  Returning lock $l\n", lock);
			dp->dp_Res1 = MKBADDR(lock);
			dp->dp_Res2 = lock ? 0 : ERROR_NO_FREE_STORE;
		}
	}

	reply_packet(dp);
}

void action_parent(struct DosPacket *dp)
{
	struct FileLock *prev_lock = (struct FileLock *)BADDR(dp->dp_Arg1);

	dbg("ACTION_PARENT\n");
	dbg("  lock = $l\n", prev_lock);

	if (prev_lock == NULL)
	{
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = ERROR_INVALID_LOCK;
	}
	else
	{
		struct ParentRequest *req = (struct ParentRequest *)request_buffer;
		req->has_response = 0;
		req->type = dp->dp_Type;
		req->key = prev_lock->fl_Key;

		write_req_and_wait_for_res(sizeof(struct ParentRequest));

		struct ParentResponse *res = (struct ParentResponse *)request_buffer;
		if (!res->success)
		{
			dbg("  Failed, error code $l\n", (LONG)res->error_code);
			dp->dp_Res1 = DOSFALSE;
			dp->dp_Res2 = res->error_code;
		}
		else if (res->key == 0)
		{
			dp->dp_Res1 = 0;
			dp->dp_Res2 = 0;
		}
		else
		{
			struct FileLock *lock = create_and_add_file_lock(res->key, SHARED_LOCK);

			dbg("  Returning lock $l\n", lock);
			dp->dp_Res1 = MKBADDR(lock);
			dp->dp_Res2 = lock ? 0 : ERROR_NO_FREE_STORE;
		}
	}

	reply_packet(dp);
}

void action_examine_object(struct DosPacket *dp)
{
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg1);
	struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(dp->dp_Arg2);

	dbg("ACTION_EXAMINE_OBJECT\n");
	dbg("  lock = $l\n", lock);
	dbg("  fib = $l\n", fib);

	struct ExamineObjectRequest *req = (struct ExamineObjectRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = lock == NULL ? 0 : lock->fl_Key;

	write_req_and_wait_for_res(sizeof(struct ExamineObjectRequest));

	struct ExamineObjectResponse *res = (struct ExamineObjectResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		int nlen = (unsigned char)(res->data[0]);
		memcpy(fib->fib_FileName, res->data, nlen + 1);
		fib->fib_FileName[nlen + 1] = 0;

		int clen = (unsigned char)(res->data[nlen + 1]);
		memcpy(fib->fib_Comment, &(res->data[nlen + 1]), clen + 1);
		fib->fib_Comment[clen + 1] = 0;

		fib->fib_DiskKey = res->disk_key;
		fib->fib_DirEntryType = res->entry_type;
		fib->fib_EntryType = res->entry_type;
		fib->fib_Protection = res->protection;
		fib->fib_Size = res->size;
		fib->fib_NumBlocks = (res->size + 511) >> 9;
		fib->fib_Date.ds_Days = res->date[0];
		fib->fib_Date.ds_Minute = res->date[1];
		fib->fib_Date.ds_Tick = res->date[2];

		dp->dp_Res1 = DOSTRUE;
		dp->dp_Res2 = 0;
	}

	reply_packet(dp);
}

void action_examine_next(struct DosPacket *dp)
{
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg1);
	struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(dp->dp_Arg2);

	dbg("ACTION_EXAMINE_NEXT\n");
	dbg("  lock = $l\n", lock);
	dbg("  fib = $l\n", fib);

	struct ExamineNextRequest *req = (struct ExamineNextRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = lock == NULL ? 0 : lock->fl_Key;
	req->disk_key = fib->fib_DiskKey;

	write_req_and_wait_for_res(sizeof(struct ExamineNextRequest));

	struct ExamineNextResponse *res = (struct ExamineNextResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		int nlen = (unsigned char)(res->data[0]);
		memcpy(fib->fib_FileName, res->data, nlen + 1);
		fib->fib_FileName[nlen + 1] = 0;

		int clen = (unsigned char)(res->data[nlen + 1]);
		memcpy(fib->fib_Comment, &(res->data[nlen + 1]), clen + 1);
		fib->fib_Comment[clen + 1] = 0;

		fib->fib_DiskKey = res->disk_key;
		fib->fib_DirEntryType = res->entry_type;
		fib->fib_EntryType = res->entry_type;
		fib->fib_Protection = res->protection;
		fib->fib_Size = res->size;
		fib->fib_NumBlocks = (res->size + 511) >> 9;
		fib->fib_Date.ds_Days = res->date[0];
		fib->fib_Date.ds_Minute = res->date[1];
		fib->fib_Date.ds_Tick = res->date[2];

		dp->dp_Res1 = DOSTRUE;
		dp->dp_Res2 = 0;
	}

	reply_packet(dp);
}

void action_examine_fh(struct DosPacket *dp)
{
	ULONG arg1 = cur_file_key;
	struct FileInfoBlock *fib = (struct FileInfoBlock *)BADDR(dp->dp_Arg2);

	dbg("ACTION_EXAMINE_FH\n");
	dbg("  arg1 = $l\n", arg1);
	dbg("  fib = $l\n", fib);

	struct ExamineFhRequest *req = (struct ExamineFhRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->arg1 = arg1;

	write_req_and_wait_for_res(sizeof(struct ExamineFhRequest));

	struct ExamineFhResponse *res = (struct ExamineFhResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		int nlen = (unsigned char)(res->data[0]);
		memcpy(fib->fib_FileName, res->data, nlen + 1);
		fib->fib_FileName[nlen + 1] = 0;

		int clen = (unsigned char)(res->data[nlen + 1]);
		memcpy(fib->fib_Comment, &(res->data[nlen + 1]), clen + 1);
		fib->fib_Comment[clen + 1] = 0;

		fib->fib_DiskKey = res->disk_key;
		fib->fib_DirEntryType = res->entry_type;
		fib->fib_EntryType = res->entry_type;
		fib->fib_Protection = res->protection;
		fib->fib_Size = res->size;
		fib->fib_NumBlocks = (res->size + 511) >> 9;
		fib->fib_Date.ds_Days = res->date[0];
		fib->fib_Date.ds_Minute = res->date[1];
		fib->fib_Date.ds_Tick = res->date[2];

		dp->dp_Res1 = DOSTRUE;
		dp->dp_Res2 = 0;
	}

	reply_packet(dp);
}

void action_findxxx(struct DosPacket *dp)
{
	struct FileHandle *fh = (struct FileHandle *)BADDR(dp->dp_Arg1);
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg2);
	unsigned char *name = (unsigned char *)BADDR(dp->dp_Arg3);

	if (dp->dp_Type == ACTION_FINDUPDATE)
		dbg("ACTION_FINDUPDATE\n");
	else if (dp->dp_Type == ACTION_FINDINPUT)
		dbg("ACTION_FINDINPUT\n");
	else if (dp->dp_Type == ACTION_FINDOUTPUT)
		dbg("ACTION_FINDOUTPUT\n");

	dbg("  file handle = $l\n", fh);
	dbg("  lock = $l\n", lock);
	dbg("  name = $S\n", name);

	struct FindXxxRequest *req = (struct FindXxxRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = lock == NULL ? 0 : lock->fl_Key;

	int nlen = *name;
	memcpy(req->name, name, nlen + 1);

	write_req_and_wait_for_res(sizeof(struct FindXxxRequest) + nlen);

	struct FindXxxResponse *res = (struct FindXxxResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		struct UsbFile *uf = (struct UsbFile *)AllocMem(sizeof(struct UsbFile), MEMF_PUBLIC);
		if (!uf)
		{
			// Can't keep it: closed again on the Pi
			struct EndRequest *ereq = (struct EndRequest *)request_buffer;
			long key = res->arg1;
			ereq->has_response = 0;
			ereq->type = ACTION_END;
			ereq->arg1 = key;
			write_req_and_wait_for_res(sizeof(struct EndRequest));
			dp->dp_Res1 = DOSFALSE;
			dp->dp_Res2 = ERROR_NO_FREE_STORE;
			reply_packet(dp);
			return;
		}
		uf->key = res->arg1;
		uf->insertion = insertion;
		fh->fh_Arg1 = (long)uf;
		fh->fh_Type = mp;
		fh->fh_Port = DOSFALSE;  // Not an interactive file.

		dp->dp_Res1 = DOSTRUE;
		dp->dp_Res2 = 0;
	}

	reply_packet(dp);
}

void action_read(struct DosPacket *dp)
{
	ULONG arg1 = cur_file_key;
	UBYTE *dst = (UBYTE *)dp->dp_Arg2;
	int length = dp->dp_Arg3;

	dbg("ACTION_READ\n");
	dbg("  arg1 = $l\n", arg1);
	dbg("  length = $l\n", length);

	if (length == 0)
	{
		dp->dp_Res1 = -1;
		dp->dp_Res2 = ERROR_INVALID_LOCK; // This is not the correct error.
		reply_packet(dp);
		return;
	}

	int total_read = 0;
	while (length)
	{
		int to_read = length;
		if (to_read > BUFFER_SIZE)
			to_read = BUFFER_SIZE;

		struct ReadRequest *req = (struct ReadRequest *)request_buffer;
		req->has_response = 0;
		req->type = dp->dp_Type;
		req->arg1 = arg1;
		req->address = data_buffer_address;
		req->length = to_read;

		write_req_and_wait_for_res(sizeof(struct ReadRequest));

		struct ReadResponse *res = (struct ReadResponse *)request_buffer;
		if (!res->success)
		{
			dbg("  Failed, error code $l\n", (LONG)res->error_code);
			dp->dp_Res1 = -1;
			dp->dp_Res2 = res->error_code;
			reply_packet(dp);
			return;
		}

		if (res->actual)
		{
			ReadMemA314(dst, data_buffer_address, res->actual);
			dst += res->actual;
			total_read += res->actual;
			length -= res->actual;
		}

		if (res->actual < to_read)
			break;
	}

	dp->dp_Res1 = total_read;
	dp->dp_Res2 = 0;
	reply_packet(dp);
}

void action_write(struct DosPacket *dp)
{
	ULONG arg1 = cur_file_key;
	UBYTE *src = (UBYTE *)dp->dp_Arg2;
	int length = dp->dp_Arg3;

	dbg("ACTION_WRITE\n");
	dbg("  arg1 = $l\n", arg1);
	dbg("  length = $l\n", length);

	int total_written = 0;
	while (length)
	{
		int to_write = length;
		if (to_write > BUFFER_SIZE)
			to_write = BUFFER_SIZE;

		WriteMemA314(data_buffer_address, src, to_write);

		struct WriteRequest *req = (struct WriteRequest *)request_buffer;
		req->has_response = 0;
		req->type = dp->dp_Type;
		req->arg1 = arg1;
		req->address = data_buffer_address;
		req->length = to_write;

		write_req_and_wait_for_res(sizeof(struct WriteRequest));

		struct WriteResponse *res = (struct WriteResponse *)request_buffer;
		if (!res->success)
		{
			dbg("  Failed, error code $l\n", (LONG)res->error_code);
			dp->dp_Res1 = total_written;
			dp->dp_Res2 = res->error_code;
			reply_packet(dp);
			return;
		}

		if (res->actual)
		{
			src += res->actual;
			total_written += res->actual;
			length -= res->actual;
		}

		if (res->actual < to_write)
			break;
	}

	dp->dp_Res1 = total_written;
	dp->dp_Res2 = 0;
	reply_packet(dp);
}

void action_seek(struct DosPacket *dp)
{
	ULONG arg1 = cur_file_key;
	LONG new_pos = dp->dp_Arg2;
	LONG mode = dp->dp_Arg3;

	dbg("ACTION_SEEK\n");
	dbg("  arg1 = $l\n", arg1);
	dbg("  new_pos = $l\n", new_pos);
	dbg("  mode = $l\n", mode);

	struct SeekRequest *req = (struct SeekRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->arg1 = arg1;
	req->new_pos = new_pos;
	req->mode = mode;

	write_req_and_wait_for_res(sizeof(struct SeekRequest));

	struct SeekResponse *res = (struct SeekResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = -1;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		dp->dp_Res1 = res->old_pos;
		dp->dp_Res2 = 0;
	}

	reply_packet(dp);
}

void action_end(struct DosPacket *dp)
{
	ULONG arg1 = cur_file_key;

	dbg("ACTION_END\n");
	dbg("  arg1 = $l\n", arg1);

	struct EndRequest *req = (struct EndRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->arg1 = arg1;

	write_req_and_wait_for_res(sizeof(struct EndRequest));

	//struct EndResponse *res = (struct EndResponse *)request_buffer;

	dp->dp_Res1 = DOSTRUE;
	dp->dp_Res2 = 0;
	reply_packet(dp);
}

void action_delete_object(struct DosPacket *dp)
{
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg1);
	unsigned char *name = (unsigned char *)BADDR(dp->dp_Arg2);

	dbg("ACTION_DELETE_OBJECT\n");
	dbg("  lock = $l\n", lock);
	dbg("  name = $S\n", name);

	struct DeleteObjectRequest *req = (struct DeleteObjectRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = lock == NULL ? 0 : lock->fl_Key;

	int nlen = *name;
	memcpy(req->name, name, nlen + 1);

	write_req_and_wait_for_res(sizeof(struct DeleteObjectRequest) + nlen);

	struct DeleteObjectResponse *res = (struct DeleteObjectResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		dp->dp_Res1 = DOSTRUE;
		dp->dp_Res2 = 0;
	}

	reply_packet(dp);
}

void action_rename_object(struct DosPacket *dp)
{
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg1);
	unsigned char *name = (unsigned char *)BADDR(dp->dp_Arg2);
	struct FileLock *target_dir = (struct FileLock *)BADDR(dp->dp_Arg3);
	unsigned char *new_name = (unsigned char *)BADDR(dp->dp_Arg4);

	dbg("ACTION_RENAME_OBJECT\n");
	dbg("  lock = $l\n", lock);
	dbg("  name = $S\n", name);
	dbg("  target directory = $l\n", lock);
	dbg("  new name = $S\n", new_name);

	struct RenameObjectRequest *req = (struct RenameObjectRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = lock == NULL ? 0 : lock->fl_Key;
	req->target_dir = target_dir == NULL ? 0 : target_dir->fl_Key;

	int nlen = *name;
	int nnlen = *new_name;

	req->name_len = nlen;
	req->new_name_len = nnlen;

	unsigned char *p = &(req->new_name_len) + 1;
	memcpy(p, name + 1, nlen);
	p += nlen;
	memcpy(p, new_name + 1, nnlen);

	write_req_and_wait_for_res(sizeof(struct RenameObjectRequest) + nlen + nnlen);

	struct RenameObjectResponse *res = (struct RenameObjectResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		dp->dp_Res1 = DOSTRUE;
		dp->dp_Res2 = 0;
	}

	reply_packet(dp);
}

void action_create_dir(struct DosPacket *dp)
{
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg1);
	unsigned char *name = (unsigned char *)BADDR(dp->dp_Arg2);

	dbg("ACTION_CREATE_DIR\n");
	dbg("  lock = $l\n", lock);
	dbg("  name = $S\n", name);

	struct CreateDirRequest *req = (struct CreateDirRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = lock == NULL ? 0 : lock->fl_Key;

	int nlen = *name;
	memcpy(req->name, name, nlen + 1);

	write_req_and_wait_for_res(sizeof(struct CreateDirRequest) + nlen);

	struct CreateDirResponse *res = (struct CreateDirResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		struct FileLock *lock = create_and_add_file_lock(res->key, SHARED_LOCK);

		dbg("  Returning lock $l\n", lock);
		dp->dp_Res1 = MKBADDR(lock);
		dp->dp_Res2 = lock ? 0 : ERROR_NO_FREE_STORE;
	}

	reply_packet(dp);
}

void action_set_protect(struct DosPacket *dp)
{
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg2);
	unsigned char *name = (unsigned char *)BADDR(dp->dp_Arg3);
	long mask = dp->dp_Arg4;

	dbg("ACTION_SET_PROTECT\n");
	dbg("  lock = $l\n", lock);
	dbg("  name = $S\n", name);
	dbg("  mask = $l\n", mask);

	struct SetProtectRequest *req = (struct SetProtectRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = lock == NULL ? 0 : lock->fl_Key;
	req->mask = mask;

	int nlen = *name;
	memcpy(req->name, name, nlen + 1);

	write_req_and_wait_for_res(sizeof(struct SetProtectRequest) + nlen);

	struct SetProtectResponse *res = (struct SetProtectResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		dp->dp_Res1 = DOSTRUE;
		dp->dp_Res2 = 0;
	}

	reply_packet(dp);
}

void action_set_comment(struct DosPacket *dp)
{
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg2);
	unsigned char *name = (unsigned char *)BADDR(dp->dp_Arg3);
	unsigned char *comment = (unsigned char *)BADDR(dp->dp_Arg4);

	dbg("ACTION_SET_COMMENT\n");
	dbg("  lock = $l\n", lock);
	dbg("  name = $S\n", name);
	dbg("  comment = $S\n", comment);

	struct SetCommentRequest *req = (struct SetCommentRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = lock == NULL ? 0 : lock->fl_Key;

	int nlen = *name;
	int clen = *comment;

	req->name_len = nlen;
	req->comment_len = clen;

	unsigned char *p = &(req->comment_len) + 1;
	memcpy(p, name + 1, nlen);
	p += nlen;
	memcpy(p, comment + 1, clen);

	write_req_and_wait_for_res(sizeof(struct SetCommentRequest) + nlen + clen);

	struct SetCommentResponse *res = (struct SetCommentResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
		dp->dp_Res1 = DOSFALSE;
		dp->dp_Res2 = res->error_code;
	}
	else
	{
		dp->dp_Res1 = DOSTRUE;
		dp->dp_Res2 = 0;
	}

	reply_packet(dp);
}

void action_same_lock(struct DosPacket *dp)
{
	struct FileLock *lock1 = (struct FileLock *)BADDR(dp->dp_Arg1);
	struct FileLock *lock2 = (struct FileLock *)BADDR(dp->dp_Arg2);

	dbg("ACTION_SAME_LOCK\n");
	dbg("  locks to compare = $l $l\n", lock1, lock2);

	struct SameLockRequest *req = (struct SameLockRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key1 = lock1->fl_Key;
	req->key2 = lock2->fl_Key;

	write_req_and_wait_for_res(sizeof(struct SameLockRequest));

	struct SameLockResponse *res = (struct SameLockResponse *)request_buffer;
	dp->dp_Res1 = res->success ? DOSTRUE : DOSFALSE;
	dp->dp_Res2 = res->error_code;

	reply_packet(dp);
}

void fill_info_data(struct InfoData *id)
{
	memset(id, 0, sizeof(struct InfoData));
	id->id_UnitNumber = unit;
	id->id_BytesPerBlock = 512;
	if (!my_volume)
	{
		id->id_DiskState = ID_VALIDATED;
		id->id_DiskType = ID_NO_DISK_PRESENT;
		return;
	}
	id->id_DiskState = volume_read_only ? ID_WRITE_PROTECTED : ID_VALIDATED;
	id->id_NumBlocks = 512 * 1024;
	id->id_NumBlocksUsed = 10;
	id->id_DiskType = ID_DOS_DISK;
	id->id_VolumeNode = MKBADDR(my_volume);
	id->id_InUse = my_volume->dl_Lock ? DOSTRUE : DOSFALSE;
}

void action_info(struct DosPacket *dp)
{
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg1);
	struct InfoData *id = (struct InfoData *)BADDR(dp->dp_Arg2);

	dbg("ACTION_INFO\n");
	dbg("  lock = $l\n", lock);

	struct InfoRequest *req = (struct InfoRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = lock == NULL ? 0 : lock->fl_Key;

	write_req_and_wait_for_res(sizeof(struct InfoRequest));
	fill_info_data(id);

	struct InfoResponse *res = (struct InfoResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
	}
	else
	{
		id->id_NumBlocks = res->total_blocks;
		id->id_NumBlocksUsed = res->used_blocks;
		id->id_BytesPerBlock = res->block_size;
	}

	dp->dp_Res1 = DOSTRUE;
	dp->dp_Res2 = 0;
	reply_packet(dp);
}

void action_disk_info(struct DosPacket *dp)
{
	struct InfoData *id = (struct InfoData *)BADDR(dp->dp_Arg1);

	dbg("ACTION_DISK_INFO\n");

	struct DiskInfoRequest *req = (struct DiskInfoRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;

	write_req_and_wait_for_res(sizeof(struct DiskInfoRequest));
	fill_info_data(id);

	struct DiskInfoResponse *res = (struct DiskInfoResponse *)request_buffer;
	if (!res->success)
	{
		dbg("  Failed, error code $l\n", (LONG)res->error_code);
	}
	else
	{
		id->id_NumBlocks = res->total_blocks;
		id->id_NumBlocksUsed = res->used_blocks;
		id->id_BytesPerBlock = res->block_size;
	}

	dp->dp_Res1 = DOSTRUE;
	dp->dp_Res2 = 0;
	reply_packet(dp);
}

void action_is_filesystem(struct DosPacket *dp)
{
	dbg("ACTION_IS_FILESYSTEM\n");

	dp->dp_Res1 = DOSTRUE;
	dp->dp_Res2 = 0;
	reply_packet(dp);
}

void action_unsupported(struct DosPacket *dp)
{
	dbg("ACTION_UNSUPPORTED\n");
	dbg("  Unsupported action: $l\n", (ULONG)dp->dp_Type);

	struct UnsupportedRequest *req = (struct UnsupportedRequest *)request_buffer;
	req->has_response = 0;
	req->type = ACTION_UNSUPPORTED;
	req->dp_Type = dp->dp_Type;

	write_req_and_wait_for_res(sizeof(struct UnsupportedRequest));

	struct UnsupportedResponse *res = (struct UnsupportedResponse *)request_buffer;
	dp->dp_Res1 = res->success ? DOSTRUE : DOSFALSE;
	dp->dp_Res2 = res->error_code;
	reply_packet(dp);
}

void action_set_date(struct DosPacket *dp)
{
	struct FileLock *lock = (struct FileLock *)BADDR(dp->dp_Arg2);
	unsigned char *name = (unsigned char *)BADDR(dp->dp_Arg3);
	struct DateStamp *ds = (struct DateStamp *)dp->dp_Arg4;

	struct UsbSetDateRequest *req = (struct UsbSetDateRequest *)request_buffer;
	req->has_response = 0;
	req->type = dp->dp_Type;
	req->key = lock == NULL ? 0 : lock->fl_Key;
	req->days = ds->ds_Days;
	req->minute = ds->ds_Minute;
	req->tick = ds->ds_Tick;

	int nlen = *name;
	memcpy(req->name, name, nlen + 1);

	write_req_and_wait_for_res(sizeof(struct UsbSetDateRequest) + nlen);

	short *res = (short *)request_buffer;
	dp->dp_Res1 = res[1] ? DOSTRUE : DOSFALSE;
	dp->dp_Res2 = res[1] ? 0 : res[2];
	reply_packet(dp);
}

// Whether a lock is on the stick that is in the unit: 0 if so, else the
// error for the packet (no stick, or the lock's stick is out)
LONG lock_error(BPTR lock)
{
	if (!lock)
		return my_volume ? 0 : ERROR_NO_DISK;
	if (my_volume && ((struct FileLock *)BADDR(lock))->fl_Volume == MKBADDR(my_volume))
		return 0;
	return ERROR_DEVICE_NOT_MOUNTED;
}

// Answers a packet that can't be handled now; FALSE if it can be
BOOL refused(struct DosPacket *dp)
{
	LONG err = 0;

	switch (dp->dp_Type)
	{
	case ACTION_LOCATE_OBJECT: case ACTION_COPY_DIR: case ACTION_PARENT:
	case ACTION_EXAMINE_OBJECT: case ACTION_EXAMINE_NEXT: case ACTION_DELETE_OBJECT:
	case ACTION_CREATE_DIR: case ACTION_INFO:
		err = lock_error(dp->dp_Arg1);
		break;
	case ACTION_FINDINPUT: case ACTION_FINDOUTPUT: case ACTION_FINDUPDATE:
	case ACTION_SET_PROTECT: case ACTION_SET_COMMENT: case ACTION_SET_DATE:
		err = lock_error(dp->dp_Arg2);
		break;
	case ACTION_RENAME_OBJECT:
		err = lock_error(dp->dp_Arg1);
		if (!err)
			err = lock_error(dp->dp_Arg3);
		break;
	case ACTION_SAME_LOCK:
		err = lock_error(dp->dp_Arg1);
		if (!err)
			err = lock_error(dp->dp_Arg2);
		break;
	case ACTION_READ: case ACTION_WRITE: case ACTION_SEEK: case ACTION_EXAMINE_FH:
	{
		// The Pi's key, if the file's stick is still in (this insertion)
		struct UsbFile *uf = (struct UsbFile *)dp->dp_Arg1;
		if (!my_volume || uf->insertion != insertion)
			err = ERROR_DEVICE_NOT_MOUNTED;
		else
			cur_file_key = uf->key;
		break;
	}
	case ACTION_END:
	{
		struct UsbFile *uf = (struct UsbFile *)dp->dp_Arg1;
		BOOL here = my_volume && uf->insertion == insertion;
		cur_file_key = uf->key;
		FreeMem(uf, sizeof(struct UsbFile));
		if (!here)
		{
			// The file went with the stick: closing it is fine
			dp->dp_Res1 = DOSTRUE;
			dp->dp_Res2 = 0;
			reply_packet(dp);
			return TRUE;
		}
		break;
	}
	}
	if (!err)
		return FALSE;
	dp->dp_Res1 = (dp->dp_Type == ACTION_READ || dp->dp_Type == ACTION_WRITE ||
		dp->dp_Type == ACTION_SEEK) ? -1 : DOSFALSE;
	dp->dp_Res2 = err;
	reply_packet(dp);
	return TRUE;
}

void reply_simple(struct DosPacket *dp, LONG res1, LONG res2)
{
	dp->dp_Res1 = res1;
	dp->dp_Res2 = res2;
	reply_packet(dp);
}

void action_eject(struct DosPacket *dp)
{
	if (!my_volume)
	{
		reply_simple(dp, DOSFALSE, ERROR_NO_DISK);
		return;
	}
	struct UsbStatusRequest *req = (struct UsbStatusRequest *)request_buffer;
	req->has_response = 0;
	req->type = ACTION_USB_EJECT;
	write_req_and_wait_for_res(sizeof(struct UsbStatusRequest));

	short *res = (short *)request_buffer;
	if (!res[1])
	{
		reply_simple(dp, DOSFALSE, res[2]);
		return;
	}
	remove_volume();	// (if the list is busy, the next poll does it)
	reply_simple(dp, DOSTRUE, 0);
}

void start(__reg("a0") struct DosPacket *startup_packet)
{
	SysBase = *(struct ExecBase **)4;
	DOSBase = (struct DosLibrary *)OpenLibrary(DOSNAME, 0);

	mp = MyCreatePort(NULL, 0);

	startup_fs_handler(startup_packet);

	ULONG packet_sig = 1UL << mp->mp_SigBit;
	ULONG timer_sig = timer_mp ? 1UL << timer_mp->mp_SigBit : 0;

	while (1)
	{
		ULONG sigs = Wait(packet_sig | timer_sig);

		if ((sigs & timer_sig) && GetMsg(timer_mp))
		{
			timer_pending = FALSE;
			poll_unit();
			start_timer();
		}

		struct StandardPacket *sp;
		while ((sp = (struct StandardPacket *)GetMsg(mp)))
		{
		struct DosPacket *dp = (struct DosPacket *)(sp->sp_Msg.mn_Node.ln_Name);

		if (refused(dp))
			continue;

		switch (dp->dp_Type)
		{
		case ACTION_CURRENT_VOLUME: reply_simple(dp, MKBADDR(my_volume), 0); break;
		case ACTION_FLUSH: reply_simple(dp, DOSTRUE, 0); break;
		case ACTION_INHIBIT: reply_simple(dp, DOSTRUE, 0); break;
		case ACTION_DISK_CHANGE: poll_unit(); reply_simple(dp, DOSTRUE, 0); break;
		case ACTION_USB_EJECT: action_eject(dp); break;
		case ACTION_LOCATE_OBJECT: action_locate_object(dp); break;
		case ACTION_FREE_LOCK: action_free_lock(dp); break;
		case ACTION_COPY_DIR: action_copy_dir(dp); break;
		case ACTION_PARENT: action_parent(dp); break;
		case ACTION_EXAMINE_OBJECT: action_examine_object(dp); break;
		case ACTION_EXAMINE_NEXT: action_examine_next(dp); break;
		case ACTION_EXAMINE_FH: action_examine_fh(dp); break;

		case ACTION_FINDUPDATE: action_findxxx(dp); break;
		case ACTION_FINDINPUT: action_findxxx(dp); break;
		case ACTION_FINDOUTPUT: action_findxxx(dp); break;
		case ACTION_READ: action_read(dp); break;
		case ACTION_WRITE: action_write(dp); break;
		case ACTION_SEEK: action_seek(dp); break;
		case ACTION_END: action_end(dp); break;
		//case ACTION_TRUNCATE: action_truncate(dp); break;

		case ACTION_DELETE_OBJECT: action_delete_object(dp); break;
		case ACTION_RENAME_OBJECT: action_rename_object(dp); break;
		case ACTION_CREATE_DIR: action_create_dir(dp); break;

		case ACTION_SET_PROTECT: action_set_protect(dp); break;
		case ACTION_SET_COMMENT: action_set_comment(dp); break;
		case ACTION_SET_DATE: action_set_date(dp); break;

		case ACTION_SAME_LOCK: action_same_lock(dp); break;
		case ACTION_DISK_INFO: action_disk_info(dp); break;
		case ACTION_INFO: action_info(dp); break;

		case ACTION_IS_FILESYSTEM: action_is_filesystem(dp); break;

		/*
		case ACTION_CURRENT_VOLUME: action_current_volume(dp); break;
		case ACTION_RENAME_DISK: action_rename_disk(dp); break;

		case ACTION_DIE: //action_die(dp); break;
		case ACTION_MORE_CACHE: //action_more_cache(dp); break;
		case ACTION_FLUSH: //action_flush(dp); break;
		case ACTION_INHIBIT: //action_inhibit(dp); break;
		case ACTION_WRITE_PROTECT: //action_write_protect(dp); break;
		*/

		default: action_unsupported(dp); break;
		}
		}
	}

	dbg("Shutting down\n");

	// More cleaning up is necessary if the handler is to exit.
	CloseLibrary((struct Library *)DOSBase);
}
