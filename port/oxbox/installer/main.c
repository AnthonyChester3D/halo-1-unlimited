/* main.c: the Halo 1 Unlimited installer, an XBE of its own (nxdk; port/oxbox/installer/README.md). It makes OpenCE
from the player's own clean retail Halo XBE, with the package beside it (opence.oxpk: tools/mkpackage.py) and the
pictures from the game's own ui.map (pictures.c), keeping the retail XBE as default_retail.xbe. Nothing of
Microsoft's code or of Halo's art is in the installer or the package.

	A      install (or update) OpenCE in the Halo folder chosen
	X      put the retail Halo back (from default_retail.xbe)
	START  play what's in the folder
	B      back to the dashboard */
#include <hal/debug.h>
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
#include "sha256.h"
#include "text.h"

#define TITLE "Halo 1 Unlimited installer"
#define PACKAGE_PATH "D:\\opence.oxpk"
#define LOG_PATH "D:\\installer_log.txt"
#define HALO_TITLE_ID 0x4D530004ul
#define MAX_XBE (16 * 1024 * 1024)

/* ---- the screen: 24 lines of 62 (nxdk's debugPrint wraps a line after 63 characters, and starts again at the top
after 24 lines) ---- */

#define ROWS 24
#define COLUMNS 62
static char screen[ROWS][COLUMNS + 1];
static int log_top; /* (the lines below this one are the log, which scrolls) */

static void draw(void)
{
	int r;
	debugClearScreen();
	for (r = 0; r < ROWS; r++)
		debugPrint("%s\n", screen[r]);
}

static void line(int row, char const *format, ...)
{
	va_list list;
	if (row < 0 || row >= ROWS)
		return;
	va_start(list, format);
	text_vprint(screen[row], COLUMNS + 1, format, list);
	va_end(list);
}

static void log_file(char const *text)
{
	FILE *file = fopen(LOG_PATH, "a");
	if (file)
	{
		fprintf(file, "%s\n", text);
		fclose(file);
	}
}

/* a line in the log at the bottom (and the log file), drawn at once */
static void say(char const *format, ...)
{
	char text[256];
	va_list list;
	int r;
	va_start(list, format);
	text_vprint(text, sizeof(text), format, list);
	va_end(list);
	log_file(text);
	for (r = log_top; r < ROWS - 1; r++)
		memcpy(screen[r], screen[r + 1], sizeof(screen[r]));
	text_print(screen[ROWS - 1], COLUMNS + 1, "%s", text);
	draw();
}

static void clear_log(void)
{
	int r;
	for (r = log_top; r < ROWS; r++)
		screen[r][0] = 0;
}

static void pictures_step(char const *step)
{
	say("  %s", step);
}

/* ---- the controllers ---- */

enum { NONE, UP, DOWN, A, B, X, START };

static int button(void)
{
	SDL_Event e;
	while (SDL_PollEvent(&e))
	{
		if (e.type == SDL_CONTROLLERDEVICEADDED)
			SDL_GameControllerOpen(e.cdevice.which);
		else if (e.type == SDL_CONTROLLERBUTTONDOWN)
		{
			switch (e.cbutton.button)
			{
			case SDL_CONTROLLER_BUTTON_DPAD_UP: return UP;
			case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return DOWN;
			case SDL_CONTROLLER_BUTTON_A: return A;
			case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: return B;
			case SDL_CONTROLLER_BUTTON_X: return X;
			case SDL_CONTROLLER_BUTTON_START: return START;
			}
		}
	}
	return NONE;
}

static int wait_button(void)
{
	int b;
	while ((b = button()) == NONE)
		Sleep(16);
	return b;
}

/* ---- files ---- */

static unsigned long u32(unsigned char const *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned long)p[3] << 24);
}

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

/* ---- the Halo folders ---- */

enum
{
	RETAIL,    /* default.xbe is a clean retail Halo the package knows */
	INSTALLED, /* default.xbe is OpenCE, and default_retail.xbe a clean retail Halo */
	OTHER,     /* default.xbe is another XBE, and default_retail.xbe a clean retail Halo */
	MISSING,   /* no default.xbe, and default_retail.xbe a clean retail Halo */
	UNKNOWN    /* no clean retail Halo the package knows */
};

struct halo
{
	char path[200];   /* "E:\Games\Halo" */
	char device[240]; /* "\Device\Harddisk0\Partition1\Games\Halo" */
	int state;
	struct package_xbe const *retail; /* (the package's build for its clean retail XBE) */
	int backup_in_the_way;            /* default_retail.xbe is there and isn't a clean retail Halo */
	char what[COLUMNS + 1];
};

