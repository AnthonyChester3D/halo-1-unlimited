/* main.c: the Halo 1 Unlimited installer, an XBE of its own (nxdk; port/oxbox/installer/README.md). It makes OpenCE
from the player's own retail Halo XBE, with the package beside it (opence.oxpk: tools/mkpackage.py) and the pictures
from the game's own ui.map (pictures.c), keeping the retail XBE as default_retail.xbe. Nothing of Microsoft's code or
of Halo's art is in the installer or the package.

The list shows every Halo it finds: the ones it can install to first, in green; the others in grey.
	A      install (or update) OpenCE in the folder chosen
	X      put the retail Halo back (from default_retail.xbe)
	START  play what's in the folder (green ones only)
	B      back to the dashboard
	LT/RT  a page up or down */
#include <hal/video.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <SDL.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "package.h"
#include "pictures.h"
#include "screen.h"
#include "sha256.h"
#include "text.h"

#define TITLE "Halo 1 Unlimited installer"
#define PACKAGE_PATH "D:\\opence.oxpk"
#define LOG_PATH "D:\\installer_log.txt"
#define HALO_TITLE_ID 0x4D530004ul
#define MAX_XBE (16 * 1024 * 1024)
static struct package package;
static int package_ok;
static char package_error[256];

static unsigned long u32(unsigned char const *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* ---- the log: the screen below the heading while working, and installer_log.txt ---- */

#define LOG_TOP 5
#define LOG_ROWS (SCREEN_ROWS - LOG_TOP)
static char log_lines[LOG_ROWS][SCREEN_COLUMNS + 1];
static unsigned long log_colours[LOG_ROWS];
static int log_count;

static void log_file(char const *text)
{
	FILE *file = fopen(LOG_PATH, "a");
	if (file)
	{
		fprintf(file, "%s\n", text);
		fclose(file);
	}
}

static void log_clear(void)
{
	memset(log_lines, 0, sizeof(log_lines));
	log_count = 0;
}

static void say_in(unsigned long colour, char const *format, ...)
{
	char text[256];
	va_list list;
	int r;
	va_start(list, format);
	text_vprint(text, sizeof(text), format, list);
	va_end(list);
	log_file(text);
	if (log_count == LOG_ROWS)
	{
		memmove(log_lines[0], log_lines[1], sizeof(log_lines[0]) * (LOG_ROWS - 1));
		memmove(&log_colours[0], &log_colours[1], sizeof(log_colours[0]) * (LOG_ROWS - 1));
		log_count--;
	}
	text_print(log_lines[log_count], sizeof(log_lines[0]), "%s", text);
	log_colours[log_count++] = colour;
	for (r = 0; r < LOG_ROWS; r++)
	{
		screen_row(LOG_TOP + r, COLOUR_BLACK);
		screen_text(LOG_TOP + r, 0, log_colours[r], "%s", log_lines[r]);
	}
	screen_show();
}

#define say(...) say_in(COLOUR_WHITE, __VA_ARGS__)

static void pictures_step(char const *step)
{
	say("  %s", step);
}

static void heading(char const *what)
{
	screen_clear();
	screen_text(0, 0, COLOUR_YELLOW, TITLE);
	screen_text(1, 0, COLOUR_GREY, "Package: ");
	screen_text(1, 9, COLOUR_WHITE, "%s", package_ok ? package.name : "none");
	screen_text(2, 0, COLOUR_DIM, "------------------------------------------------------------------------");
	if (what)
		screen_text(3, 0, COLOUR_WHITE, "%s", what);
	screen_show();
}

/* ---- the controllers: every pad, d-pad or left stick (held: repeats), triggers a page ---- */

enum { NONE, UP, DOWN, PAGE_UP, PAGE_DOWN, A, B, X, START };

static SDL_GameController *pads[4];
static unsigned held_buttons = ~0u; /* (none counts as pressed until it's been seen up) */
static int held_direction;
static DWORD held_since, last_repeat;

static int poll_input(void)
{
	SDL_Event e;
	unsigned buttons = 0, pressed;
	int direction = NONE, i;
	DWORD now;
	while (SDL_PollEvent(&e))
	{
		if (e.type == SDL_CONTROLLERDEVICEADDED)
		{
			for (i = 0; i < 4 && pads[i]; i++)
				;
			if (i < 4)
				pads[i] = SDL_GameControllerOpen(e.cdevice.which);
		}
		else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
		{
			for (i = 0; i < 4; i++)
			{
				if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == e.cdevice.which)
				{
					SDL_GameControllerClose(pads[i]);
					pads[i] = NULL;
				}
			}
		}
	}
	for (i = 0; i < 4; i++)
	{
		SDL_GameController *p = pads[i];
		int y;
		if (!p)
			continue;
		buttons |= SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_A) << A;
		buttons |= (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_B) |
			SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_BACK)) << B;
		buttons |= SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_X) << X;
		buttons |= SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_START) << START;
		y = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
		if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -16000)
			direction = UP;
		else if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > 16000)
			direction = DOWN;
		else if (SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000)
			direction = PAGE_UP;
		else if (SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)
			direction = PAGE_DOWN;
	}
	pressed = buttons & ~held_buttons;
	held_buttons = buttons;
	for (i = A; i <= START; i++)
	{
		if (pressed & (1u << i))
			return i;
	}
	now = GetTickCount();
	if (direction != held_direction)
	{
		held_direction = direction;
		held_since = last_repeat = now;
		return direction;
	}
	if (direction != NONE && now - held_since > 400 && now - last_repeat > 90)
	{
		last_repeat = now;
		return direction;
	}
	return NONE;
}

