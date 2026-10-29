/*
OXBOX_SDK.C

The SDK functions the game calls that the retail XBE does not contain (the January build had them; retail
Halo did not link them), or that need care here:
- a few Win32 calls, written over the ones the retail XAPI has,
- D3DX's matrix helpers (D3DX is mostly static code the retail game left out),
- the debug monitor (XBDM) and debug keyboard, which a retail console does not have,
- the input calls, which must not see the debug keyboard's device type (the retail input library has no debug
  keyboard: XInitDevices and the rest get only the types it knows),
- Bink's summary calls (debug statistics).
*/

#include "platform.h"

#include <string.h>

/* ---------- Win32 */

BOOL WINAPI GetFileSizeEx(HANDLE file, PLARGE_INTEGER size);

DWORD WINAPI GetFileSize(HANDLE file, LPDWORD high)
{
	LARGE_INTEGER size;

	if (!GetFileSizeEx(file, &size))
	{
		if (high)
			*high = 0;
		return 0xFFFFFFFF;
	}
	if (high)
		*high = (DWORD)size.HighPart;
	return size.LowPart;
}

LONG WINAPI CompareFileTime(const FILETIME *a, const FILETIME *b)
{
	unsigned long long x = ((unsigned long long)a->dwHighDateTime << 32) | a->dwLowDateTime;
	unsigned long long y = ((unsigned long long)b->dwHighDateTime << 32) | b->dwLowDateTime;

	return x < y ? -1 : x > y ? 1 : 0;
}

/* MmQueryAddressProtect, through the retail XBE's kernel import (ordinal 179) */
DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	return (*(DWORD (WINAPI **)(LPVOID))0x1d6734)(address);
}

/* the code protection shell_xbox.c changes after a debug monitor walk, which a retail console never gets to */
BOOL WINAPI VirtualProtect(LPVOID address, DWORD size, DWORD protection, PDWORD old_protection)
{
	if (old_protection)
		*old_protection = PAGE_EXECUTE_READWRITE;
	return TRUE;
}

/* the dashboard's language (ExQueryNonVolatileSetting XC_LANGUAGE, 7); English if unset */
LONG WINAPI ExQueryNonVolatileSetting(DWORD index, DWORD *type, void *value, DWORD size, DWORD *result_size);

DWORD WINAPI XGetLanguage(void)
{
	DWORD type, language = 0, got = 0;

	if (ExQueryNonVolatileSetting(7, &type, &language, sizeof(language), &got) < 0 ||
		language < XC_LANGUAGE_ENGLISH || language > 9)
		language = XC_LANGUAGE_ENGLISH;
	return language;
}

/* whether the dashboard is set to widescreen (ExQueryNonVolatileSetting
XC_VIDEO, 8: XC_VIDEO_FLAGS_WIDESCREEN 0x10000), read once: Halo Universal's
Hor+ view (render_cameras.c, render_objects.c) */
int oxbox_dashboard_widescreen(void);

/* the screen shape the game uses: OpenCE's OPTIONS (T:\\opence_settings.txt), else the dashboard's */
int oxbox_widescreen(void)
{
	extern int hmp_settings_aspect(void);
	int aspect = hmp_settings_aspect();

	return aspect >= 0 ? aspect : oxbox_dashboard_widescreen();
}

int oxbox_dashboard_widescreen(void)
{
	static int known = 0, widescreen = 0;

	if (!known)
	{
		DWORD type, flags = 0, got = 0;

		known = 1;
		widescreen = ExQueryNonVolatileSetting(8, &type, &flags, sizeof(flags), &got) >= 0 &&
			(flags & 0x10000) != 0;
	}
	return widescreen;
}

/* ---------- D3DX */

D3DXMATRIX *WINAPI D3DXMatrixOrthoLH(D3DXMATRIX *out, float width, float height, float near_z, float far_z)
{
	memset(out, 0, sizeof(*out));
	out->_11 = 2.0f / width;
	out->_22 = 2.0f / height;
	out->_33 = 1.0f / (far_z - near_z);
	out->_43 = near_z / (near_z - far_z);
	out->_44 = 1.0f;
	return out;
}

D3DXMATRIX *WINAPI D3DXMatrixPerspectiveLH(D3DXMATRIX *out, float width, float height, float near_z, float far_z)
{
	memset(out, 0, sizeof(*out));
	out->_11 = 2.0f * near_z / width;
	out->_22 = 2.0f * near_z / height;
	out->_33 = far_z / (far_z - near_z);
	out->_34 = 1.0f;
	out->_43 = near_z * far_z / (near_z - far_z);
	return out;
}

