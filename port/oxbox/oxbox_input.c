/*
OXBOX_INPUT.C

A scripted controller for testing in an emulator without a bound gamepad: d:\oxbox_input.txt holds lines
"<seconds> <button> [<hold seconds>]" (seconds since start-up; buttons: up down left right start back a b x y
black white lt rt). With the file, a virtual gamepad is plugged into port 1 (it shows up in XGetDeviceChanges,
opens with XInputOpen) and plays the script; without it, the controllers are read as they are.
*/

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DWORD WINAPI retail_XInputGetState(HANDLE device, PXINPUT_STATE state);
DWORD WINAPI retail_XInputSetState(HANDLE device, struct _XINPUT_FEEDBACK *feedback);
void WINAPI retail_XInputClose(HANDLE device);

/* the virtual gamepad's handle (any address no real handle can have) */
static char virtual_pad;
#define VIRTUAL_PAD ((HANDLE)&virtual_pad)
static int virtual_pad_announced;

struct scripted_press
{
	unsigned long start, end;     /* milliseconds since start-up */
	unsigned short buttons;       /* digital (wButtons) */
	short analog;                 /* index into bAnalogButtons, or -1 */
};

static struct scripted_press presses[256];

/* presses a debugger asks for (oxemu/pad.sh writes them through xemu's gdb stub): a new sequence number starts
a press of `buttons` (wButtons), `analog` (a mask of bAnalogButtons) and the sticks for hold_ms */
struct oxbox_remote_press
{
	volatile unsigned long sequence, buttons, analog, hold_ms;
	volatile short thumb_lx, thumb_ly, thumb_rx, thumb_ry;
};
struct oxbox_remote_press oxbox_remote;
static unsigned long remote_sequence, remote_end;
static int press_count, script_loaded, script_loaded_with_file;
static unsigned long start_ticks, packet;

static void script_load(void)
{
	static const struct { const char *name; unsigned short buttons; short analog; } names[] =
	{
		{ "up", 0x1, -1 }, { "down", 0x2, -1 }, { "left", 0x4, -1 }, { "right", 0x8, -1 },
		{ "start", 0x10, -1 }, { "back", 0x20, -1 }, { "a", 0, 0 }, { "b", 0, 1 }, { "x", 0, 2 }, { "y", 0, 3 },
		{ "black", 0, 4 }, { "white", 0, 5 }, { "lt", 0, 6 }, { "rt", 0, 7 },
	};
	FILE *file;
	char line[128], button[32];
	float at, hold;
	unsigned int i;

	script_loaded = 1;
	start_ticks = GetTickCount();
	file = fopen("d:\\oxbox_input.txt", "r");
	if (!file)
		return;
	script_loaded_with_file = 1;
	while (press_count < 256 && fgets(line, sizeof(line), file))
	{
		hold = 0.25f;
		if (sscanf(line, "%f %31s %f", &at, button, &hold) < 2)
			continue;
		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		{
			if (!strcmp(button, names[i].name))
			{
				presses[press_count].start = (unsigned long)(at * 1000.0f);
				presses[press_count].end = (unsigned long)((at + hold) * 1000.0f);
				presses[press_count].buttons = names[i].buttons;
				presses[press_count].analog = names[i].analog;
				press_count++;
				break;
			}
		}
	}
	fclose(file);
	platform_log("input script: %d presses", press_count);
}

/* port 0's insertion, once, when a script plays */
unsigned long oxbox_virtual_pad_insertions(void)
{
	if (!script_loaded)
		script_load();
	if (!script_loaded_with_file || virtual_pad_announced)
		return 0;
	virtual_pad_announced = 1;
	platform_log("virtual gamepad plugged into port 1");
	return 1;
}

HANDLE oxbox_virtual_pad_open(DWORD port)
{
	return script_loaded_with_file && port == 0 ? VIRTUAL_PAD : NULL;
}

void WINAPI XInputClose(HANDLE device)
{
	if (device != VIRTUAL_PAD)
		retail_XInputClose(device);
}