static int wait_input(void)
{
	int b;
	while ((b = poll_input()) == NONE)
		Sleep(16);
	return b;
}

/* ---- files ---- */

static int exists(char const *path)
{
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* a whole file (up to MAX_XBE bytes), or NULL */
static unsigned char *read_whole(char const *path, unsigned long *size)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	unsigned char *data;
	DWORD got;
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	*size = GetFileSize(file, NULL);
	if (*size == INVALID_FILE_SIZE || *size > MAX_XBE || !(data = malloc(*size ? *size : 1)))
	{
		CloseHandle(file);
		return NULL;
	}
	if (!ReadFile(file, data, *size, &got, NULL) || got != *size)
	{
		free(data);
		data = NULL;
	}
	CloseHandle(file);
	return data;
}

/* the file's SHA-256 with some of it read as zeros (a build's pictures), as written on the disk; 0 when read */
static int file_sha256(char const *path, unsigned long const *zero_at, unsigned long const *zero_size, int zeros,
	unsigned char *digest, unsigned long *size)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	static unsigned char buffer[65536];
	struct sha256 s;
	unsigned long at = 0;
	DWORD got;
	int z;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	sha256_start(&s);
	while (ReadFile(file, buffer, sizeof(buffer), &got, NULL) && got)
	{
		for (z = 0; z < zeros; z++)
		{
			unsigned long from = zero_at[z] > at ? zero_at[z] : at;
			unsigned long to = zero_at[z] + zero_size[z] < at + got ? zero_at[z] + zero_size[z] : at + got;
			if (from < to)
				memset(buffer + (from - at), 0, to - from);
		}
		sha256_add(&s, buffer, got);
		at += got;
	}
	CloseHandle(file);
	sha256_end(&s, digest);
	*size = at;
	return 0;
}

struct writer
{
	HANDLE file;
};

static int write_part(void *context, unsigned char const *data, unsigned long size)
{
	DWORD put;
	return !WriteFile(((struct writer *)context)->file, data, size, &put, NULL) || put != size;
}