#define MAX_HALOS 12
static struct halo halos[MAX_HALOS];
static int halo_count;
static struct package package;
static int package_ok;
static char package_error[256];

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

static int is_halo_xbe(char const *path)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	unsigned char header[4096];
	unsigned long base, cert;
	DWORD got;
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	got = 0;
	ReadFile(file, header, sizeof(header), &got, NULL);
	CloseHandle(file);
	if (got < 0x180 || memcmp(header, "XBEH", 4))
		return 0;
	base = u32(header + 0x104);
	cert = u32(header + 0x118) - base;
	return cert + 12 <= got && u32(header + cert + 8) == HALO_TITLE_ID;
}

/* the package's build for this file's retail XBE, if it's one (and whether it's an OpenCE build) */
static struct package_xbe const *look_at(char const *path, int *present, int *opence)
{
	unsigned long size;
	unsigned char *data = read_whole(path, &size), digest[32];
	struct package_xbe const *xbe = NULL;
	*present = data != NULL || exists(path);
	*opence = 0;
	if (!data)
		return NULL;
	sha256(data, size, digest);
	if (package_ok)
		xbe = package_find(&package, digest);
	if (!xbe)
	{
		unsigned long i;
		for (i = 0; i + 8 <= size && !*opence; i++)
			*opence = data[i] == 'O' && !memcmp(data + i, "OXWORDS1", 8);
	}
	free(data);
	return xbe;
}

static void examine(struct halo *halo)
{
	char path[260];
	int present, opence, backup_present, backup_opence;
	struct package_xbe const *main_retail, *backup_retail;
	text_print(path, sizeof(path), "%s\\default.xbe", halo->path);
	main_retail = look_at(path, &present, &opence);
	text_print(path, sizeof(path), "%s\\default_retail.xbe", halo->path);
	backup_retail = look_at(path, &backup_present, &backup_opence);
	halo->backup_in_the_way = backup_present && !backup_retail;
	if (main_retail)
	{
		halo->state = RETAIL;
		halo->retail = main_retail;
		text_print(halo->what, sizeof(halo->what), "  clean retail %s", main_retail->name);
	}
	else if (backup_retail)
	{
		halo->state = !present ? MISSING : opence ? INSTALLED : OTHER;
		halo->retail = backup_retail;
		text_print(halo->what, sizeof(halo->what), "  %s (retail kept: %s)",
			halo->state == INSTALLED ? "OpenCE" : halo->state == OTHER ? "another XBE" : "no default.xbe",
			backup_retail->name);
	}
	else
	{
		halo->state = UNKNOWN;
		halo->retail = NULL;
		text_print(halo->what, sizeof(halo->what), "  %s", !package_ok ? "(no package to install)" :
			opence ? "an OpenCE build, but no clean retail XBE kept" :
			"not a clean retail Halo XBE this package knows");
	}
}

static void add_halo(char const *folder)
{
	char path[260];
	int i;
	if (halo_count == MAX_HALOS)
		return;
	for (i = 0; i < halo_count; i++)
	{
		if (!_stricmp(halos[i].path, folder))
			return;
	}
	text_print(path, sizeof(path), "%s\\default.xbe", folder);
	if (!is_halo_xbe(path))
	{
		text_print(path, sizeof(path), "%s\\default_retail.xbe", folder);
		if (!is_halo_xbe(path))
			return;
	}
	text_print(halos[halo_count].path, sizeof(halos[halo_count].path), "%s", folder);
	to_device(folder, halos[halo_count].device, sizeof(halos[halo_count].device));
	examine(&halos[halo_count]);
	halo_count++;
}

/* each folder in this one with a Halo in it */
static void scan(char const *folder)
{
	WIN32_FIND_DATAA found;
	char pattern[260], path[260];
	HANDLE find;
	text_print(pattern, sizeof(pattern), "%s\\*", folder);
	find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && found.cFileName[0] != '.')
		{
			text_print(path, sizeof(path), "%s\\%s", folder, found.cFileName);
			add_halo(path);
		}
	} while (FindNextFileA(find, &found));
	FindClose(find);
}

