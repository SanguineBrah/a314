#include <exec/types.h>
#include <exec/memory.h>

#include <libraries/dos.h>

#include <devices/audio.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include <proto/alib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/icon.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../a314device/a314.h"
#include "../a314device/proto_a314.h"

#define SERVICE_NAME "piaudio"

#define FREQ 18000
#define BUFFER_LEN_MS 50
#define SAMPLES (FREQ * BUFFER_LEN_MS / 1000)

/* Paula's channels 0 and 3 are on the left output, 1 and 2 on the right */
#define L0		1
#define R0		2
#define R1		4
#define L1		8

#define LEFT_CHAN_MASK	(L0 | L1)
#define RIGHT_CHAN_MASK	(R0 | R1)

#define LEFT 0
#define RIGHT 1

/*
 * Optional AHI output (piaudio AHI). AHI mixes PiAudio with other
 * programs that play through AHI, instead of PiAudio taking two Paula
 * channels for itself. These definitions are from devices/ahi.h of the
 * AHI developer kit (version 4), so that building doesn't need it.
 */
#define AHINAME		"ahi.device"
#define AHI_DEFAULT_UNIT	0
#define AHIST_S8S	2	/* stereo, 8 bit signed */

struct AHIRequest
{
	struct IOStdReq ahir_Std;
	UWORD ahir_Version;
	UWORD ahir_Pad1;
	ULONG ahir_Private[2];
	ULONG ahir_Type;
	ULONG ahir_Frequency;
	LONG ahir_Volume;		/* fixed point, 0x10000 is full volume */
	LONG ahir_Position;		/* fixed point, 0x8000 is the middle */
	struct AHIRequest *ahir_Link;
};

struct MsgPort *sync_mp = NULL;
struct MsgPort *async_mp = NULL;

struct A314_IORequest *sync_a314_req = NULL;
struct A314_IORequest *write_a314_req = NULL;

struct Library *A314Base;

char *audio_buffers[4] = { NULL, NULL, NULL, NULL };
ULONG a314_buffers[2] = { INVALID_A314_ADDRESS, INVALID_A314_ADDRESS };

struct IOAudio *sync_audio_req = NULL;
struct IOAudio *async_audio_req[4] = { NULL, NULL, NULL, NULL };

ULONG allocated_channels;

struct IntuitionBase *IntuitionBase = NULL;
struct Library *IconBase = NULL;

/* Set by the startup code when started from Workbench */
extern struct WBStartup *_WBenchMsg;

#define PORT_NAME "PiAudio"	/* marks that PiAudio is running */
struct MsgPort *marker_port = NULL;

BOOL use_ahi = FALSE;
BOOL ahi_device_open = FALSE;
struct AHIRequest *ahi_req[2] = { NULL, NULL };
BOOL ahi_pending[2] = { FALSE, FALSE };
char *ahi_buffers[2] = { NULL, NULL };	/* interleaved stereo */

BOOL a314_device_open = FALSE;
BOOL audio_device_open = FALSE;
BOOL stream_open = FALSE;
BOOL pending_a314_write = FALSE;
BOOL has_a314_chip_memory = FALSE;

ULONG socket;
int back_index = 0;
char awbuf[8];

void start_a314_cmd(struct MsgPort *reply_port, struct A314_IORequest *ior, UWORD cmd, char *buffer, int length)
{
	ior->a314_Request.io_Message.mn_ReplyPort = reply_port;
	ior->a314_Request.io_Command = cmd;
	ior->a314_Request.io_Error = 0;
	ior->a314_Socket = socket;
	ior->a314_Buffer = buffer;
	ior->a314_Length = length;
	SendIO((struct IORequest *)ior);
}

BYTE a314_connect(char *name)
{
	socket = time(NULL);
	start_a314_cmd(sync_mp, sync_a314_req, A314_CONNECT, name, strlen(name));
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_a314_req->a314_Request.io_Error;
}

BYTE a314_write(char *buffer, int length)
{
	start_a314_cmd(sync_mp, sync_a314_req, A314_WRITE, buffer, length);
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_a314_req->a314_Request.io_Error;
}

BYTE a314_eos()
{
	start_a314_cmd(sync_mp, sync_a314_req, A314_EOS, NULL, 0);
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_a314_req->a314_Request.io_Error;
}