DWORD WINAPI XInputSetState(HANDLE device, struct _XINPUT_FEEDBACK *feedback)
{
	if (device == VIRTUAL_PAD)
		return ERROR_SUCCESS;
	return retail_XInputSetState(device, feedback);
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	DWORD result;
	unsigned long now;
	int i, active = 0;
	{   /* (memory measurement: every 10 s) */
		extern unsigned long oxbox_heap_live, oxbox_heap_peak, oxbox_heap_blocks, oxbox_system_live, oxbox_system_peak;
		extern unsigned long oxbox_text_cache_misses;
		extern unsigned long oxbox_texture_reads, oxbox_texture_read_bytes, oxbox_sound_reads;
		static unsigned long last_memory_report, texture_reads_before, texture_bytes_before, sound_reads_before;
		unsigned long t = GetTickCount();
		if (t - last_memory_report >= 10000)
		{
			MEMORYSTATUS memory;
			last_memory_report = t;
			GlobalMemoryStatus(&memory);
			/* (test build 24: and the textures and sounds read into their caches in those 10 seconds: many textures read
			over and over means the texture cache is too small for what's in view) */
			platform_log("mem %lus: free %lu KB, C heap %lu KB (peak %lu KB, %lu blocks), game heap %lu KB (peak %lu KB), text cache misses %lu; "
				"textures read %lu (%lu KB), sounds read %lu", t / 1000,
				memory.dwAvailPhys / 1024, oxbox_heap_live / 1024, oxbox_heap_peak / 1024, oxbox_heap_blocks,
				oxbox_system_live / 1024, oxbox_system_peak / 1024, oxbox_text_cache_misses,
				oxbox_texture_reads - texture_reads_before, (oxbox_texture_read_bytes - texture_bytes_before) / 1024,
				oxbox_sound_reads - sound_reads_before);
			oxbox_text_cache_misses = 0;
			texture_reads_before = oxbox_texture_reads;
			texture_bytes_before = oxbox_texture_read_bytes;
			sound_reads_before = oxbox_sound_reads;
#if defined(OXBOX_MEMPROF)
			{
				/* (the allocations again at each new low, down 32 KB or more, a few times) */
				void oxbox_memprof_report(const char *label);
				static unsigned long lowest = 0xFFFFFFFF, reports;
				extern volatile int oxbox_memprof_request;
				if (oxbox_memprof_request)
				{
					oxbox_memprof_request = 0;
					oxbox_memprof_report("now");
				}
				else if (memory.dwAvailPhys + 32768 <= lowest && reports < 8)
				{
					char label[16];
					lowest = memory.dwAvailPhys;
					reports++;
					sprintf(label, "%lus", t / 1000);
					oxbox_memprof_report(label);
				}
			}
#endif
		}
	}

	if (device != VIRTUAL_PAD)
		return retail_XInputGetState(device, state);
	memset(state, 0, sizeof(*state));
	state->dwPacketNumber = packet;
	result = ERROR_SUCCESS;
	now = GetTickCount() - start_ticks;
	for (i = 0; i < press_count; i++)
	{
		if (now >= presses[i].start && now < presses[i].end)
		{
			state->Gamepad.wButtons |= presses[i].buttons;
			if (presses[i].analog >= 0)
				state->Gamepad.bAnalogButtons[presses[i].analog] = 0xFF;
			active = 1;
		}
	}
	if (oxbox_remote.sequence != remote_sequence)
	{
		remote_sequence = oxbox_remote.sequence;
		remote_end = now + oxbox_remote.hold_ms;
	}
	if (now < remote_end)
	{
		state->Gamepad.wButtons |= (unsigned short)oxbox_remote.buttons;
		for (i = 0; i < 8; i++)
			if (oxbox_remote.analog & (1 << i))
				state->Gamepad.bAnalogButtons[i] = 0xFF;
		state->Gamepad.sThumbLX = oxbox_remote.thumb_lx;
		state->Gamepad.sThumbLY = oxbox_remote.thumb_ly;
		state->Gamepad.sThumbRX = oxbox_remote.thumb_rx;
		state->Gamepad.sThumbRY = oxbox_remote.thumb_ry;
		active = 1;
	}
	if (active)
		state->dwPacketNumber = ++packet;
	return result;
}