static HANDLE create(char const *path)
{
	DeleteFileA(path);
	return CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* (written to the disk before it's closed) */
static int finish(HANDLE file)
{
	IO_STATUS_BLOCK status;
	int ok = NT_SUCCESS(NtFlushBuffersFile(file, &status));
	return CloseHandle(file) && ok;
}

/* ---- XBEs ---- */

/* what an XBE's headers say */
struct xbe_head
{
	int present, halo, opence;
	unsigned long size;
};

static void read_head(char const *path, struct xbe_head *head)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	static unsigned char h[16384];
	unsigned long base, cert, count, sections, i;
	DWORD got = 0;
	memset(head, 0, sizeof(*head));
	if (file == INVALID_HANDLE_VALUE)
		return;
	head->present = 1;
	head->size = GetFileSize(file, NULL);
	ReadFile(file, h, sizeof(h), &got, NULL);
	CloseHandle(file);
	if (got < 0x180 || memcmp(h, "XBEH", 4))
		return;
	base = u32(h + 0x104);
	cert = u32(h + 0x118) - base;
	head->halo = cert + 12 <= got && u32(h + cert + 8) == HALO_TITLE_ID;
	/* (an OpenCE build: the perf log's section, OXPERF, which mkxbe.py adds) */
	count = u32(h + 0x11c);
	sections = u32(h + 0x120) - base;
	for (i = 0; i < count && i < 64 && sections + (i + 1) * 0x38 <= got; i++)
	{
		unsigned long name = u32(h + sections + i * 0x38 + 0x14) - base;
		if (name + 7 <= got && !memcmp(h + name, "OXPERF", 7))
			head->opence = 1;
	}
}

static int retail_size(unsigned long size)
{
	int i;
	for (i = 0; package_ok && i < package.count; i++)
	{
		if (package.xbes[i].retail_size == size)
			return 1;
	}
	return 0;
}

/* a retail XBE read whole, and the package's build for it (package_match: its data is the disc's while it's
matched as a patched copy; retail_original() puts the copy's own bytes back) */
struct retail
{
	unsigned char *data;
	unsigned long size;
	struct package_patch patch;
	struct package_xbe const *xbe;
};

static void retail_original(struct retail *r)
{
	package_unpatch(r->data, &r->patch);
}

/* reads path as a retail XBE; r->xbe NULL if the package doesn't know it */
static int retail_read(char const *path, struct retail *r)
{
	memset(r, 0, sizeof(*r));
	r->data = read_whole(path, &r->size);
	if (!r->data)
		return 1;
	if (package_ok)
		r->xbe = package_match(&package, r->data, r->size, &r->patch);
	return 0;
}

static void retail_free(struct retail *r)
{
	free(r->data);
	r->data = NULL;
}

/* ---- the Halo folders ---- */

enum
{
	RETAIL,    /* default.xbe is a retail Halo the package knows */
	INSTALLED, /* default.xbe is OpenCE, and default_retail.xbe a retail Halo the package knows */
	OTHER,     /* default.xbe is another XBE, and default_retail.xbe a retail Halo */
	MISSING,   /* no default.xbe, and default_retail.xbe a retail Halo */
	UNKNOWN    /* no retail Halo the package knows (or default_retail.xbe is in the way) */
};

struct halo
{
	char path[160];   /* "E:\Games\Halo" */
	char device[200]; /* "\Device\Harddisk0\Partition1\Games\Halo" */
	int state;
	struct package_xbe const *retail; /* (the package's build for its retail XBE) */
	int patched;                      /* that retail XBE is a disc's patched for media or regions */
	int backup_in_the_way;            /* default_retail.xbe is there and isn't a retail Halo it knows */
	int opence, up_to_date;
	char note[40];
	char detail[2][SCREEN_COLUMNS + 1];
};

#define MAX_HALOS 128
static struct halo halos[MAX_HALOS];
static int halo_count;

static int ready(struct halo const *h)
{
	return h->state != UNKNOWN;
}

static char const *const partitions[] = {"E:\\Partition1", "F:\\Partition6", "G:\\Partition7"};

static void to_device(char const *path, char *device, size_t size)
{
	int i;
	for (i = 0; i < 3; i++)
	{
		if (path[0] == partitions[i][0])
		{
			text_print(device, size, "\\Device\\Harddisk0\\%s%s", partitions[i] + 3, path + 2);
			return;
		}
	}
	text_print(device, size, "%s", path);
}

/* "E:\..." from a device path on a partition this has mounted, or 0 */
static int from_device(char const *device, char *path, size_t size)
{
	int i;
	for (i = 0; i < 3; i++)
	{
		char prefix[64];
		size_t n;
		text_print(prefix, sizeof(prefix), "\\Device\\Harddisk0\\%s\\", partitions[i] + 3);
		n = strlen(prefix);
		if (!_strnicmp(device, prefix, n))
		{
			text_print(path, size, "%c:\\%s", partitions[i][0], device + n);
			return 1;
		}
	}
	return 0;
}

static void examine(struct halo *h)
{
	char main[200], backup[200];
	struct xbe_head main_head, backup_head;
	struct retail r;
	struct package_xbe const *main_retail = NULL, *backup_retail = NULL;
	int main_patched = 0, backup_patched = 0;

	text_print(main, sizeof(main), "%s\\default.xbe", h->path);
	text_print(backup, sizeof(backup), "%s\\default_retail.xbe", h->path);
	read_head(main, &main_head);
	read_head(backup, &backup_head);
	if (main_head.halo && retail_size(main_head.size) && !retail_read(main, &r))
	{
		main_retail = r.xbe, main_patched = r.patch.patched;
		retail_free(&r);
	}
	if (backup_head.present && retail_size(backup_head.size) && !retail_read(backup, &r))
	{
		backup_retail = r.xbe, backup_patched = r.patch.patched;
		retail_free(&r);
	}
	h->opence = main_head.opence;
	h->up_to_date = 0;
	h->backup_in_the_way = backup_head.present && !backup_retail;
	if (main_retail && !h->backup_in_the_way)
	{
		h->state = RETAIL;
		h->retail = main_retail, h->patched = main_patched;
	}
	else if (backup_retail)
	{
		h->state = !main_head.present ? MISSING : h->opence ? INSTALLED : OTHER;
		h->retail = backup_retail, h->patched = backup_patched;
		if (h->state == INSTALLED)
		{
			unsigned long at[4], size[4], made;
			unsigned char digest[32];
			int zeros = package_pictures(&package, h->retail, at, size, 4);
			h->up_to_date = !file_sha256(main, at, size, zeros, digest, &made) && made == h->retail->build_size &&
				!memcmp(digest, h->retail->build_sha256, 32);
		}
	}
	else
	{
		h->state = UNKNOWN;
		h->retail = NULL;
	}
	switch (h->state)
	{
	case RETAIL:
		text_print(h->note, sizeof(h->note), "retail Halo: ready to install");
		text_print(h->detail[0], sizeof(h->detail[0]), "default.xbe: retail %s%s.", h->retail->name,
			h->patched ? ", region/media patched" : "");
		text_print(h->detail[1], sizeof(h->detail[1]), "A installs OpenCE (keeping it as default_retail.xbe).");
		break;
	case INSTALLED:
		text_print(h->note, sizeof(h->note), h->up_to_date ? "OpenCE (this package's)" : "OpenCE (another build)");
		text_print(h->detail[0], sizeof(h->detail[0]), "default.xbe: OpenCE (%s). Kept: %s.",
			h->up_to_date ? "this package's" : "another build", h->retail->name);
		text_print(h->detail[1], sizeof(h->detail[1]), "%s", h->up_to_date ?
			"A installs it again; X puts the retail Halo back." : "A updates it to this package's; X: retail Halo.");
		break;
	case OTHER:
		text_print(h->note, sizeof(h->note), "another XBE (retail kept)");
		text_print(h->detail[0], sizeof(h->detail[0]), "default.xbe: another XBE. Kept: retail %s.", h->retail->name);
		text_print(h->detail[1], sizeof(h->detail[1]), "A installs OpenCE (keeping that XBE as default_old.xbe).");
		break;
	case MISSING:
		text_print(h->note, sizeof(h->note), "no default.xbe (retail kept)");
		text_print(h->detail[0], sizeof(h->detail[0]), "No default.xbe. Kept: retail %s.", h->retail->name);
		text_print(h->detail[1], sizeof(h->detail[1]), "A installs OpenCE; X puts the retail Halo back.");
		break;
	default:
		if (h->backup_in_the_way)
		{
			text_print(h->note, sizeof(h->note), "default_retail.xbe in the way");
			text_print(h->detail[0], sizeof(h->detail[0]),
				"default_retail.xbe is here, but isn't a retail Halo XBE this package knows.");
			text_print(h->detail[1], sizeof(h->detail[1]), "Move it out of the folder, then start the installer again.");
		}
		else
		{
			text_print(h->note, sizeof(h->note), h->opence ? "OpenCE, but no retail kept" : "modified or unknown XBE");
			text_print(h->detail[0], sizeof(h->detail[0]), "%s",
				!package_ok ? "There's no package to install." :
				"default.xbe isn't a retail Halo XBE this package knows (from a disc).");
			text_print(h->detail[1], sizeof(h->detail[1]),
				"To install here, copy your disc's default.xbe in as default_retail.xbe.");
		}
	}
}

static int skipped_folder(char const *name)
{
	static char const *const skip[] = {"TDATA", "UDATA", "CACHE", "$RECYCLE.BIN", "System Volume Information"};
	size_t i;
	if (name[0] == '.')
		return 1;
	for (i = 0; i < sizeof(skip) / sizeof(*skip); i++)
	{
		if (!_stricmp(name, skip[i]))
			return 1;
	}
	return 0;
}

static void scan_note(char const *folder)
{
	screen_row(5, COLOUR_BLACK);
	screen_text(4, 0, COLOUR_WHITE, "Looking for Halo: %d found", halo_count);
	screen_text(5, 0, COLOUR_DIM, "%.72s", folder);
	screen_show();
}

/* folder, if it has a Halo XBE in it (as default.xbe, or default_retail.xbe alone); 1 if it has a default.xbe */
static int add_halo(char const *folder)
{
	char path[200];
	struct xbe_head head;
	int i, has_main;
	text_print(path, sizeof(path), "%s\\default.xbe", folder);
	read_head(path, &head);
	has_main = head.present;
	if (!head.halo)
	{
		text_print(path, sizeof(path), "%s\\default_retail.xbe", folder);
		read_head(path, &head);
		if (!head.halo)
			return has_main;
	}
	for (i = 0; i < halo_count; i++)
	{
		if (!_stricmp(halos[i].path, folder))
			return has_main;
	}
	if (halo_count == MAX_HALOS)
		return has_main;
	scan_note(folder);
	text_print(halos[halo_count].path, sizeof(halos[halo_count].path), "%s", folder);
	to_device(folder, halos[halo_count].device, sizeof(halos[halo_count].device));
	examine(&halos[halo_count]);
	halo_count++;
	return 1;
}

/* the folders in this one: each with a Halo is added; one without any XBE is looked in too, `depth` more levels
(E:\Games, E:\Games\Halo mods) */
static void scan(char const *folder, int depth)
{
	WIN32_FIND_DATAA found;
	char pattern[200], path[200];
	HANDLE find;
	text_print(pattern, sizeof(pattern), "%s\\*", folder);
	find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !skipped_folder(found.cFileName))
		{
			text_print(path, sizeof(path), "%s\\%s", folder, found.cFileName);
			if (!add_halo(path) && depth > 0)
				scan(path, depth - 1);
		}
	} while (FindNextFileA(find, &found));
	FindClose(find);
}

