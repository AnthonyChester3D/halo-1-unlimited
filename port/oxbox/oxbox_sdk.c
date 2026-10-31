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

#include <stdlib.h>
#include <string.h>

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);

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

/* ---------- the look for a newer release, and the offer to update

opence.ini update_check = 1 (the default; 0: never; 2: even on a test build, release 0, to try it): once a boot, in
the menus, the first 128 bytes of the project's latest package (its header: release and network version) are fetched
from GitHub by the XBE section OXUPD (net/oxbox_update.c, Mbed TLS: about 180 KB while it runs), which is
unloaded as soon as the answer is in, or when a map starts loading. The installed release is the installer's word in
D:\opence_release.txt. A newer one is offered on the main menu (interface.c draws it, ui_widget.c gives it the
buttons): A launches the updater, the installer's copy in D:\updater (it writes it there when it installs), with a
note, auto_update.txt, that has it update this folder and start the game again; B asks again next boot. */

int oxbox_setting_update_check = 1;
volatile long oxbox_update_found_release = -1, oxbox_update_found_network = -1;   /* (OXUPD's answer) */

#define OXBOX_UPDATE_URL "https://github.com/AnthonyChester3D/halo-1-unlimited/releases/latest/download/opence.oxpk"
#define OXBOX_RELEASE_PATH "D:\\opence_release.txt"
#define OXBOX_UPDATER_PATH "D:\\updater\\default.xbe"
#define OXBOX_UPDATER_NOTE "D:\\updater\\auto_update.txt"

static struct
{
	int state;                         /* 0 not yet, 1 checking, 2 done (or not to be done) */
	long installed_release, installed_network;
	int offer, dismissed, updater_missing;
	unsigned long started, drawn, closed;
	void *section;
} oxbox_update;