D3DXVECTOR4 *WINAPI D3DXVec4Transform(D3DXVECTOR4 *out, const D3DXVECTOR4 *v, const D3DXMATRIX *m)
{
	D3DXVECTOR4 r;

	r.x = v->x * m->_11 + v->y * m->_21 + v->z * m->_31 + v->w * m->_41;
	r.y = v->x * m->_12 + v->y * m->_22 + v->z * m->_32 + v->w * m->_42;
	r.z = v->x * m->_13 + v->y * m->_23 + v->z * m->_33 + v->w * m->_43;
	r.w = v->x * m->_14 + v->y * m->_24 + v->z * m->_34 + v->w * m->_44;
	*out = r;
	return out;
}

/* the retail D3D library has no callbacks in the push buffer at 3911's level the profiler used; profiling only */
void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	PLATFORM_UNIMPLEMENTED();
}

/* ---------- the debug monitor: absent on a retail console */

HRESULT WINAPI DmWalkLoadedModules(PDM_WALK_MODULES *walk, PDMN_MODLOAD module)
{
	return XBDM_ENDOFLIST;
}

HRESULT WINAPI DmWalkModuleSections(PDM_WALK_MODSECT *walk, const char *module_name, PDMN_SECTIONLOAD section)
{
	return XBDM_ENDOFLIST;
}

HRESULT WINAPI DmCloseModuleSections(PDM_WALK_MODSECT walk)
{
	return XBDM_NOERR;
}

/* ---------- input: the retail library's device types only */

XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;
unsigned long oxbox_virtual_pad_insertions(void);
HANDLE oxbox_virtual_pad_open(DWORD port);

void WINAPI retail_XInitDevices(DWORD count, PXDEVICE_PREALLOC_TYPE types);
BOOL WINAPI retail_XGetDeviceChanges(PXPP_DEVICE_TYPE type, PDWORD insertions, PDWORD removals);
HANDLE WINAPI retail_XInputOpen(PXPP_DEVICE_TYPE type, DWORD port, DWORD slot, PXINPUT_POLLING_PARAMETERS parameters);

void WINAPI XInitDevices(DWORD count, PXDEVICE_PREALLOC_TYPE types)
{
	XDEVICE_PREALLOC_TYPE known[8];
	DWORD index, kept = 0;

	for (index = 0; index < count && kept < 8; index++)
		if (types[index].DeviceType != &XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE)
			known[kept++] = types[index];
	retail_XInitDevices(kept, known);
	platform_log("XInitDevices: %lu device types", kept);
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE type, PDWORD insertions, PDWORD removals)
{
	if (type == &XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE)
	{
		*insertions = *removals = 0;
		return FALSE;
	}
	{
		static int calls;
		BOOL changed = retail_XGetDeviceChanges(type, insertions, removals);

		if (type == &XDEVICE_TYPE_GAMEPAD_TABLE)
		{
			unsigned long virtual_insertions = oxbox_virtual_pad_insertions();

			if (!changed)
				*insertions = *removals = 0;
			if (virtual_insertions)
			{
				*insertions |= virtual_insertions;
				changed = TRUE;
			}
		}

		if (changed || calls++ < 4 || calls % 500 == 0)
			platform_log("device changes (%s): inserted %08lx, removed %08lx",
				type == &XDEVICE_TYPE_GAMEPAD_TABLE ? "gamepad" : "other", *insertions, *removals);
		return changed;
	}
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE type, DWORD port, DWORD slot, PXINPUT_POLLING_PARAMETERS parameters)
{
	if (type == &XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE)
		return NULL;
	{
		HANDLE handle = type == &XDEVICE_TYPE_GAMEPAD_TABLE ? oxbox_virtual_pad_open(port) : NULL;

		if (!handle)
			handle = retail_XInputOpen(type, port, slot, parameters);

		platform_log("XInputOpen(port %lu) = %p (error %lu)", port, handle, handle ? 0 : GetLastError());
		return handle;
	}
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	return ERROR_HANDLE_EOF;
}

/* ---------- Bink: statistics the retail Bink library leaves out */

void WINAPI BinkGetSummary(void *bink, void *summary)
{
	PLATFORM_UNIMPLEMENTED();
}

void WINAPI BinkGetRealtime(void *bink, void *realtime, DWORD frames)
{
	PLATFORM_UNIMPLEMENTED();
}

/* ---------- physical memory, logged: the proof of concept's tightest budget */

LPVOID WINAPI retail_XPhysicalAlloc(SIZE_T size, ULONG_PTR address, ULONG_PTR alignment, DWORD protect);

static unsigned long free_kb(void)
{
	MEMORYSTATUS memory;

	GlobalMemoryStatus(&memory);
	return memory.dwAvailPhys / 1024;
}

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR address, ULONG_PTR alignment, DWORD protect)
{
	LPVOID result = retail_XPhysicalAlloc(size, address, alignment, protect);

	platform_log("XPhysicalAlloc(%lu KB at %08lx) = %p, %lu KB free", (unsigned long)size / 1024,
		(unsigned long)address, result, free_kb());
	return result;
}