BYTE a314_reset()
{
	start_a314_cmd(sync_mp, sync_a314_req, A314_RESET, NULL, 0);
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_a314_req->a314_Request.io_Error;
}

void start_a314_write(char *buffer, int length)
{
	start_a314_cmd(async_mp, write_a314_req, A314_WRITE, buffer, length);
	pending_a314_write = TRUE;
}

void submit_async_audio_req(int index)
{
	ULONG mask = ((index & 1) == LEFT) ? LEFT_CHAN_MASK : RIGHT_CHAN_MASK;
	ULONG unit = allocated_channels & mask;

	async_audio_req[index]->ioa_Request.io_Message.mn_ReplyPort = async_mp;
	async_audio_req[index]->ioa_Request.io_Command = CMD_WRITE;
	async_audio_req[index]->ioa_Request.io_Flags = ADIOF_PERVOL;
	async_audio_req[index]->ioa_Request.io_Unit = (void*)unit;
	async_audio_req[index]->ioa_Data = audio_buffers[index];
	async_audio_req[index]->ioa_Length = SAMPLES;
	async_audio_req[index]->ioa_Period = 197;
	async_audio_req[index]->ioa_Volume = 64;
	async_audio_req[index]->ioa_Cycles = 1;
	BeginIO((struct IORequest *)async_audio_req[index]);
}

/* Shows a message in the shell, or in a requester when started from
   Workbench (where there is no shell). With no, asks a question and
   returns TRUE for yes. */
BOOL show(const char *text, const char *yes, const char *no)
{
	struct IntuiText body = { 0, 1, JAM2, 16, 8, NULL, (UBYTE *)text, NULL };
	struct IntuiText pos = { 0, 1, JAM2, 6, 3, NULL, (UBYTE *)yes, NULL };
	struct IntuiText neg = { 0, 1, JAM2, 6, 3, NULL, (UBYTE *)no, NULL };

	if (!_WBenchMsg)
	{
		printf("%s\n", text);
		return FALSE;
	}
	if (!IntuitionBase)
		IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 0);
	if (!IntuitionBase)
		return FALSE;
	return AutoRequest(NULL, &body, no ? &pos : NULL, no ? &neg : &pos, 0, 0,
		8 * strlen(text) + 48, 54);
}

void report(const char *text)
{
	show(text, "OK", NULL);
}

/* Reads the options from the icon's ToolTypes, when started from
   Workbench: AHI selects AHI output. The icon is the last argument: the
   program's own icon, or a project icon (such as Tools/PiAudio) whose
   default tool is C:PiAudio. */
void read_tooltypes()
{
	struct WBArg *arg = &_WBenchMsg->sm_ArgList[_WBenchMsg->sm_NumArgs - 1];
	struct DiskObject *icon;
	BPTR old_dir;

	IconBase = OpenLibrary("icon.library", 0);
	if (!IconBase)
		return;
	old_dir = CurrentDir(arg->wa_Lock);
	icon = GetDiskObject(arg->wa_Name);
	if (icon)
	{
		if (FindToolType((STRPTR *)icon->do_ToolTypes, (STRPTR)"AHI"))
			use_ahi = TRUE;
		FreeDiskObject(icon);
	}
	CurrentDir(old_dir);
}

/* Plays buffer index (0 or 2) through AHI. AHI wants the left and right
   samples interleaved, so they are copied into ahi_buffers first. */
void submit_ahi_req(int index)
{
	int slot = index >> 1, other = slot ^ 1, i;
	char *left = audio_buffers[index + LEFT];
	char *right = audio_buffers[index + RIGHT];
	char *dst = ahi_buffers[slot];
	struct AHIRequest *req = ahi_req[slot];

	for (i = 0; i < SAMPLES; i++)
	{
		*dst++ = left[i];
		*dst++ = right[i];
	}

	req->ahir_Std.io_Message.mn_ReplyPort = async_mp;
	req->ahir_Std.io_Command = CMD_WRITE;
	req->ahir_Std.io_Data = ahi_buffers[slot];
	req->ahir_Std.io_Length = SAMPLES * 2;
	req->ahir_Std.io_Offset = 0;
	req->ahir_Type = AHIST_S8S;
	req->ahir_Frequency = FREQ;
	req->ahir_Volume = 0x10000;
	req->ahir_Position = 0x8000;
	/* Linking to the request that is playing makes AHI play this one
	   straight after it, without a gap */
	req->ahir_Link = ahi_pending[other] ? ahi_req[other] : NULL;
	SendIO((struct IORequest *)req);
	ahi_pending[slot] = TRUE;
}

