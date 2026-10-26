/*
OXBOX_ENTRY.C

The proof of concept's entry point and log. The XBE is the retail Halo XBE with OpenCE's game code added in a new
section: the retail startup (mainXapiStartup) initializes XAPI, the C runtime and the libraries as usual, then
calls oxbox_entry where it called the retail main; oxbox_entry runs OpenCE's main (source/shell/shell_xbox.c).
*/

#include "platform.h"

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "oxbox_overlay.h"

int main(void);
int halo_linux_vsnprintf(char *buffer, size_t count, const char *format, va_list arguments);
BOOL WINAPI FlushFileBuffers(HANDLE file);

/* ---------- the log: oxbox_log.txt beside the XBE, and a ring in memory (readable with a debugger) */

char oxbox_log_ring[0x4000];   /* (a power of two) */
unsigned long oxbox_log_ring_position;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static int log_opened;
unsigned long oxbox_stage;   /* how far start-up got (a debugger reads it) */

/* a byte to the debug serial port (COM1, 0x3F8: xemu's isa-serial when started with one). A real Xbox has no
UART there: its line status register reads 0xFF, and then the port is left alone. */
static int serial_state;   /* 0 = not checked, 1 = present, 2 = absent */

static void serial_put(char c)
{
	unsigned char status;
	int tries = 1000;

	if (!serial_state)
	{
		__asm__ volatile ("inb %w1, %0" : "=a"(status) : "Nd"(0x3FD));
		serial_state = status != 0xFF && (status & 0x60) == 0x60 ? 1 : 2;
	}
	if (serial_state != 1)
		return;
	do
		__asm__ volatile ("inb %w1, %0" : "=a"(status) : "Nd"(0x3FD));
	while (!(status & 0x20) && --tries);
	__asm__ volatile ("outb %0, %w1" : : "a"(c), "Nd"(0x3F8));
}