static void find_halos(void)
{
	char own[260], *slash;
	int i;
	halo_count = 0;
	/* the folder the installer is in, if it's in Halo's (E:\Games\Halo\OpenCE installer: E:\Games\Halo) */
	text_print(own, sizeof(own), "%.*s", XeImageFileName->Length, XeImageFileName->Buffer);
	for (i = 0; i < 2 && (slash = strrchr(own, '\\')); i++)
		*slash = 0;
	{
		char path[260];
		if (from_device(own, path, sizeof(path)))
			add_halo(path);
	}
	for (i = 0; i < 3; i++)
	{
		char root[8], games[16];
		text_print(root, sizeof(root), "%c:", partitions[i][0]);
		text_print(games, sizeof(games), "%c:\\Games", partitions[i][0]);
		scan(games);
		scan(root);
	}
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
	char temp[280];
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
static int set_aside(struct halo *halo, char const *main)
{
	char old[280];
	int n;
	if (!exists(main))
		return 0;
	if (halo->state != OTHER)
		return !DeleteFileA(main);
	for (n = 1; n < 100; n++)
	{
		if (n == 1)
			text_print(old, sizeof(old), "%s\\default_old.xbe", halo->path);
		else
			text_print(old, sizeof(old), "%s\\default_old%d.xbe", halo->path, n);
		if (!exists(old))
		{
			say("Kept the XBE that was there as %s", strrchr(old, '\\') + 1);
			return !MoveFileA(main, old);
		}
	}
	return 1;
}

static int install(struct halo *halo)
{
	char main[260], backup[260], temp[280], ui_map[260], error[256];
	unsigned long size, zero_at[4], zero_size[4], made_size;
	unsigned char *retail, digest[32];
	int zeros, have_pictures;
	struct writer w;

	text_print(main, sizeof(main), "%s\\default.xbe", halo->path);
	text_print(backup, sizeof(backup), "%s\\default_retail.xbe", halo->path);
	text_print(temp, sizeof(temp), "%s\\default.xbe.new", halo->path);
	text_print(ui_map, sizeof(ui_map), "%s\\maps\\ui.map", halo->path);
	clear_log();
	say("Installing %s", package.name);
	say("in %s", halo->path);
	if (halo->state == RETAIL && halo->backup_in_the_way)
	{
		say("default_retail.xbe is already there, and isn't a clean");
		say("retail Halo: move it out of the folder first.");
		return 1;
	}
	/* the clean retail XBE, checked again */
	retail = read_whole(halo->state == RETAIL ? main : backup, &size);
	if (!retail)
	{
		say("Couldn't read the retail XBE.");
		return 1;
	}
	sha256(retail, size, digest);
	if (memcmp(digest, halo->retail->retail_sha256, 32))
	{
		say("The retail XBE changed: look again (B).");
		free(retail);
		return 1;
	}
	if (!free_space(halo->path, (unsigned long long)halo->retail->build_size + size + 1024 * 1024))
	{
		say("Not enough room on the drive (%lu KB needed).", (halo->retail->build_size + size) / 1024 + 1024);
		free(retail);
		return 1;
	}
	/* the pictures, from the game's own ui.map */
	say("Making the pictures from maps\\ui.map:");
	pictures_progress = pictures_step;
	have_pictures = !pictures_make(ui_map, pictures, header, error);
	if (!have_pictures)
		say("  no pictures (%s): they'll be blank", error);
	/* the build, beside the old XBE */
	say("Making OpenCE from %s...", halo->retail->name);
	w.file = create(temp);
	if (w.file == INVALID_HANDLE_VALUE)
	{
		say("Couldn't create default.xbe.new.");
		free(retail);
		return 1;
	}
	if (package_make(&package, halo->retail, retail, have_pictures ? pictures : NULL, have_pictures ? header : NULL,
		write_part, &w, error))
	{
		CloseHandle(w.file);
		DeleteFileA(temp);
		say("%s", error);
		free(retail);
		return 1;
	}
	if (!finish(w.file))
	{
		DeleteFileA(temp);
		say("Couldn't finish writing default.xbe.new.");
		free(retail);
		return 1;
	}
	zeros = package_pictures(&package, halo->retail, zero_at, zero_size, 4);
	if (file_sha256(temp, zero_at, zero_size, zeros, digest, &made_size) || made_size != halo->retail->build_size ||
		memcmp(digest, halo->retail->build_sha256, 32))
	{
		DeleteFileA(temp);
		say("default.xbe.new didn't read back right: nothing changed.");
		free(retail);
		return 1;
	}
	say("  written and checked (%lu KB)", made_size / 1024);
	/* the retail XBE kept */
	if (halo->state == RETAIL && !exists(backup))
	{
		if (write_checked(backup, retail, size))
		{
			DeleteFileA(temp);
			say("Couldn't keep the retail XBE: nothing changed.");
			free(retail);
			return 1;
		}
		say("Kept the retail XBE as default_retail.xbe");
	}
	free(retail);
	/* and the new one in its place */
	if (set_aside(halo, main) || !MoveFileA(temp, main))
	{
		say("Couldn't put default.xbe.new in place of default.xbe.");
		say("(The retail XBE is safe in default_retail.xbe.)");
		return 1;
	}
	say("Done: OpenCE is this folder's default.xbe.");
	return 0;
}

static int restore(struct halo *halo)
{
	char main[260], backup[260];
	unsigned char *retail, digest[32];
	unsigned long size;
	text_print(main, sizeof(main), "%s\\default.xbe", halo->path);
	text_print(backup, sizeof(backup), "%s\\default_retail.xbe", halo->path);
	clear_log();
	say("Putting the retail Halo back in %s", halo->path);
	retail = read_whole(backup, &size);
	if (!retail)
	{
		say("Couldn't read default_retail.xbe.");
		return 1;
	}
	sha256(retail, size, digest);
	if (memcmp(digest, halo->retail->retail_sha256, 32))
	{
		say("default_retail.xbe changed: look again (B).");
		free(retail);
		return 1;
	}
	if (halo->state == OTHER && set_aside(halo, main))
	{
		say("Couldn't move the XBE that's there out of the way.");
		free(retail);
		return 1;
	}
	if (write_checked(main, retail, size))
	{
		say("Couldn't write default.xbe: try again.");
		free(retail);
		return 1;
	}
	free(retail);
	say("Done: the retail Halo is back (default_retail.xbe is kept).");
	return 0;
}

static void launch(struct halo *halo)
{
	char device[280];
	text_print(device, sizeof(device), "%s\\default.xbe", halo->device);
	say("Starting %s\\default.xbe", halo->path);
	XLaunchXBE(device);
	say("Couldn't start it.");
}

/* ---- the menu ---- */

static int selected;

/* the menu's lines (above the log) */
static void menu_top(void)
{
	int row = 0, i;
	for (i = 0; i < log_top && i < ROWS; i++)
		screen[i][0] = 0;
	line(row++, TITLE);
	line(row++, "Package: %s", package_ok ? package.name : "none");
	line(row++, "--------------------------------------------------------------");
	if (!package_ok)
	{
		line(row++, "%s", package_error);
		line(row++, "Put opence.oxpk beside this installer.");
	}
	if (!halo_count)
		line(row++, "No Halo found in E:, F:, G: or their Games folders.");
	for (i = 0; i < halo_count && row < 16; i++)
	{
		line(row++, "%s %s", i == selected ? ">" : " ", halos[i].path);
		line(row++, "%s", halos[i].what);
	}
	row++;
	if (halo_count)
	{
		struct halo *h = &halos[selected];
		int can_install = package_ok && h->state != UNKNOWN;
		int can_restore = h->state == INSTALLED || h->state == OTHER || h->state == MISSING;
		line(row++, "%s%s%sB: dashboard", can_install ? (h->state == INSTALLED ? "A: update  " : "A: install  ") : "",
			can_restore ? "X: retail Halo back  " : "", h->state != MISSING ? "START: play  " : "");
	}
	else
		line(row++, "B: dashboard");
	log_top = row + 1;
}

static void menu(void)
{
	memset(screen, 0, sizeof(screen));
	menu_top();
	draw();
}

int main(void)
{
	int b;
	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
	nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
	nxMountDrive('F', "\\Device\\Harddisk0\\Partition6\\");
	nxMountDrive('G', "\\Device\\Harddisk0\\Partition7\\");
	SDL_Init(SDL_INIT_GAMECONTROLLER);
	memset(screen, 0, sizeof(screen));
	line(0, TITLE);
	line(2, "Reading the package and looking for Halo...");
	draw();
	log_file("---- " TITLE);
	package_ok = !package_load(PACKAGE_PATH, &package, package_error);
	log_file(package_ok ? package.name : package_error);
	find_halos();
	for (;;)
	{
		struct halo *h;
		menu();
		b = wait_button();
		h = halo_count ? &halos[selected] : NULL;
		if (b == UP && selected > 0)
			selected--;
		else if (b == DOWN && selected + 1 < halo_count)
			selected++;
		else if (b == B)
			XLaunchXBE(NULL);
		else if (h && b == A && package_ok && h->state != UNKNOWN)
		{
			int failed = install(h);
			flush_drive(h->path);
			examine(h);
			menu_top();
			say(failed ? "B: back" : "START: play it   B: back");
			while ((b = wait_button()) != B && !(b == START && !failed))
				;
			if (b == START)
				launch(h);
		}
		else if (h && b == X && (h->state == INSTALLED || h->state == OTHER || h->state == MISSING))
		{
			restore(h);
			flush_drive(h->path);
			examine(h);
			menu_top();
			say("B: back");
			while (wait_button() != B)
				;
		}
		else if (h && b == START && h->state != MISSING)
			launch(h);
	}
	return 0;
}