/* Allocates a left and a right Paula channel through audio.device */
BOOL open_paula()
{
	int i;

	sync_audio_req = (struct IOAudio *)CreateExtIO(sync_mp, sizeof(struct IOAudio));
	if (!sync_audio_req)
	{
		report("Unable to allocate sync audio request");
		return FALSE;
	}

	for (i = 0; i < 4; i++)
	{
		async_audio_req[i] = AllocMem(sizeof(struct IOAudio), MEMF_PUBLIC);
		if (!async_audio_req[i])
		{
			report("Unable to allocate async audio request");
			return FALSE;
		}
	}

	UBYTE which_channels[] = { L0 | R0, L0 | R1, L1 | R0, L1 | R1 };

	sync_audio_req->ioa_Request.io_Message.mn_ReplyPort = sync_mp;
	sync_audio_req->ioa_Request.io_Message.mn_Node.ln_Pri = 127;
	sync_audio_req->ioa_Request.io_Command = ADCMD_ALLOCATE;
	sync_audio_req->ioa_Request.io_Flags = ADIOF_NOWAIT;
	sync_audio_req->ioa_AllocKey = 0;
	sync_audio_req->ioa_Data = which_channels;
	sync_audio_req->ioa_Length = sizeof(which_channels);

	if (OpenDevice(AUDIONAME, 0, (struct IORequest *)sync_audio_req, 0))
	{
		report("Unable to open audio.device");
		return FALSE;
	}

	audio_device_open = TRUE;

	allocated_channels = (ULONG)sync_audio_req->ioa_Request.io_Unit;

	for (i = 0; i < 4; i++)
		memcpy(async_audio_req[i], sync_audio_req, sizeof(struct IOAudio));

	return TRUE;
}

/* Opens ahi.device instead of allocating Paula channels */
BOOL open_ahi()
{
	int i;

	for (i = 0; i < 2; i++)
	{
		ahi_req[i] = (struct AHIRequest *)CreateExtIO(async_mp, sizeof(struct AHIRequest));
		ahi_buffers[i] = AllocMem(SAMPLES * 2, MEMF_PUBLIC | MEMF_CLEAR);
		if (!ahi_req[i] || !ahi_buffers[i])
		{
			report("Unable to allocate memory for AHI");
			return FALSE;
		}
	}

	ahi_req[0]->ahir_Version = 4;
	if (OpenDevice(AHINAME, AHI_DEFAULT_UNIT, (struct IORequest *)ahi_req[0], 0))
	{
		report("Unable to open ahi.device (AHI needs Kickstart 2.04 or later)");
		return FALSE;
	}
	ahi_device_open = TRUE;
	memcpy(ahi_req[1], ahi_req[0], sizeof(struct AHIRequest));
	return TRUE;
}