static void log_write(const char *text, unsigned long length)
{
	unsigned long index, written;

	for (index = 0; index < length; index++)
		serial_put(text[index]);

	for (index = 0; index < length; index++)
		oxbox_log_ring[(oxbox_log_ring_position++) & (sizeof(oxbox_log_ring) - 1)] = text[index];
	if (!log_opened)
	{
		log_opened = 1;
		log_file = CreateFileA("d:\\oxbox_log.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
		/* (D: can't be written, as a disc or an ISO: T:, Halo's title data on the hard drive, E:\TDATA\4d530004) */
		if (log_file == INVALID_HANDLE_VALUE)
		{
			log_file = CreateFileA("T:\\oxbox_log.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, NULL);
			if (log_file != INVALID_HANDLE_VALUE)
			{
				static const char note[] = "oxbox: D: can't be written (a disc or an ISO?): this log is T:\\oxbox_log.txt "
					"(E:\\TDATA\\4d530004), debug.txt beside it\r\n";
				unsigned long done;

				WriteFile(log_file, note, sizeof(note) - 1, &done, NULL);
			}
		}
		if (log_file == INVALID_HANDLE_VALUE)
		{
			char note[64];
			unsigned long error = GetLastError(), i, n = 0;
			const char *prefix = "oxbox: d:\\oxbox_log.txt: error ";

			for (i = 0; prefix[i]; i++) note[n++] = prefix[i];
			for (i = 1000000000; i; i /= 10) if (error >= i || i == 1) note[n++] = (char)('0' + error / i % 10);
			note[n++] = '\r'; note[n++] = '\n';
			for (i = 0; i < n; i++) serial_put(note[i]);
		}
	}
	if (log_file != INVALID_HANDLE_VALUE)
	{
		/* flushed every 2 seconds at most (a flush per line could stall the game on the hard drive): a crash
		loses at most the last 2 seconds */
		static unsigned long last_flush;
		unsigned long now = GetTickCount();

		WriteFile(log_file, text, length, &written, NULL);
		if (now - last_flush >= 2000)
		{
			FlushFileBuffers(log_file);
			last_flush = now;
		}
	}
}

/* the ring alone (debug.txt's lines, which have their own file) */
void oxbox_log_text(const char *text, unsigned long length)
{
	unsigned long index;

	for (index = 0; index < length; index++)
		serial_put(text[index]);

	for (index = 0; index < length; index++)
		oxbox_log_ring[(oxbox_log_ring_position++) & (sizeof(oxbox_log_ring) - 1)] = text[index];
}

void platform_log(const char *format, ...)
{
	char line[1024];
	va_list arguments;
	int length;

	strcpy(line, "oxbox: ");
	va_start(arguments, format);
	length = halo_linux_vsnprintf(line + 7, sizeof(line) - 9, format, arguments);
	va_end(arguments);
	if (length < 0) length = 0;
	length = (int)strlen(line);
	if (length > (int)sizeof(line) - 2) length = sizeof(line) - 2;
	line[length++] = '\r';
	line[length++] = '\n';
	log_write(line, length);
}

void platform_unimplemented(const char *name)
{
	static const char *reported[256];
	static int count;
	int index;

	for (index = 0; index < count; index++)
		if (reported[index] == name)
			return;
	if (count < 256)
		reported[count++] = name;
	platform_log("not provided: %s", name);
}

/* ---------- kernel exports the retail XBE does not import (its thunk table is fixed) */

void *oxbox_kernel_export(unsigned long ordinal)
{
	const unsigned char *base = (const unsigned char *)0x80010000;
	unsigned long pe = *(const unsigned long *)(base + 0x3C);
	unsigned long export_rva = *(const unsigned long *)(base + pe + 0x78);
	const unsigned long *directory = (const unsigned long *)(base + export_rva);
	unsigned long first = directory[4], count = directory[5];
	const unsigned long *functions = (const unsigned long *)(base + directory[7]);

	if (ordinal < first || ordinal - first >= count)
		return NULL;
	return (void *)(base + functions[ordinal - first]);
}

/* ---------- D: is the XBE's own folder

A dashboard maps D: to the folder of the XBE it launches; booted by the kernel as the dashboard itself
(C:\\xboxdash.xbe, as the xemu test disks do), D: stays the DVD drive, and the game finds no d:\\maps. So D: is
pointed at the folder in the kernel's XeImageFileName, as XAPI's XapiMapLetterToDirectory does. */

typedef struct { unsigned short length, maximum_length; char *buffer; } oxbox_object_string;

/* d:\ and the XBE's own file name; the path the kernel loaded it from */
static char oxbox_image_name[64] = "d:\\";
static char oxbox_image_path[260];

static void map_d_to_image_folder(void)
{
	oxbox_object_string *image = oxbox_kernel_export(326);   /* XeImageFileName */
	LONG (WINAPI *create_link)(oxbox_object_string *, oxbox_object_string *) = oxbox_kernel_export(67);
	LONG (WINAPI *delete_link)(oxbox_object_string *) = oxbox_kernel_export(69);
	static char folder[260];
	static char link_name[] = "\\??\\D:";
	oxbox_object_string link = { sizeof(link_name) - 1, sizeof(link_name), link_name }, target;
	int length, last_slash = -1, i;
	LONG status;

	if (!image || !image->buffer || !create_link || !delete_link)
		return;
	length = image->length < sizeof(folder) - 2 ? image->length : sizeof(folder) - 2;
	for (i = 0; i < length; i++)
	{
		folder[i] = image->buffer[i];
		if (folder[i] == '\\')
			last_slash = i;
	}
	if (last_slash < 0)
		return;
	folder[last_slash + 1] = 0;
	target.length = (unsigned short)(last_slash + 1);
	target.maximum_length = target.length + 1;
	target.buffer = folder;
	/* (its file name, for oxbox_effects_image) */
	for (i = last_slash + 1; i < length && i - last_slash < (int)sizeof(oxbox_image_name) - 3; i++)
		oxbox_image_name[3 + i - last_slash - 1] = image->buffer[i];
	/* (and its whole path, read without D: if d:\<name> can't be: own_xbe_open) */
	for (i = 0; i < length && i < (int)sizeof(oxbox_image_path) - 1; i++)
		oxbox_image_path[i] = image->buffer[i];
	oxbox_image_path[i] = 0;
	delete_link(&link);
	status = create_link(&link, &target);
	platform_log("D: is %s (status %08lx); the XBE: %s", folder, (unsigned long)status, oxbox_image_path);
}

/* ---------- reading the XBE's own file (the effects image, the pictures): by d:\<its name>, or else by the
path the kernel loaded it from (a launcher or BIOS that starts it in a way that leaves d:\<name> elsewhere) */

typedef struct { void *root; oxbox_object_string *name; unsigned long attributes; } oxbox_object_attributes;
typedef struct { LONG status; unsigned long information; } oxbox_io_status;

/* offset, size: read into buffer; TRUE when all of it was read */
static int own_xbe_read(unsigned long offset, void *buffer, unsigned long size)
{
	FILE *file = fopen(oxbox_image_name, "rb");
	static int reported;

	if (file)
	{
		int ok = fseek(file, (long)offset, SEEK_SET) == 0 && fread(buffer, 1, size, file) == size;

		fclose(file);
		return ok;
	}
	{
		LONG (WINAPI *nt_open)(HANDLE *, unsigned long, oxbox_object_attributes *, oxbox_io_status *, unsigned long,
			unsigned long) = oxbox_kernel_export(202);   /* NtOpenFile */
		LONG (WINAPI *nt_read)(HANDLE, HANDLE, void *, void *, oxbox_io_status *, void *, unsigned long,
			LARGE_INTEGER *) = oxbox_kernel_export(219);   /* NtReadFile */
		LONG (WINAPI *nt_close)(HANDLE) = oxbox_kernel_export(187);   /* NtClose */
		oxbox_object_string name;
		oxbox_object_attributes attributes;
		oxbox_io_status io;
		LARGE_INTEGER position;
		HANDLE handle;
		LONG status = -1;
		int ok = 0;

		if (nt_open && nt_read && nt_close && oxbox_image_path[0])
		{
			name.length = (unsigned short)strlen(oxbox_image_path);
			name.maximum_length = (unsigned short)(name.length + 1);
			name.buffer = oxbox_image_path;
			attributes.root = NULL;
			attributes.name = &name;
			attributes.attributes = 0x40;   /* OBJ_CASE_INSENSITIVE */
			/* (GENERIC_READ | SYNCHRONIZE, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE) */
			status = nt_open(&handle, 0x80100000, &attributes, &io, 1, 0x60);
			if (status >= 0)
			{
				position.QuadPart = offset;
				status = nt_read(handle, NULL, NULL, NULL, &io, buffer, size, &position);
				ok = status >= 0 && io.information == size;
				nt_close(handle);
			}
		}
		if (!reported)
		{
			reported = 1;
			platform_log("the XBE's own file: %s couldn't be opened; by the kernel's path %s: %s (status %08lx)",
				oxbox_image_name, oxbox_image_path, ok ? "read" : "failed", (unsigned long)status);
		}
		return ok;
	}
}

/* ---------- what the console runs: the kernel's version and the hardware's revisions. (Builds 24 and 25 also read
the BIOS flash, to log the BIOS's name and a checksum: real consoles hung there, on a black screen right after the
memory line, though xemu didn't. The flash is left alone.) */

unsigned short oxbox_kernel_build;

static void report_system(void)
{
	const unsigned short *version = oxbox_kernel_export(324);   /* XboxKrnlVersion */
	const unsigned char *hardware = oxbox_kernel_export(322);   /* XboxHardwareInfo: flags, GPU and MCP revisions */

	if (version)
		oxbox_kernel_build = version[2];
	platform_log("console: kernel %u.%u.%u.%u, hardware flags %08lx, GPU %02x, MCP %02x",
		version ? version[0] : 0, version ? version[1] : 0, version ? version[2] : 0, version ? version[3] : 0,
		hardware ? *(const unsigned long *)hardware : 0, hardware ? hardware[4] : 0, hardware ? hardware[5] : 0);
}

/* ---------- opence.ini (test build 24): settings a player can change without another XBE, read at start-up from the
XBE's folder (D:\opence.ini), then from the game's own save space (T:\opence.ini), whose settings win. Lines
"name = number"; anything else is ignored.
	audio_effects = 0    no DirectSound effects image (the audio chip's reverb program): for a console whose BIOS
	                     doesn't take it (a Stellar BIOS's player had to take it out to play)
	fps_counter = 1      the frame rate in the screen's bottom right corner, for testing (test build 27): Halo's own
	                     counter (display_framerate), frames a second over the last half second, green at 30 and
	                     up, red under
	bot_log = 1          the bots' developer log (test build 28, bots.c): a line every 10 seconds in this log, and
	                     opence_bot_log.txt beside it, the spots where bots gave up on where they were going
	perf_log = 1         the developers' performance and memory log (test build 31, oxbox_perf.c): a CSV file a map
	                     in perf\ beside the XBE; its own settings (perf_slow_frame_ms, perf_gpu_sample_ms) are
	                     read by it */
int oxbox_setting_audio_effects = 1;
int oxbox_setting_bot_log = 0;
static int oxbox_setting_perf_log = 0;
extern unsigned char display_framerate;   /* (main.c's, a boolean) */

/* a whole file written beside the logs (the XBE's folder, else T:); FALSE when neither could be written */
int oxbox_write_log_file(const char *name, const void *data, unsigned long size)
{
	static const char *const folders[] = { "d:\\", "T:\\" };
	unsigned i;

	for (i = 0; i < sizeof(folders) / sizeof(folders[0]); i++)
	{
		char path[128];
		HANDLE file;
		unsigned long written = 0, n = 0, k;

		for (k = 0; folders[i][k] && n < sizeof(path) - 1; k++)
			path[n++] = folders[i][k];
		for (k = 0; name[k] && n < sizeof(path) - 1; k++)
			path[n++] = name[k];
		path[n] = 0;
		file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			continue;
		WriteFile(file, data, size, &written, NULL);
		CloseHandle(file);
		return written == size;
	}
	return 0;
}

/* (a byte loop: the compiler makes strcmp against a constant into bcmp, which this build has no copy of) */
static int same_text(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *(volatile const char *)a == *b;
}

static void read_settings(const char *path)
{
	char text[1024], *p, *e;
	unsigned long got = 0;
	HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (h == INVALID_HANDLE_VALUE)
		return;
	if (!ReadFile(h, text, sizeof(text) - 1, &got, NULL))
		got = 0;
	CloseHandle(h);
	text[got] = 0;
	for (p = text; *p; p = e)
	{
		char name[32];
		int n = 0, value = 0, digits = 0;

		for (e = p; *e && *e != '\n'; e++) ;
		if (*e)
			*e++ = 0;
		while (*p == ' ' || *p == '\t')
			p++;
		while (*p && *p != '=' && *p != ' ' && *p != '\t' && n < (int)sizeof(name) - 1)
			name[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p), p++;
		name[n] = 0;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p++ != '=')
			continue;
		while (*p == ' ' || *p == '\t')
			p++;
#if defined(OXBOX_NET_TEST)
		/* (the online test's addresses: a.b.c.d, oxbox_nettest.c) */
		if (same_text(name, "net_test_host") || same_text(name, "net_test_dns") || same_text(name, "net_test_static"))
		{
			extern unsigned long oxbox_net_test_host, oxbox_net_test_dns, oxbox_net_test_static;
			unsigned long address = 0, part = 0;
			int parts = 0, part_digits = 0;

			for (;; p++)
			{
				if (*p >= '0' && *p <= '9' && part_digits < 3)
				{
					part = part * 10 + (unsigned long)(*p - '0');
					part_digits++;
					continue;
				}
				if (!part_digits || part > 255)
					break;
				address |= part << (8 * parts++);
				part = 0;
				part_digits = 0;
				if (*p != '.' || parts == 4)
					break;
			}
			if (parts == 4)
			{
				if (same_text(name, "net_test_host"))
					oxbox_net_test_host = address;
				else if (same_text(name, "net_test_static"))
					oxbox_net_test_static = address;
				else
					oxbox_net_test_dns = address;
				platform_log("settings: %s from %s", name, path);
			}
			continue;
		}
#endif
#if defined(OXBOX_NET_P2P)
		/* (internet play's lists: host:port, host:port ...; port/oxbox/net/p2p/oxbox_p2p_glue.c, port/linux/src/p2p.c) */
		if (same_text(name, "online_brokers") || same_text(name, "online_stun"))
		{
			extern char oxbox_online_brokers[256];
			extern void oxbox_config_set(const char *name, const char *value);
			char list[192];
			int length = 0;

			while (*p && *p != '\r' && length < (int)sizeof(list) - 3)
				list[length++] = *p++;
			while (length && (list[length - 1] == ' ' || list[length - 1] == '\t'))
				length--;
			list[length] = 0;
			if (same_text(name, "online_brokers"))
			{
				memcpy(oxbox_online_brokers, list, (size_t)length + 1);
			}
			else
			{
				char quoted[194];

				quoted[0] = '"';
				memcpy(quoted + 1, list, (size_t)length);
				quoted[length + 1] = '"';
				quoted[length + 2] = 0;
				oxbox_config_set("network.stun_servers", quoted);
			}
			platform_log("settings: %s = %s from %s", name, list, path);
			continue;
		}
#endif
		for (; *p >= '0' && *p <= '9' && digits < 6; p++, digits++)
			value = value * 10 + (*p - '0');
		if (!digits)
			continue;
		if (same_text(name, "audio_effects"))
			oxbox_setting_audio_effects = value != 0;
		else if (same_text(name, "fps_counter"))
			display_framerate = value != 0;
		else if (same_text(name, "bot_log"))
			oxbox_setting_bot_log = value != 0;
		else if (same_text(name, "perf_log"))
			oxbox_setting_perf_log = value != 0;
#if defined(OXBOX_NET_TEST)
		else if (same_text(name, "net_test"))
		{
			extern int oxbox_net_test;
			oxbox_net_test = value != 0;
		}
		else if (same_text(name, "net_test_bypass"))
		{
			extern int oxbox_net_test_bypass;
			oxbox_net_test_bypass = value != 0;
		}
		else if (same_text(name, "net_test_port"))
		{
			extern int oxbox_net_test_port;
			oxbox_net_test_port = value;
		}
#endif
#if defined(OXBOX_NET_P2P)
		else if (same_text(name, "online"))
		{
			extern int oxbox_online;
			oxbox_online = value != 0;
		}
		else if (same_text(name, "online_upnp"))
		{
			extern int oxbox_online_upnp;
			oxbox_online_upnp = value != 0;
		}
#if defined(OXBOX_NET_TEST)
		else if (same_text(name, "net_test_join"))
		{
			extern int oxbox_net_test_join;
			oxbox_net_test_join = value != 0;
		}
#endif
		else if (same_text(name, "online_tunnel_port"))
		{
			extern void oxbox_config_set(const char *name, const char *value);
			char number[8];
			int length = 0, rest = value;

			do
				number[length++] = (char)('0' + rest % 10), rest /= 10;
			while (rest && length < 6);
			/* (the digits the other way round) */
			{
				int index;

				for (index = 0; index < length / 2; index++)
				{
					char swap = number[index];

					number[index] = number[length - 1 - index];
					number[length - 1 - index] = swap;
				}
			}
			number[length] = 0;
			oxbox_config_set("network.tunnel_port", number);
		}
#endif
		else
			continue;
		platform_log("settings: %s from %s", name, path);
	}
}

/* ---------- start-up */

extern char __oxbox_image_start[], __oxbox_image_end[], __oxbox_bss_start[], __oxbox_bss_end[];
extern char __ovlz0_start[], __ovlz0_end[], __ovlz1_start[], __ovlz1_end[], __ovlz2_start[], __ovlz2_end[],
	__ovlz3_start[], __ovlz3_end[], __ovlz4_start[], __ovlz4_end[], __ovlz5_start[], __ovlz5_end[],
	__ovlz6_start[], __ovlz6_end[], __ovlz7_start[], __ovlz7_end[], __ovlz8_start[], __ovlz8_end[],
	__ovlz9_start[], __ovlz9_end[], __ovlz10_start[], __ovlz10_end[], __ovlz11_start[], __ovlz11_end[],
	__ovlz12_start[], __ovlz12_end[], __ovlz13_start[], __ovlz13_end[], __ovlz14_start[], __ovlz14_end[],
	__ovlz15_start[], __ovlz15_end[];
static char *const oxbox_overlay_zero[16][2] = {
	{ __ovlz0_start, __ovlz0_end }, { __ovlz1_start, __ovlz1_end }, { __ovlz2_start, __ovlz2_end },
	{ __ovlz3_start, __ovlz3_end }, { __ovlz4_start, __ovlz4_end }, { __ovlz5_start, __ovlz5_end },
	{ __ovlz6_start, __ovlz6_end }, { __ovlz7_start, __ovlz7_end }, { __ovlz8_start, __ovlz8_end },
	{ __ovlz9_start, __ovlz9_end }, { __ovlz10_start, __ovlz10_end }, { __ovlz11_start, __ovlz11_end },
	{ __ovlz12_start, __ovlz12_end }, { __ovlz13_start, __ovlz13_end }, { __ovlz14_start, __ovlz14_end },
	{ __ovlz15_start, __ovlz15_end },
};

int oxbox_entry(int argc, char **argv, char **envp)
{
	int result;
	MEMORYSTATUS memory;
	unsigned i;

	/* OpenCE's larger globals live over the retail game's code and globals, which nothing runs or reads: zero
	them before anything uses them (they are .bss) */
	/* (only the .bss parts: the rest of the regions holds OpenCE code and data the XBE loaded there,
	mine/gen_overlay.py) */
	for (i = 0; i < sizeof(oxbox_overlay_zero) / sizeof(oxbox_overlay_zero[0]); i++)
		if (oxbox_overlay_zero[i][1] > oxbox_overlay_zero[i][0])
			memset(oxbox_overlay_zero[i][0], 0, oxbox_overlay_zero[i][1] - oxbox_overlay_zero[i][0]);
	oxbox_stage = 1;
#if defined(OXBOX_MEMPROF)
	{
		void oxbox_memprof_initialize(void);
		oxbox_memprof_initialize();
	}
#endif
	map_d_to_image_folder();
	GlobalMemoryStatus(&memory);
	platform_log("OpenCE on the Xbox (proof of concept): image %p-%p, bss %p-%p",
		__oxbox_image_start, __oxbox_image_end, __oxbox_bss_start, __oxbox_bss_end);
	platform_log("memory: %lu KB total, %lu KB free", memory.dwTotalPhys / 1024, memory.dwAvailPhys / 1024);
	report_system();
	read_settings("D:\\opence.ini");
	read_settings("T:\\opence.ini");
	if (!oxbox_setting_audio_effects)
		platform_log("settings: no DirectSound effects image (audio_effects = 0)");
	/* (the developers' perf log: its section loaded and started only with perf_log = 1, oxbox_sdk.c) */
	if (oxbox_setting_perf_log)
	{
		void oxbox_perf_load(void);

		oxbox_perf_load();
	}
	oxbox_stage = 2;
	result = main();
	platform_log("main returned %d", result);
	oxbox_stage = 3;
	return result;
}

/* ---------- DirectSound's effects image, from the XBE's file

mine/mkxbe.py puts it after the sections (the loader leaves it on the disc) and its offset and size in this
marker; sound_dsound_xbox.c reads it for IDirectSound_DownloadEffectsImage, which copies it to the audio
processor, and frees it: 15 KB that a game no longer carries. */
static volatile struct { char magic[8]; unsigned long offset, size; } oxbox_effects_marker = { "OXFXIMG1", 0, 0 };

void *oxbox_effects_image(unsigned long *size)
{
	void *image;

	*size = 0;
	if (!oxbox_effects_marker.size)
	{
		platform_log("effects image: not in %s", oxbox_image_name);
		return NULL;
	}
	image = malloc(oxbox_effects_marker.size);
	if (image && !own_xbe_read(oxbox_effects_marker.offset, image, oxbox_effects_marker.size))
	{
		free(image);
		image = NULL;
	}
	if (!image)
	{
		platform_log("effects image: could not read %lu bytes at %lu of %s", oxbox_effects_marker.size,
			oxbox_effects_marker.offset, oxbox_image_name);
		return NULL;
	}
	*size = oxbox_effects_marker.size;
	return image;
}

void oxbox_effects_image_free(void *image)
{
	free(image);
}

/* OpenCE's pictures (16 KB each: the main menu's OPTIONS and PROFILES words, normal and highlighted, then the
postgame's 5 gametype pictures), after the effects image in the file (mine/mkxbe.py): picture `index` read into
buffer (a texture's memory) when it's loaded; nothing of them in memory otherwise. TRUE when read */
static volatile struct { char magic[8]; unsigned long offset, size; } oxbox_words_marker = { "OXWORDS1", 0, 0 };

#if defined(OXBOX_NET_P2P)
/* the online test builds' ONLINE GAMES header (32 KB, DXT3 512 x 64: mine/mkheader.py), the same way, after the
pictures (port/linux/game/hmp_online.c). TRUE when read */
static volatile struct { char magic[8]; unsigned long offset, size; } oxbox_online_marker = { "OXONLIN1", 0, 0 };

int oxbox_xbe_online_header_read(void *buffer, unsigned long size)
{
	if (!oxbox_online_marker.offset || size != oxbox_online_marker.size)
	{
		platform_log("pictures: the ONLINE GAMES header isn't in %s", oxbox_image_name);
		return 0;
	}
	return own_xbe_read(oxbox_online_marker.offset, buffer, size);
}
#endif

int oxbox_picture_read(long index, void *buffer, unsigned long size)
{
	int ok;

	if (index < 0 || size != 16384 || (unsigned long)(index + 1) * size > oxbox_words_marker.size)
	{
		platform_log("pictures: picture %ld isn't in %s", index, oxbox_image_name);
		return 0;
	}
	ok = own_xbe_read(oxbox_words_marker.offset + (unsigned long)index * size, buffer, size);
	if (!ok)
		platform_log("pictures: could not read picture %ld of %s", index, oxbox_image_name);
	return ok;
}