/* the ones it can install to first (each group as found) */
static void sort_halos(void)
{
	static struct halo sorted[MAX_HALOS];
	int i, n = 0;
	for (i = 0; i < halo_count; i++)
	{
		if (ready(&halos[i]))
			sorted[n++] = halos[i];
	}
	for (i = 0; i < halo_count; i++)
	{
		if (!ready(&halos[i]))
			sorted[n++] = halos[i];
	}
	memcpy(halos, sorted, sizeof(halos[0]) * halo_count);
}

static void find_halos(void)
{
	char own[260], *slash, path[260];
	int i;
	halo_count = 0;
	heading(NULL);
	/* the folder the installer is in, if it's in Halo's (E:\Games\Halo\OpenCE installer: E:\Games\Halo) */
	text_print(own, sizeof(own), "%.*s", XeImageFileName->Length, XeImageFileName->Buffer);
	for (i = 0; i < 2 && (slash = strrchr(own, '\\')); i++)
		*slash = 0;
	if (from_device(own, path, sizeof(path)))
		add_halo(path);
	for (i = 0; i < 3; i++)
	{
		char root[4] = {partitions[i][0], ':', 0};
		scan(root, 2);
	}
	sort_halos();
}

/* ---- installing ---- */

/* everything the drive's driver is holding back written now: the folder's changes (a file deleted, renamed) as well
as the files' contents. (FATX keeps them for a while, so a console switched off just after could lose them) */
static void flush_drive(char const *path)
{
	char device[64];
	ANSI_STRING name;
	OBJECT_ATTRIBUTES attributes;
	IO_STATUS_BLOCK status;
	HANDLE volume;
	NTSTATUS result;
	int i;
	for (i = 0; i < 3 && path[0] != partitions[i][0]; i++)
		;
	if (i == 3)
		return;
	text_print(device, sizeof(device), "\\Device\\Harddisk0\\%s", partitions[i] + 3);
	RtlInitAnsiString(&name, device);
	InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
	result = NtOpenFile(&volume, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &attributes, &status,
		FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
	if (NT_SUCCESS(result))
	{
		result = NtFlushBuffersFile(volume, &status);
		NtClose(volume);
	}
	if (!NT_SUCCESS(result))
	{
		char text[96];
		text_print(text, sizeof(text), "(couldn't flush %s: %08lx)", device, (unsigned long)result);
		log_file(text);
	}
}

static unsigned char pictures[PICTURES_SIZE], header[HEADER_SIZE];

static int free_space(char const *path, unsigned long long need)
{
	ULARGE_INTEGER available, total, free_total;
	char root[4] = {path[0], ':', '\\', 0};
	if (!GetDiskFreeSpaceExA(root, &available, &total, &free_total))
		return 1;
	return available.QuadPart >= need;
}

/* writes data to path (through path.new, checked as written), replacing what's there */
static int write_checked(char const *path, unsigned char const *data, unsigned long size)
{
	char temp[220];
	unsigned char want[32], got[32];
	unsigned long got_size;
	struct writer w;
	text_print(temp, sizeof(temp), "%s.new", path);
	w.file = create(temp);
	if (w.file == INVALID_HANDLE_VALUE)
		return 1;
	if (write_part(&w, data, size))
	{
		CloseHandle(w.file);
		return 1;
	}
	if (!finish(w.file))
		return 1;
	sha256(data, size, want);
	if (file_sha256(temp, NULL, NULL, 0, got, &got_size) || got_size != size || memcmp(want, got, 32))
		return 1;
	DeleteFileA(path);
	return !MoveFileA(temp, path);
}

/* the old default.xbe out of the way: an XBE this didn't make is kept (default_old.xbe, default_old2.xbe...) */
static int set_aside(struct halo *h, char const *main)
{
	char old[220];
	int n;
	if (!exists(main))
		return 0;
	if (h->state != OTHER)
		return !DeleteFileA(main);
	for (n = 1; n < 100; n++)
	{
		if (n == 1)
			text_print(old, sizeof(old), "%s\\default_old.xbe", h->path);
		else
			text_print(old, sizeof(old), "%s\\default_old%d.xbe", h->path, n);
		if (!exists(old))
		{
			say("Kept the XBE that was there as %s", strrchr(old, '\\') + 1);
			return !MoveFileA(main, old);
		}
	}
	return 1;
}

static int install(struct halo *h)
{
	char main[200], backup[200], temp[220], ui_map[200], error[256], line[SCREEN_COLUMNS + 8];
	unsigned long zero_at[4], zero_size[4], made_size;
	unsigned char digest[32];
	int zeros, have_pictures;
	struct retail r;
	struct writer w;

	text_print(main, sizeof(main), "%s\\default.xbe", h->path);
	text_print(backup, sizeof(backup), "%s\\default_retail.xbe", h->path);
	text_print(temp, sizeof(temp), "%s\\default.xbe.new", h->path);
	text_print(ui_map, sizeof(ui_map), "%s\\maps\\ui.map", h->path);
	text_print(line, sizeof(line), "Installing OpenCE in %s", h->path);
	heading(line);
	log_clear();
	say("Installing %s", package.name);
	say("in %s", h->path);
	/* the retail XBE, checked again */
	if (retail_read(h->state == RETAIL ? main : backup, &r) || r.xbe != h->retail)
	{
		say_in(COLOUR_RED, "The retail XBE couldn't be read, or changed: look again.");
		retail_free(&r);
		return 1;
	}
	if (!free_space(h->path, (unsigned long long)h->retail->build_size + r.size + 1024 * 1024))
	{
		say_in(COLOUR_RED, "Not enough room on the drive (%lu KB needed).", (h->retail->build_size + r.size) / 1024 + 1024);
		retail_free(&r);
		return 1;
	}
	/* the pictures, from the game's own ui.map */
	say("Making the pictures from maps\\ui.map:");
	pictures_progress = pictures_step;
	have_pictures = !pictures_make(ui_map, pictures, header, error);
	if (!have_pictures)
		say_in(COLOUR_YELLOW, "  no pictures (%s): they'll be blank", error);
	/* the build, beside the old XBE (from the disc's bytes) */
	say("Making OpenCE from %s%s...", h->retail->name, r.patch.patched ? " (region/media patched)" : "");
	w.file = create(temp);
	if (w.file == INVALID_HANDLE_VALUE)
	{
		say_in(COLOUR_RED, "Couldn't create default.xbe.new.");
		retail_free(&r);
		return 1;
	}
	if (package_make(&package, h->retail, r.data, have_pictures ? pictures : NULL, have_pictures ? header : NULL,
		write_part, &w, error))
	{
		CloseHandle(w.file);
		DeleteFileA(temp);
		say_in(COLOUR_RED, "%s", error);
		retail_free(&r);
		return 1;
	}
	if (!finish(w.file))
	{
		DeleteFileA(temp);
		say_in(COLOUR_RED, "Couldn't finish writing default.xbe.new.");
		retail_free(&r);
		return 1;
	}
	zeros = package_pictures(&package, h->retail, zero_at, zero_size, 4);
	if (file_sha256(temp, zero_at, zero_size, zeros, digest, &made_size) || made_size != h->retail->build_size ||
		memcmp(digest, h->retail->build_sha256, 32))
	{
		DeleteFileA(temp);
		say_in(COLOUR_RED, "default.xbe.new didn't read back right: nothing changed.");
		retail_free(&r);
		return 1;
	}
	say("  written and checked (%lu KB)", made_size / 1024);
	/* the retail XBE kept, as it was */
	if (h->state == RETAIL && !exists(backup))
	{
		retail_original(&r);
		if (write_checked(backup, r.data, r.size))
		{
			DeleteFileA(temp);
			say_in(COLOUR_RED, "Couldn't keep the retail XBE: nothing changed.");
			retail_free(&r);
			return 1;
		}
		say("Kept the retail XBE as default_retail.xbe");
	}
	retail_free(&r);
	/* and the new one in its place */
	if (set_aside(h, main) || !MoveFileA(temp, main))
	{
		say_in(COLOUR_RED, "Couldn't put default.xbe.new in place of default.xbe.");
		say("(The retail XBE is safe in default_retail.xbe.)");
		return 1;
	}
	say_in(COLOUR_GREEN, "Done: OpenCE is this folder's default.xbe.");
	return 0;
}

static int restore(struct halo *h)
{
	char main[200], backup[200], line[SCREEN_COLUMNS + 8];
	struct retail r;
	text_print(main, sizeof(main), "%s\\default.xbe", h->path);
	text_print(backup, sizeof(backup), "%s\\default_retail.xbe", h->path);
	text_print(line, sizeof(line), "Putting the retail Halo back in %s", h->path);
	heading(line);
	log_clear();
	say("Putting the retail Halo back in %s", h->path);
	if (retail_read(backup, &r) || r.xbe != h->retail)
	{
		say_in(COLOUR_RED, "default_retail.xbe couldn't be read, or changed: look again.");
		retail_free(&r);
		return 1;
	}
	retail_original(&r);
	if (h->state == OTHER && set_aside(h, main))
	{
		say_in(COLOUR_RED, "Couldn't move the XBE that's there out of the way.");
		retail_free(&r);
		return 1;
	}
	if (write_checked(main, r.data, r.size))
	{
		say_in(COLOUR_RED, "Couldn't write default.xbe: try again.");
		retail_free(&r);
		return 1;
	}
	retail_free(&r);
	say_in(COLOUR_GREEN, "Done: the retail Halo is back (default_retail.xbe is kept).");
	return 0;
}

static void launch(char const *device)
{
	screen_hand_over();
	XLaunchXBE(device);
	/* (only if the path didn't convert) */
	screen_init();
	heading("Couldn't start it.");
}

static void launch_halo(struct halo *h)
{
	char device[240];
	text_print(device, sizeof(device), "%s\\default.xbe", h->device);
	log_file("starting");
	log_file(device);
	launch(device);
}

/* ---- the list ---- */

#define LIST_TOP 3
#define LIST_ROWS 16
static int selected, first_shown;

static void show_list(void)
{
	int i, ready_count = 0, row;
	heading(NULL);
	for (i = 0; i < halo_count; i++)
		ready_count += ready(&halos[i]);
	{
		char counts[40];
		text_print(counts, sizeof(counts), "%d Halo found, %d ready", halo_count, ready_count);
		screen_text(0, SCREEN_COLUMNS - (int)strlen(counts), COLOUR_GREY, "%s", counts);
	}
	if (!package_ok)
	{
		screen_text(LIST_TOP, 0, COLOUR_RED, "%s", package_error);
		screen_text(LIST_TOP + 1, 0, COLOUR_WHITE, "Put opence.oxpk beside this installer.");
	}
	else if (!halo_count)
	{
		screen_text(LIST_TOP, 0, COLOUR_WHITE, "No Halo found in E:, F: or G: (in their folders, or Games folders).");
	}
	if (selected < first_shown)
		first_shown = selected;
	if (selected >= first_shown + LIST_ROWS)
		first_shown = selected - LIST_ROWS + 1;
	for (row = 0; package_ok && row < LIST_ROWS && first_shown + row < halo_count; row++)
	{
		struct halo *h = &halos[first_shown + row];
		int r = LIST_TOP + row, is_selected = first_shown + row == selected;
		unsigned long colour = ready(h) ? COLOUR_GREEN : COLOUR_GREY;
		size_t n = strlen(h->path);
		screen_row(r, is_selected ? COLOUR_BAR : COLOUR_BLACK);
		if (n > 40)
			screen_text(r, 1, colour, "...%s", h->path + n - 37);
		else
			screen_text(r, 1, colour, "%s", h->path);
		screen_text(r, 42, colour, "%.30s", h->note);
	}
	if (halo_count > LIST_ROWS)
		screen_text(LIST_TOP + LIST_ROWS, 1, COLOUR_DIM, "%d-%d of %d   (LT/RT: a page)", first_shown + 1,
			first_shown + row, halo_count);
	if (halo_count && package_ok)
	{
		struct halo *h = &halos[selected];
		char actions[SCREEN_COLUMNS + 1];
		int at = 0;
		screen_text(21, 0, COLOUR_WHITE, "%s", h->detail[0]);
		screen_text(22, 0, COLOUR_WHITE, "%s", h->detail[1]);
		actions[0] = 0;
		if (ready(h))
			at += text_print(actions + at, sizeof(actions) - at, "A: %s   ", h->state == INSTALLED ?
				(h->up_to_date ? "install again" : "update") : "install");
		if (h->state == INSTALLED || h->state == OTHER || h->state == MISSING)
			at += text_print(actions + at, sizeof(actions) - at, "X: retail Halo back   ");
		if (ready(h) && h->state != MISSING)
			at += text_print(actions + at, sizeof(actions) - at, "START: play   ");
		text_print(actions + at, sizeof(actions) - at, "B: dashboard");
		screen_text(24, 0, COLOUR_YELLOW, "%s", actions);
	}
	else
		screen_text(24, 0, COLOUR_YELLOW, "B: dashboard");
	screen_show();
}

/* after an install or a restore: the folder looked at again, the list kept on it */
static void look_again(struct halo *h)
{
	char path[160];
	int i;
	flush_drive(h->path);
	examine(h);
	text_print(path, sizeof(path), "%s", h->path);
	sort_halos();
	for (i = 0; i < halo_count; i++)
	{
		if (!strcmp(halos[i].path, path))
			selected = i;
	}
}

int main(void)
{
	nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
	nxMountDrive('F', "\\Device\\Harddisk0\\Partition6\\");
	nxMountDrive('G', "\\Device\\Harddisk0\\Partition7\\");
	screen_init();
	SDL_Init(SDL_INIT_GAMECONTROLLER);
	heading("Reading the package...");
	log_file("---- " TITLE);
	package_ok = !package_load(PACKAGE_PATH, &package, package_error);
	log_file(package_ok ? package.name : package_error);
	find_halos();
	for (;;)
	{
		struct halo *h = halo_count ? &halos[selected] : NULL;
		int b;
		show_list();
		b = wait_input();
		if (b == UP && selected > 0)
			selected--;
		else if (b == DOWN && selected + 1 < halo_count)
			selected++;
		else if (b == PAGE_UP)
			selected = selected > LIST_ROWS ? selected - LIST_ROWS : 0;
		else if (b == PAGE_DOWN && halo_count)
			selected = selected + LIST_ROWS < halo_count ? selected + LIST_ROWS : halo_count - 1;
		else if (b == B)
		{
			log_file("to the dashboard");
			launch(NULL);
		}
		else if (h && package_ok && b == A && ready(h))
		{
			int failed = install(h);
			look_again(h);
			say_in(COLOUR_YELLOW, failed ? "B: back" : "START: play it   B: back");
			while ((b = wait_input()) != B && !(b == START && !failed))
				;
			if (b == START)
				launch_halo(&halos[selected]);
		}
		else if (h && b == X && (h->state == INSTALLED || h->state == OTHER || h->state == MISSING))
		{
			restore(h);
			look_again(h);
			say_in(COLOUR_YELLOW, "B: back");
			while (wait_input() != B)
				;
		}
		else if (h && b == START && ready(h) && h->state != MISSING)
			launch_halo(h);
	}
	return 0;
}