void oxbox_log_allocation_failure(unsigned long size)
{
	platform_log("malloc(%lu) failed: %lu KB free", size, free_kb());
}

HGLOBAL WINAPI retail_GlobalAlloc(UINT flags, SIZE_T size);

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T size)
{
	HGLOBAL result = retail_GlobalAlloc(flags, size);

	static unsigned long total, count;
	if (!result)
		oxbox_log_allocation_failure(size);
	else
	{
		total += size;
		count++;
		if (size >= 0x10000)
			platform_log("GlobalAlloc(%lu KB) = %p by %p, %lu KB free (all GlobalAllocs so far: %lu KB in %lu)", (unsigned long)size / 1024,
				result, __builtin_return_address(0), free_kb(), total / 1024, count);
	}
	return result;
}

/* ---------- thread priorities, logged */

BOOL WINAPI retail_SetThreadPriority(HANDLE thread, int priority);

BOOL WINAPI SetThreadPriority(HANDLE thread, int priority)
{
	BOOL result = retail_SetThreadPriority(thread, priority);

	platform_log("SetThreadPriority(%p, %d) = %d", thread, priority, result);
	return result;
}

/* ---------- the Bink player's sections, loaded only while a movie plays

The XBE's BINK* sections (Bink's code, ~100 KB, and BINKDATA) are marked not to load at boot (mkxbe.py), so a
game doesn't carry them: bink_playback.c loads them before a movie and unloads them after, with the kernel's
XeLoadSection / XeUnloadSection (exports 327 and 328; retail Halo never imported them, so they are looked up in the
kernel's export table). */

typedef LONG (WINAPI *oxbox_section_proc)(void *section);

static void *oxbox_kernel_export(unsigned long ordinal)
{
	const unsigned char *base = (const unsigned char *)0x80010000;
	unsigned long pe, exports, ordinal_base, count;
	const unsigned long *functions;

	if (base[0] != 'M' || base[1] != 'Z')
		return NULL;
	pe = *(const unsigned long *)(base + 0x3C);
	if (memcmp(base + pe, "PE\0\0", 4))
		return NULL;
	/* (the optional header's first data directory, the exports: PE header 0x18, its directories at 0x60) */
	exports = *(const unsigned long *)(base + pe + 0x18 + 0x60);
	if (!exports)
		return NULL;
	ordinal_base = *(const unsigned long *)(base + exports + 0x10);
	count = *(const unsigned long *)(base + exports + 0x14);
	functions = (const unsigned long *)(base + *(const unsigned long *)(base + exports + 0x1C));
	if (ordinal < ordinal_base || ordinal - ordinal_base >= count || !functions[ordinal - ordinal_base])
		return NULL;
	return (void *)(base + functions[ordinal - ordinal_base]);
}

static int oxbox_bink_loaded;

/* load (TRUE) or unload (FALSE) the BINK* sections; FALSE if they couldn't be loaded */
int oxbox_bink_sections(int load)
{
	static oxbox_section_proc load_section, unload_section;
	const unsigned char *header = (const unsigned char *)0x10000;
	unsigned long count = *(const unsigned long *)(header + 0x11C);
	unsigned char *sections = *(unsigned char *const *)(header + 0x120);
	unsigned long index;
	int ok = 1;

	if (load == oxbox_bink_loaded)
		return 1;
	if (!load_section)
	{
		load_section = (oxbox_section_proc)oxbox_kernel_export(327);
		unload_section = (oxbox_section_proc)oxbox_kernel_export(328);
		if (!load_section || !unload_section)
		{
			platform_log("bink: the kernel's XeLoadSection/XeUnloadSection weren't found");
			load_section = unload_section = NULL;
			return 0;
		}
	}
	for (index = 0; index < count; index++)
	{
		unsigned char *section = sections + index * 0x38;
		const char *name = *(const char *const *)(section + 0x14);

		if (strncmp(name, "BINK", 4) || (*(unsigned long *)section & 2))
			continue;
		if (load)
		{
			LONG status = load_section(section);

			if (status < 0)
			{
				platform_log("bink: XeLoadSection(%s) failed (%08lx)", name, (unsigned long)status);
				ok = 0;
				break;
			}
		}
		else
			unload_section(section);
	}
	if (load && !ok)
	{
		/* (the ones loaded before it) */
		unsigned long undo;

		for (undo = 0; undo < index; undo++)
		{
			unsigned char *section = sections + undo * 0x38;
			const char *name = *(const char *const *)(section + 0x14);

			if (!strncmp(name, "BINK", 4) && !(*(unsigned long *)section & 2))
				unload_section(section);
		}
		return 0;
	}
	oxbox_bink_loaded = load;
	platform_log("bink: player %s", load ? "loaded" : "unloaded");
	return 1;
}