int main(int argc, char **argv)
{
	LONG old_priority = SetTaskPri(FindTask(NULL), 50);

	if (_WBenchMsg)
		read_tooltypes();
	else if (argc == 2 && (strcmp(argv[1], "AHI") == 0 || strcmp(argv[1], "ahi") == 0))
		use_ahi = TRUE;
	else if (argc > 1)
	{
		report("Usage: piaudio [AHI]");
		return 10;
	}

	/* Only one PiAudio at a time. From Workbench, starting it again
	   offers to stop the one that is running. */
	struct MsgPort *other;
	Forbid();
	other = FindPort(PORT_NAME);
	if (!other)
		marker_port = CreatePort(PORT_NAME, 0);
	Permit();
	if (other)
	{
		if (!_WBenchMsg)
			printf("PiAudio is already running\n");
		else if (show("PiAudio is already running. Stop it?", "Stop", "Cancel"))
		{
			Forbid();
			other = FindPort(PORT_NAME);
			if (other)
				Signal(other->mp_SigTask, SIGBREAKF_CTRL_C);
			Permit();
		}
		goto cleanup;
	}

	sync_mp = CreatePort(NULL, 0);
	if (!sync_mp)
	{
		report("Unable to create sync reply message port");
		goto cleanup;
	}

	async_mp = CreatePort(NULL, 0);
	if (!async_mp)
	{
		report("Unable to create async reply message port");
		goto cleanup;
	}

	sync_a314_req = (struct A314_IORequest *)CreateExtIO(sync_mp, sizeof(struct A314_IORequest));
	write_a314_req = (struct A314_IORequest *)CreateExtIO(sync_mp, sizeof(struct A314_IORequest));
	if (!sync_a314_req || !write_a314_req)
	{
		report("Unable to create A314_IORequest");
		goto cleanup;
	}

	if (OpenDevice(A314_NAME, 0, (struct IORequest *)sync_a314_req, 0))
	{
		report("Unable to open a314.device");
		goto cleanup;
	}

	a314_device_open = TRUE;

	A314Base = &(sync_a314_req->a314_Request.io_Device->dd_Library);

	memcpy(write_a314_req, sync_a314_req, sizeof(struct A314_IORequest));

	audio_buffers[0] = AllocMem(SAMPLES * 2, MEMF_A314 | MEMF_CHIP | MEMF_CLEAR);
	if (audio_buffers[0])
	{
		audio_buffers[2] = AllocMem(SAMPLES * 2, MEMF_A314 | MEMF_CHIP | MEMF_CLEAR);
		if (!audio_buffers[2])
		{
			report("Unable to allocate audio buffers in A314 chip memory");
			goto cleanup;
		}
		has_a314_chip_memory = TRUE;
	}
	else
	{
		audio_buffers[0] = AllocMem(SAMPLES * 2, MEMF_CHIP | MEMF_CLEAR);
		audio_buffers[2] = AllocMem(SAMPLES * 2, MEMF_CHIP | MEMF_CLEAR);
		if (!audio_buffers[0] || !audio_buffers[2])
		{
			report("Unable to allocate audio buffers in chip memory");
			goto cleanup;
		}

		a314_buffers[0] = AllocMemA314(SAMPLES * 2);
		a314_buffers[1] = AllocMemA314(SAMPLES * 2);
		if (a314_buffers[0] == INVALID_A314_ADDRESS ||
			a314_buffers[1] == INVALID_A314_ADDRESS)
		{
			report("Unable to allocate buffers in A314 memory");
			goto cleanup;
		}

		WriteMemA314(a314_buffers[0], audio_buffers[0], SAMPLES * 2);
		WriteMemA314(a314_buffers[1], audio_buffers[2], SAMPLES * 2);
	}

	audio_buffers[1] = audio_buffers[0] + SAMPLES;
	audio_buffers[3] = audio_buffers[2] + SAMPLES;

	int i;

	if (use_ahi ? !open_ahi() : !open_paula())
		goto cleanup;

	if (a314_connect(SERVICE_NAME) != A314_CONNECT_OK)
	{
		report("Unable to connect to piaudio service");
		goto cleanup;
	}

	stream_open = TRUE;

	ULONG *buf_ptrs = (ULONG *)awbuf;
	if (has_a314_chip_memory)
	{
		buf_ptrs[0] = TranslateAddressA314(audio_buffers[0]);
		buf_ptrs[1] = TranslateAddressA314(audio_buffers[2]);
	}
	else
	{
		buf_ptrs[0] = a314_buffers[0];
		buf_ptrs[1] = a314_buffers[1];
	}

	if (a314_write(awbuf, 8) != A314_WRITE_OK)
	{
		report("Unable to write buffer pointers");
		goto cleanup;
	}

	/* Requests per buffer: one for AHI, a left and a right one for Paula */
	int reqs_per_buffer = use_ahi ? 1 : 2;

	if (use_ahi)
	{
		if (!_WBenchMsg)
			printf("PiAudio started, playing through AHI\n");
		submit_ahi_req(back_index);
	}
	else
	{
		if (!_WBenchMsg)
			printf("PiAudio started, allocated channels: L%d, R%d\n",
				(allocated_channels & LEFT_CHAN_MASK) == L0 ? 0 : 1,
				(allocated_channels & RIGHT_CHAN_MASK) == R0 ? 0 : 1);

		sync_audio_req->ioa_Request.io_Command = CMD_STOP;
		DoIO((struct IORequest *)sync_audio_req);

		submit_async_audio_req(back_index + LEFT);
		submit_async_audio_req(back_index + RIGHT);

		sync_audio_req->ioa_Request.io_Command = CMD_START;
		DoIO((struct IORequest *)sync_audio_req);
	}

	int pending_audio_reqs = reqs_per_buffer;

	ULONG portsig = 1L << async_mp->mp_SigBit;

	if (!_WBenchMsg)
		printf("Press ctrl-c to exit...\n");

	while (TRUE)
	{
		if (pending_audio_reqs <= reqs_per_buffer)
		{
			back_index ^= 2;

			if (!has_a314_chip_memory)
			{
				char *dst = audio_buffers[back_index];
				ULONG address = a314_buffers[back_index >> 1];
				ReadMemA314(dst, address, SAMPLES * 2);
			}

			if (use_ahi)
				submit_ahi_req(back_index);
			else
			{
				submit_async_audio_req(back_index + LEFT);
				submit_async_audio_req(back_index + RIGHT);
			}

			pending_audio_reqs += reqs_per_buffer;

			if (!pending_a314_write)
			{
				awbuf[0] = back_index == 0 ? 0 : 1;
				start_a314_write(awbuf, 1);
			}
		}

		ULONG signal = Wait(SIGBREAKF_CTRL_C | portsig);

		if (signal & SIGBREAKF_CTRL_C)
			break;
		else if (signal & portsig)
		{
			struct Message *msg;
			while (msg = GetMsg(async_mp))
			{
				if (msg == (struct Message *)write_a314_req)
				{
					if (write_a314_req->a314_Request.io_Error == A314_WRITE_OK)
						pending_a314_write = FALSE;
					else
						goto cleanup;
				}
				else
				{
					if (msg == (struct Message *)ahi_req[0])
						ahi_pending[0] = FALSE;
					else if (msg == (struct Message *)ahi_req[1])
						ahi_pending[1] = FALSE;
					pending_audio_reqs--;
				}
			}
		}
	}

cleanup:
	if (stream_open)
		a314_reset();
	for (i = 0; i < 2; i++)
	{
		if (ahi_pending[i])
		{
			AbortIO((struct IORequest *)ahi_req[i]);
			WaitIO((struct IORequest *)ahi_req[i]);
		}
	}
	if (ahi_device_open)
		CloseDevice((struct IORequest *)ahi_req[0]);
	for (i = 0; i < 2; i++)
	{
		if (ahi_req[i])
			DeleteExtIO((struct IORequest *)ahi_req[i]);
		if (ahi_buffers[i])
			FreeMem(ahi_buffers[i], SAMPLES * 2);
	}
	if (audio_device_open)
		CloseDevice((struct IORequest *)sync_audio_req);
	for (i = 3; i >= 0; i--)
		if (async_audio_req[i])
			FreeMem(async_audio_req[i], sizeof(struct IOAudio));
	if (sync_audio_req)
		DeleteExtIO((struct IORequest *)sync_audio_req);
	if (a314_buffers[1] != INVALID_A314_ADDRESS)
		FreeMemA314(a314_buffers[1], SAMPLES * 2);
	if (a314_buffers[0] != INVALID_A314_ADDRESS)
		FreeMemA314(a314_buffers[0], SAMPLES * 2);
	if (audio_buffers[2])
		FreeMem(audio_buffers[2], SAMPLES * 2);
	if (audio_buffers[0])
		FreeMem(audio_buffers[0], SAMPLES * 2);
	if (a314_device_open)
		CloseDevice((struct IORequest *)sync_a314_req);
	if (write_a314_req)
		DeleteExtIO((struct IORequest *)write_a314_req);
	if (sync_a314_req)
		DeleteExtIO((struct IORequest *)sync_a314_req);
	if (async_mp)
		DeletePort(async_mp);
	if (sync_mp)
		DeletePort(sync_mp);
	if (marker_port)
		DeletePort(marker_port);
	if (IconBase)
		CloseLibrary(IconBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	SetTaskPri(FindTask(NULL), old_priority);

	return 0;
}