/* the installer's word on what's installed (D:\opence_release.txt: "release N", "network N"): 0 if it isn't there */
static void oxbox_update_read_installed(void)
{
	HANDLE file = CreateFileA(OXBOX_RELEASE_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	char text[256];
	DWORD got = 0;
	char *at;

	oxbox_update.installed_release = 0;
	oxbox_update.installed_network = 0;
	if (file == INVALID_HANDLE_VALUE)
		return;
	if (!ReadFile(file, text, sizeof(text) - 1, &got, NULL))
		got = 0;
	CloseHandle(file);
	text[got] = 0;
	if ((at = strstr(text, "release ")) != NULL)
		oxbox_update.installed_release = (long)strtoul(at + 8, NULL, 10);
	if ((at = strstr(text, "network ")) != NULL)
		oxbox_update.installed_network = (long)strtoul(at + 8, NULL, 10);
}

static void *oxbox_update_section(void)
{
	const unsigned char *header = (const unsigned char *)0x10000;
	unsigned long count = *(const unsigned long *)(header + 0x11C);
	unsigned char *sections = *(unsigned char *const *)(header + 0x120);
	unsigned long index;

	for (index = 0; index < count; index++)
		if (!strcmp(*(const char *const *)(sections + index * 0x38 + 0x14), "OXUPD"))
			return sections + index * 0x38;
	return NULL;
}

void oxbox_update_frame(int menus)
{
#if defined(OXBOX_NET_P2P)
	int oxbox_update_check_start(char const *url);
	int oxbox_update_check_running(void);
	void oxbox_update_check_stop(void);
	void oxbox_update_check_done(void);
	unsigned long now = GetTickCount();

	if (oxbox_update.state == 0)
	{
		int oxbox_lwip_address(unsigned long *address, int *dhcp);
		oxbox_section_proc load_section;
		unsigned long address;
		int dhcp;

		if (!menus || now < 8000)          /* (the boot's own work first) */
			return;
		oxbox_update_read_installed();
		if (!oxbox_setting_update_check || (oxbox_update.installed_release == 0 && oxbox_setting_update_check != 2))
		{
			platform_log("update: no check (%s)", oxbox_setting_update_check ? "a test build: release 0" :
				"update_check = 0 in opence.ini");
			oxbox_update.state = 2;
			return;
		}
		if (!oxbox_lwip_address(&address, &dhcp))
			return;                        /* (no network yet: no section loaded for nothing) */
		load_section = (oxbox_section_proc)oxbox_kernel_export(327);
		oxbox_update.section = oxbox_update_section();
		if (!load_section || !oxbox_update.section || load_section(oxbox_update.section) < 0)
		{
			platform_log("update: the OXUPD section couldn't be loaded: no check");
			oxbox_update.state = 2;
			return;
		}
		oxbox_update.state = 1;
		oxbox_update.started = 0;
	}
	if (oxbox_update.state == 1)
	{
		if (!oxbox_update.started)
		{
			if (!menus)
				goto unload;
			if (!oxbox_update_check_start(OXBOX_UPDATE_URL))
			{
				platform_log("update: the check couldn't start");
				goto unload;
			}
			oxbox_update.started = now;
			platform_log("update: looking for a newer release (this is release %ld)", oxbox_update.installed_release);
			return;
		}
		if (!menus)
			oxbox_update_check_stop();
		if (oxbox_update_check_running())
			return;
unload:
		if (oxbox_update.started)
			oxbox_update_check_done();
		{
			oxbox_section_proc unload_section = (oxbox_section_proc)oxbox_kernel_export(328);

			if (unload_section && oxbox_update.section)
				unload_section(oxbox_update.section);
		}
		oxbox_update.state = 2;
		if (oxbox_update_found_release > oxbox_update.installed_release)
		{
			oxbox_update.offer = 1;
			oxbox_update.updater_missing = GetFileAttributesA(OXBOX_UPDATER_PATH) == 0xFFFFFFFF;
			platform_log("update: release %ld is newer than this one (%ld): offered on the main menu%s",
				oxbox_update_found_release, oxbox_update.installed_release,
				oxbox_update.updater_missing ? " (no updater in D:\\updater: the installer's Y)" : "");
		}
	}
#else
	(void)menus;
#endif
}

/* the main menu's offer: its lines (up to 5, 100 characters each), or 0 when there's none to show */
int oxbox_update_prompt_lines(wchar_t lines[5][100])
{
	char text[5][100];
	int count = 0, line, index;

	if (!oxbox_update.offer || oxbox_update.dismissed)
		return 0;
	halo_linux_snprintf(text[count++], 100, "Halo 1 Unlimited release %ld is available (you have release %ld).",
		oxbox_update_found_release, oxbox_update.installed_release);
	if (oxbox_update_found_network > oxbox_update.installed_network && oxbox_update.installed_network)
		halo_linux_snprintf(text[count++], 100, "Online games need it: yours can't join them until it's updated.");
	text[count++][0] = 0;
	if (oxbox_update.updater_missing)
	{
		halo_linux_snprintf(text[count++], 100, "Update it with the installer (Y: look online).");
		halo_linux_snprintf(text[count++], 100, "B: OK");
	}
	else
	{
		halo_linux_snprintf(text[count++], 100, "A: update now (the updater starts, then the game again)");
		halo_linux_snprintf(text[count++], 100, "B: later");
	}
	for (line = 0; line < count; line++)
	{
		for (index = 0; text[line][index] && index < 99; index++)
			lines[line][index] = (wchar_t)(unsigned char)text[line][index];
		lines[line][index] = 0;
	}
	oxbox_update.drawn = GetTickCount();
	return count;
}

/* whether the offer is on the screen (drawn in the last half second): the showcase waits */
int oxbox_update_prompt_showing(void)
{
	return oxbox_update.offer && !oxbox_update.dismissed && GetTickCount() - oxbox_update.drawn < 500;
}

/* a button pressed (or held) while the offer may be on the screen: 1 when it's the offer's (the menu doesn't get
it); a is 1 for A, b 1 for B, pressed 1 for a press (not a hold) */
int oxbox_update_prompt_button(int a, int b, int pressed)
{
	unsigned long now = GetTickCount();

	if (oxbox_update.closed && now - oxbox_update.closed < 500 && (a || b) && !pressed)
		return 1;                      /* (still held from closing it) */
	if (!oxbox_update.offer || oxbox_update.dismissed || now - oxbox_update.drawn > 500)
		return 0;
	if (!pressed)
		return 1;
	if (b)
	{
		oxbox_update.dismissed = 1;
		oxbox_update.closed = now;
		platform_log("update: later (asked again next boot)");
	}
	else if (a && !oxbox_update.updater_missing)
	{
		HANDLE note = CreateFileA(OXBOX_UPDATER_NOTE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		char text[96];
		DWORD put = 0;
		int size = halo_linux_snprintf(text, sizeof(text), "update this Halo folder to release %ld, then start it\r\n",
			oxbox_update_found_release);

		if (note == INVALID_HANDLE_VALUE || !WriteFile(note, text, (DWORD)size, &put, NULL) || put != (DWORD)size)
		{
			if (note != INVALID_HANDLE_VALUE)
				CloseHandle(note);
			platform_log("update: couldn't write %s: not started", OXBOX_UPDATER_NOTE);
			oxbox_update.updater_missing = 1;
			return 1;
		}
		CloseHandle(note);
		platform_log("update: starting the updater");
		XLaunchNewImageA(OXBOX_UPDATER_PATH, NULL);
		platform_log("update: the updater didn't start");
		DeleteFileA(OXBOX_UPDATER_NOTE);
		oxbox_update.updater_missing = 1;
	}
	return 1;
}

/* ---------- the video mode: 480p when the dashboard allows it

Halo asked D3D for no progressive scan, so it ran 480i on an HD cable (component, or an HDMI adapter reporting as one)
even with 480p turned on in the dashboard, while the installer (nxdk) and the dashboards ran 480p: the TV re-synced
at each switch. Now the game asks for 480p exactly when the installer gets it: an HDTV AV pack, NTSC, and the
dashboard's 480p on (the AV encoder's settings, AvSendTVEncoderOption VIDEO_ENC_GET_SETTINGS, as nxdk's
XVideoGetEncoderSettings reads them). Everything else stays 480i (or PAL) as it was; and if D3D won't make the
device progressive, rasterizer_xbox.c makes it again without (480i). The same 640x480 frame is drawn either way. */

#define OXBOX_PRESENT_PROGRESSIVE 0x00000040   /* (D3DPRESENTFLAG_PROGRESSIVE) */

unsigned long oxbox_video_present_flags(void)
{
	typedef void (WINAPI *encoder_option_proc)(void *base, ULONG option, ULONG parameter, ULONG *result);
	static int known;
	static unsigned long flags;

	if (!known)
	{
		encoder_option_proc encoder_option = (encoder_option_proc)oxbox_kernel_export(2);
		ULONG settings = 0;

		known = 1;
		if (encoder_option)
			encoder_option((void *)0xFD000000, 6, 0, &settings);
		if ((settings & 0xFF) == 4 && (settings & 0xFF00) != 0x300 && (settings & 0x80000))
			flags = OXBOX_PRESENT_PROGRESSIVE;
		platform_log("video: AV pack %lu, encoder settings %08lx: %s", (unsigned long)(settings & 0xFF),
			(unsigned long)settings, flags ? "480p (the dashboard's)" : "480i");
	}
	return flags;
}