/* ---------- the developers' perf log, in a section of its own

link.ld puts the perf log (oxbox_perf.c) in the XBE section OXPERF (mkxbe.py), which isn't loaded at boot: with
perf_log off it takes no memory, and the game's hooks (oxbox_perf.h) find oxbox_perf NULL. With perf_log = 1 in
opence.ini, start-up loads it here, once, and oxbox_perf points at its hooks. */

#include "oxbox_perf.h"

struct oxbox_perf_hooks *oxbox_perf;

void oxbox_perf_load(void)
{
	struct oxbox_perf_hooks *oxbox_perf_start(void);
	oxbox_section_proc load_section = (oxbox_section_proc)oxbox_kernel_export(327);
	const unsigned char *header = (const unsigned char *)0x10000;
	unsigned long count = *(const unsigned long *)(header + 0x11C);
	unsigned char *sections = *(unsigned char *const *)(header + 0x120);
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		unsigned char *section = sections + index * 0x38;
		LONG status;

		if (strcmp(*(const char *const *)(section + 0x14), "OXPERF"))
			continue;
		if (!load_section)
		{
			platform_log("perf: the kernel's XeLoadSection wasn't found");
			return;
		}
		status = load_section(section);
		if (status < 0)
		{
			platform_log("perf: XeLoadSection(OXPERF) failed (%08lx)", (unsigned long)status);
			return;
		}
		oxbox_perf = oxbox_perf_start();
		return;
	}
	platform_log("perf: this XBE has no OXPERF section");
}

/* ---------- the developers' FTP server (port/oxbox/net/oxbox_ftp.c), in the menus only

opence.ini ftp = 1 (ftp_password = text, xbox if it isn't there; ftp_port = N, 21): its section, OXFTP, is loaded the first time
the menus are up, and the server runs while they are; it stops (its memory freed) while a map is loaded. Without
ftp = 1 nothing of it is loaded. */

int oxbox_setting_ftp = 0;
char oxbox_setting_ftp_password[33] = "xbox";   /* (opence.ini ftp_password = text; the dashboards' FTP's own) */
int oxbox_setting_ftp_port = 21;
volatile long oxbox_ftp_maps_changed;   /* (the server: a file under maps\ written, deleted or renamed) */
char oxbox_ftp_status[64];              /* (the server: "FTP: 192.168.1.20 port 21" while it runs, the menus' line) */
long oxbox_ftp_writes;                  /* (changes seen: cache_files.c copies folder maps afresh after one) */

void oxbox_ftp_frame(int menus)
{
	int oxbox_ftp_start(unsigned short port, char const *password);
	void oxbox_ftp_stop(void);
	void hmp_maps_scan(void);
	int hmp_browse_list_showing(void);
	static int loaded, failed, on;
	static long seen_changes;
	static unsigned long next_try;
	unsigned long now = GetTickCount();

	if (!oxbox_setting_ftp || failed)
		return;
	if (!loaded)
	{
		oxbox_section_proc load_section = (oxbox_section_proc)oxbox_kernel_export(327);
		const unsigned char *header = (const unsigned char *)0x10000;
		unsigned long count = *(const unsigned long *)(header + 0x11C);
		unsigned char *sections = *(unsigned char *const *)(header + 0x120);
		unsigned long index;

		if (!oxbox_setting_ftp_password[0])
		{
			platform_log("ftp: ftp_password is empty in opence.ini: no FTP");
			failed = 1;
			return;
		}
		for (index = 0; index < count; index++)
		{
			unsigned char *section = sections + index * 0x38;
			if (!strcmp(*(const char *const *)(section + 0x14), "OXFTP"))
				break;
		}
		if (index == count || !load_section || load_section(sections + index * 0x38) < 0)
		{
			platform_log("ftp: the OXFTP section couldn't be loaded: no FTP");
			failed = 1;
			return;
		}
		loaded = 1;
	}
	if (menus && !on && (long)(now - next_try) >= 0)
	{
		if (oxbox_ftp_start((unsigned short)oxbox_setting_ftp_port, oxbox_setting_ftp_password))
			on = 1;
		else
			next_try = now + 5000;          /* (the network isn't up yet) */
	}
	else if (!menus && on)
	{
		oxbox_ftp_stop();
		on = 0;
	}
	/* (a change in the maps folder: the map list scanned again, on Halo's thread; folder maps copied afresh) */
	if (menus && oxbox_ftp_maps_changed != seen_changes && !hmp_browse_list_showing())   /* (not under an open map list) */
	{
		seen_changes = oxbox_ftp_maps_changed;
		oxbox_ftp_writes++;
		hmp_maps_scan();
	}
}
