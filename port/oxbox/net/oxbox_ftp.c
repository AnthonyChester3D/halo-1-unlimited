/*
OXBOX_FTP.C

A small FTP server for working on maps while the game runs (opence.ini ftp = 1; ftp_password = text, xbox by default): the game's
folder (D:\, the maps folder in it) as "/", in the menus only. It's a developer's tool, off by default.

- Its code and data are an XBE section of their own, OXFTP (link.ld, mkxbe.py), loaded only when ftp = 1
  (oxbox_sdk.c oxbox_ftp_frame): without it the game carries none of it.
- It runs in the menus: it starts when the main menu is up, and stops (its connections closed, its memory freed)
  while a map is loaded, so a match has all its memory. A transfer under way when a match starts is cut off.
- One client at a time, passive mode (PASV, EPSV): what FileZilla, WinSCP and Windows' own clients use. User name:
  anything; password: opence.ini ftp_password (as typed; xbox by default). Commands: USER PASS SYST FEAT OPTS PWD CWD CDUP TYPE MODE
  STRU PASV EPSV LIST NLST RETR STOR APPE REST SIZE MDTM DELE MKD RMD RNFR RNTO NOOP ABOR QUIT.
- A file the game has open for loading (a map being copied into the map cache) refuses to be written: the client
  gets an error, and nothing is half-written.
- A change in the maps folder (a file written, deleted or renamed) tells the game (oxbox_ftp_maps_changed): the map
  list is scanned again, and every folder map is copied into the map cache afresh the next time it's loaded
  (cache_files.c), so a new version of a map is what's played.
- Its own thread, at the game's priority, waiting in select() when idle; about 40 KB while it runs (a 16 KB stack,
  a 16 KB transfer buffer, the rest lwIP's own pools, which are there anyway).
*/

#include "lwip/opt.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "oxbox_kernel.h"

#include <string.h>

/* ---------- the calls it makes into the game's image (oxbox_entry.c, oxbox_sdk.c, the Xbox's XAPI) */

void platform_log(char const *format, ...);
int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);
int oxbox_lwip_address(unsigned long *address, int *dhcp);
extern volatile long oxbox_ftp_maps_changed;   /* (oxbox_sdk.c: the map list scanned again, maps re-cached) */
extern char oxbox_ftp_status[64];              /* (oxbox_sdk.c: the menus' line) */

#define INVALID_HANDLE_VALUE ((HANDLE)-1)
typedef struct { DWORD low, high; } FILETIME_OX;
HANDLE NTAPI CreateFileA(const char *name, DWORD access, DWORD share, PVOID security, DWORD disposition, DWORD flags, HANDLE templ);
int NTAPI ReadFile(HANDLE file, PVOID buffer, DWORD size, DWORD *got, PVOID overlapped);
int NTAPI WriteFile(HANDLE file, const void *buffer, DWORD size, DWORD *put, PVOID overlapped);
DWORD NTAPI SetFilePointer(HANDLE file, LONG distance, LONG *high, DWORD method);
DWORD NTAPI GetFileSize(HANDLE file, DWORD *high);
int NTAPI FlushFileBuffers(HANDLE file);
HANDLE NTAPI FindFirstFileA(const char *pattern, PVOID data);
int NTAPI FindNextFileA(HANDLE handle, PVOID data);
int NTAPI DeleteFileA(const char *name);
int NTAPI MoveFileA(const char *from, const char *to);
int NTAPI CreateDirectoryA(const char *name, PVOID security);
int NTAPI RemoveDirectoryA(const char *name);
DWORD NTAPI GetFileAttributesA(const char *name);
PVOID NTAPI LocalAlloc(DWORD flags, SIZE_T bytes);
PVOID NTAPI LocalFree(PVOID memory);

#define GENERIC_READ 0x80000000ul
#define GENERIC_WRITE 0x40000000ul
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define CREATE_ALWAYS 2
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define NO_ATTRIBUTES 0xFFFFFFFFul
/* (WIN32_FIND_DATA on the Xbox: the attributes at 0, the last write at 0x14, the size at 0x1c (high) and 0x20 (low),
the name at 0x2c, as hmp_share.c reads it) */
#define FIND_SIZE 0x200
#define FIND_ATTRIBUTES(f) (*(DWORD *)((f) + 0x00))
#define FIND_WRITE_TIME(f) ((FILETIME_OX *)((f) + 0x14))
#define FIND_SIZE_LOW(f) (*(DWORD *)((f) + 0x20))
#define FIND_NAME(f) ((char *)((f) + 0x2c))

/* ---------- constants */

enum
{
	FTP_STACK = 16 * 1024,
	FTP_BUFFER = 16 * 1024,
	FTP_LINE = 512,
	FTP_PATH = 256,
	FTP_IDLE_SECONDS = 600,        /* a client that says nothing this long is let go */
	FTP_DATA_SECONDS = 20          /* a data connection that doesn't come, or stalls, this long */
};

/* ---------- globals (all in OXFTP) */

static struct
{
	HANDLE thread;
	volatile long stop, running;
	int listener;
	unsigned short port;
	char password[33];
	/* the client */
	int control, passive, data;
	int logged_in, user_given;
	char cwd[FTP_PATH];            /* "/maps/CEX" */
	char rename_from[FTP_PATH];
	unsigned long rest;
	char line[FTP_LINE];
	int line_length;
	unsigned char *buffer;
	unsigned long address;
} ftp;

/* ---------- small helpers */

static int ftp_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ftp_ieq(const char *a, const char *b)
{
	while (*a && ftp_lower((unsigned char)*a) == ftp_lower((unsigned char)*b)) { a++; b++; }
	return !*a && !*b;
}

static void ftp_close(int *s)
{
	if (*s >= 0)
		lwip_close(*s);
	*s = -1;
}

/* until s can be read (or written), a quarter second at a time so a stop is seen: FALSE after `seconds`, on a
stop, or an error */
static int ftp_wait(int s, int writing, int seconds)
{
	int quarters;
	for (quarters = 0; quarters < seconds * 4 && !ftp.stop; quarters++)
	{
		fd_set set;
		struct timeval timeout;
		int ready;

		FD_ZERO(&set);
		FD_SET(s, &set);
		timeout.tv_sec = 0;
		timeout.tv_usec = 250000;
		ready = lwip_select(s + 1, writing ? NULL : &set, writing ? &set : NULL, NULL, &timeout);
		if (ready < 0)
			return 0;
		if (ready > 0)
			return 1;
	}
	return 0;
}

static int ftp_send_all(int s, const void *data, int size)
{
	const char *p = (const char *)data;
	while (size > 0)
	{
		int sent;

		if (!ftp_wait(s, 1, FTP_DATA_SECONDS))
			return 0;
		sent = lwip_send(s, p, (size_t)size, 0);
		if (sent <= 0)
			return 0;
		p += sent;
		size -= sent;
	}
	return 1;
}

static void ftp_reply(const char *format, const char *argument)
{
	char text[FTP_LINE];
	int length = halo_linux_snprintf(text, sizeof(text) - 2, format, argument ? argument : "");
	if (length < 0 || length > (int)sizeof(text) - 3)
		length = (int)sizeof(text) - 3;
	text[length++] = '\r';
	text[length++] = '\n';
	if (ftp.control >= 0)
		ftp_send_all(ftp.control, text, length);
}

/* a client's path ("x", "../x", "/maps/x") as the folder it names under the game's ("/maps/x"), FALSE when it would
leave the game's folder or is too long */
static int ftp_virtual(const char *argument, char *out)
{
	char work[FTP_PATH * 2];
	char *parts[64];
	int count = 0, k, length;
	char *p;

	if (!argument)
		argument = "";
	if (argument[0] == '/' || argument[0] == '\\')
		length = halo_linux_snprintf(work, sizeof(work), "%s", argument);
	else
		length = halo_linux_snprintf(work, sizeof(work), "%s/%s", ftp.cwd, argument);
	if (length < 0 || length >= (int)sizeof(work))
		return 0;
	for (p = work; *p; p++)
		if (*p == '\\')
			*p = '/';
	p = work;
	while (*p)
	{
		char *start;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		start = p;
		while (*p && *p != '/')
			p++;
		if (*p)
			*p++ = 0;
		if (!strcmp(start, "."))
			continue;
		if (!strcmp(start, ".."))
		{
			if (count)
				count--;
			continue;
		}
		if (strchr(start, ':') || count >= 64)
			return 0;
		parts[count++] = start;
	}
	length = 0;
	out[0] = 0;
	for (k = 0; k < count; k++)
	{
		int add = (int)strlen(parts[k]);
		if (length + 1 + add >= FTP_PATH - 4)
			return 0;
		out[length++] = '/';
		memcpy(out + length, parts[k], (size_t)add);
		length += add;
	}
	if (!length)
		out[length++] = '/';
	out[length] = 0;
	return 1;
}

/* the Xbox's path for a client's ("/maps/x" -> "d:\maps\x") */
static int ftp_real(const char *argument, char *out)
{
	char path[FTP_PATH];
	int k;

	if (!ftp_virtual(argument, path))
		return 0;
	out[0] = 'd';
	out[1] = ':';
	for (k = 0; path[k]; k++)
		out[k + 2] = path[k] == '/' ? '\\' : path[k];
	out[k + 2] = 0;
	if (k == 1)
		out[3] = 0;                    /* ("d:\") */
	return 1;
}

static int ftp_is_folder(const char *real)
{
	DWORD attributes;
	if (real[0] && real[1] == ':' && (!real[2] || (real[2] == '\\' && !real[3])))
		return 1;
	attributes = GetFileAttributesA(real);
	return attributes != NO_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* a change under maps\: the game scans its maps again (on its own thread) */
static void ftp_changed(const char *real)
{
	char lower[16];
	int k;
	for (k = 0; k < 8 && real[k]; k++)
		lower[k] = (char)ftp_lower((unsigned char)real[k]);
	lower[k] = 0;
	if (!strncmp(lower, "d:\\maps", 7))
		oxbox_ftp_maps_changed++;
}

/* ---------- the data connection */

static int ftp_open_passive(int extended)
{
	struct sockaddr_in address;
	socklen_t length = sizeof(address);
	char text[96];
	unsigned long a = ftp.address;
	unsigned short port;

	ftp_close(&ftp.passive);
	ftp_close(&ftp.data);
	ftp.passive = lwip_socket(AF_INET, SOCK_STREAM, 0);
	if (ftp.passive < 0)
		return 0;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = 0;
	address.sin_addr.s_addr = 0;
	if (lwip_bind(ftp.passive, (struct sockaddr *)&address, sizeof(address)) < 0 || lwip_listen(ftp.passive, 1) < 0 ||
		lwip_getsockname(ftp.passive, (struct sockaddr *)&address, &length) < 0)
	{
		ftp_close(&ftp.passive);
		return 0;
	}
	port = ntohs(address.sin_port);
	if (extended)
		halo_linux_snprintf(text, sizeof(text), "229 Entering Extended Passive Mode (|||%u|)", (unsigned)port);
	else
		halo_linux_snprintf(text, sizeof(text), "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)", (unsigned)(a & 255),
			(unsigned)(a >> 8 & 255), (unsigned)(a >> 16 & 255), (unsigned)(a >> 24 & 255), (unsigned)(port >> 8),
			(unsigned)(port & 255));
	ftp_reply("%s", text);
	return 1;
}

/* the client's data connection, once it's connected (after a "150"), FALSE when it didn't come */
static int ftp_accept_data(void)
{
	if (ftp.passive < 0)
		return 0;
	if (!ftp_wait(ftp.passive, 0, FTP_DATA_SECONDS))
	{
		ftp_close(&ftp.passive);
		return 0;
	}
	ftp.data = lwip_accept(ftp.passive, NULL, NULL);
	ftp_close(&ftp.passive);
	return ftp.data >= 0;
}

/* ---------- commands */

static const char *const ftp_months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
	"Nov", "Dec" };

/* a FILETIME's date (100 ns units since 1601), as the proleptic Gregorian calendar has it */
static void ftp_date(const FILETIME_OX *t, unsigned *year, unsigned *month, unsigned *day)
{
	unsigned long long ticks = ((unsigned long long)t->high << 32) | t->low;
	long days = (long)(ticks / 864000000000ull) - 134774;   /* (days since 1970) */
	long era, doe, yoe, doy, mp, y;

	if (days < 0)
		days = 0;
	days += 719468;
	era = days / 146097;
	doe = days - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*day = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
	*month = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
	*year = (unsigned)(y + (*month <= 2));
}

static void ftp_list(const char *argument, int names_only)
{
	char real[FTP_PATH], pattern[FTP_PATH + 4];
	unsigned char find[FIND_SIZE];
	HANDLE handle;
	int length = 0, ok = 1;

	/* (options such as "-la" are left out) */
	while (argument && argument[0] == '-')
	{
		while (*argument && *argument != ' ')
			argument++;
		while (*argument == ' ')
			argument++;
	}
	if (!ftp_real(argument, real) || !ftp_is_folder(real))
	{
		ftp_reply("550 No such folder.", NULL);
		return;
	}
	halo_linux_snprintf(pattern, sizeof(pattern), real[3] ? "%s\\*" : "%s*", real);
	ftp_reply("150 Here comes the folder listing.", NULL);
	if (!ftp_accept_data())
	{
		ftp_reply("425 The data connection didn't open.", NULL);
		return;
	}
	handle = FindFirstFileA(pattern, find);
	if (handle != INVALID_HANDLE_VALUE)
	{
		do
		{
			char const *name = FIND_NAME(find);
			int folder = (FIND_ATTRIBUTES(find) & FILE_ATTRIBUTE_DIRECTORY) != 0;
			struct { unsigned year, month, day; } time;

			if (!strcmp(name, ".") || !strcmp(name, ".."))
				continue;
			ftp_date(FIND_WRITE_TIME(find), &time.year, &time.month, &time.day);
			if (length > FTP_BUFFER - 400)
			{
				if (!ftp_send_all(ftp.data, ftp.buffer, length))
				{
					ok = 0;
					break;
				}
				length = 0;
			}
			if (names_only)
				length += halo_linux_snprintf((char *)ftp.buffer + length, FTP_BUFFER - length, "%s\r\n", name);
			else
				length += halo_linux_snprintf((char *)ftp.buffer + length, FTP_BUFFER - length,
					"%s 1 xbox xbox %10lu %s %2u %04u %s\r\n", folder ? "drwxr-xr-x" : "-rw-r--r--",
					folder ? 0ul : (unsigned long)FIND_SIZE_LOW(find), ftp_months[(time.month - 1) % 12],
					(unsigned)time.day, (unsigned)time.year, name);
		}
		while (FindNextFileA(handle, find));
		CloseHandle(handle);
	}
	if (ok && length)
		ok = ftp_send_all(ftp.data, ftp.buffer, length);
	ftp_close(&ftp.data);
	ftp_reply(ok ? "226 Folder listing sent." : "426 The listing was cut off.", NULL);
}

static void ftp_retrieve(const char *argument)
{
	char real[FTP_PATH];
	HANDLE file;
	DWORD got;
	int ok = 1;

	if (!ftp_real(argument, real) || ftp_is_folder(real) ||
		(file = CreateFileA(real, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)) == INVALID_HANDLE_VALUE)
	{
		ftp_reply("550 Can't open that file.", NULL);
		ftp.rest = 0;
		return;
	}
	if (ftp.rest)
		SetFilePointer(file, (LONG)ftp.rest, NULL, 0);
	ftp.rest = 0;
	ftp_reply("150 Sending the file.", NULL);
	if (!ftp_accept_data())
	{
		CloseHandle(file);
		ftp_reply("425 The data connection didn't open.", NULL);
		return;
	}
	for (;;)
	{
		if (ftp.stop || !ReadFile(file, ftp.buffer, FTP_BUFFER, &got, NULL))
		{
			ok = 0;
			break;
		}
		if (!got)
			break;
		if (!ftp_send_all(ftp.data, ftp.buffer, (int)got))
		{
			ok = 0;
			break;
		}
	}
	CloseHandle(file);
	ftp_close(&ftp.data);
	ftp_reply(ok ? "226 File sent." : "426 The transfer was cut off.", NULL);
}

static void ftp_store(const char *argument, int append)
{
	char real[FTP_PATH], temporary[FTP_PATH + 8];
	HANDLE file;
	DWORD put;
	int ok = 1, got;
	unsigned long total = 0;

	if (!ftp_real(argument, real) || ftp_is_folder(real))
	{
		ftp_reply("553 Can't write there.", NULL);
		return;
	}
	/* (written beside, then put in place: a cut-off upload leaves the old file as it was) */
	halo_linux_snprintf(temporary, sizeof(temporary), "%s.ftp", real);
	if (append || ftp.rest)
	{
		file = CreateFileA(real, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file != INVALID_HANDLE_VALUE)
			SetFilePointer(file, append ? 0 : (LONG)ftp.rest, NULL, append ? 2 : 0);
	}
	else
		file = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		ftp_reply("553 Can't write that file (the game may have it open).", NULL);
		ftp.rest = 0;
		return;
	}
	ftp_reply("150 Ready for the file.", NULL);
	if (!ftp_accept_data())
	{
		CloseHandle(file);
		if (!append && !ftp.rest)
			DeleteFileA(temporary);
		ftp.rest = 0;
		ftp_reply("425 The data connection didn't open.", NULL);
		return;
	}
	for (;;)
	{
		if (!ftp_wait(ftp.data, 0, FTP_DATA_SECONDS))
		{
			ok = 0;
			break;
		}
		got = lwip_recv(ftp.data, ftp.buffer, FTP_BUFFER, 0);
		if (got == 0)
			break;
		if (got < 0 || !WriteFile(file, ftp.buffer, (DWORD)got, &put, NULL) || put != (DWORD)got)
		{
			ok = 0;
			break;
		}
		total += (unsigned long)got;
	}
	FlushFileBuffers(file);
	CloseHandle(file);
	ftp_close(&ftp.data);
	if (!append && !ftp.rest)
	{
		if (ok)
		{
			DeleteFileA(real);
			ok = MoveFileA(temporary, real);
		}
		if (!ok)
			DeleteFileA(temporary);
	}
	ftp.rest = 0;
	if (ok)
	{
		ftp_changed(real);
		platform_log("ftp: wrote %s (%lu bytes)", real, total);
	}
	ftp_reply(ok ? "226 File received." : "451 The upload was cut off: the old file is kept.", NULL);
}

static void ftp_size(const char *argument)
{
	char real[FTP_PATH], text[32];
	HANDLE file;
	DWORD size;

	if (!ftp_real(argument, real) || ftp_is_folder(real) ||
		(file = CreateFileA(real, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)) == INVALID_HANDLE_VALUE)
	{
		ftp_reply("550 No such file.", NULL);
		return;
	}
	size = GetFileSize(file, NULL);
	CloseHandle(file);
	halo_linux_snprintf(text, sizeof(text), "213 %lu", (unsigned long)size);
	ftp_reply("%s", text);
}

/* one command line */
static int ftp_command(char *line)
{
	char *argument = line, path[FTP_PATH], real[FTP_PATH], text[FTP_PATH + 64];

	while (*argument && *argument != ' ')
	{
		*argument = (char)(*argument >= 'a' && *argument <= 'z' ? *argument - 32 : *argument);
		argument++;
	}
	if (*argument)
		*argument++ = 0;
	if (!strcmp(line, "USER"))
	{
		ftp.user_given = 1;
		ftp.logged_in = 0;
		ftp_reply("331 Password, please.", NULL);
		return 1;
	}
	if (!strcmp(line, "PASS"))
	{
		if (ftp.user_given && !strcmp(argument, ftp.password))
		{
			ftp.logged_in = 1;
			ftp_reply("230 Logged in: the game's folder.", NULL);
			platform_log("ftp: a client logged in");
		}
		else
		{
			ftp_reply("530 Wrong password.", NULL);
			platform_log("ftp: a wrong password");
		}
		return 1;
	}
	if (!strcmp(line, "QUIT"))
	{
		ftp_reply("221 Bye.", NULL);
		return 0;
	}
	if (!strcmp(line, "SYST")) { ftp_reply("215 UNIX Type: L8", NULL); return 1; }
	if (!strcmp(line, "FEAT")) { ftp_reply("211-Features:\r\n PASV\r\n EPSV\r\n SIZE\r\n REST STREAM\r\n211 End", NULL); return 1; }
	if (!strcmp(line, "OPTS") || !strcmp(line, "NOOP") || !strcmp(line, "MODE") || !strcmp(line, "STRU") ||
		!strcmp(line, "ALLO"))
	{
		ftp_reply("200 OK.", NULL);
		return 1;
	}
	if (!ftp.logged_in)
	{
		ftp_reply("530 Log in first.", NULL);
		return 1;
	}
	if (!strcmp(line, "PWD") || !strcmp(line, "XPWD"))
	{
		halo_linux_snprintf(text, sizeof(text), "257 \"%s\" is the folder.", ftp.cwd);
		ftp_reply("%s", text);
	}
	else if (!strcmp(line, "CWD") || !strcmp(line, "XCWD") || !strcmp(line, "CDUP") || !strcmp(line, "XCUP"))
	{
		if (ftp_virtual(line[1] == 'D' || line[1] == 'C' && line[2] == 'U' ? ".." : argument, path) &&
			ftp_real(path, real) && ftp_is_folder(real))
		{
			strcpy(ftp.cwd, path);
			ftp_reply("250 OK.", NULL);
		}
		else
			ftp_reply("550 No such folder.", NULL);
	}
	else if (!strcmp(line, "TYPE"))
		ftp_reply("200 Binary it is.", NULL);
	else if (!strcmp(line, "PASV"))
	{
		if (!ftp_open_passive(0))
			ftp_reply("425 Can't open a data port.", NULL);
	}
	else if (!strcmp(line, "EPSV"))
	{
		if (!ftp_open_passive(1))
			ftp_reply("425 Can't open a data port.", NULL);
	}
	else if (!strcmp(line, "PORT") || !strcmp(line, "EPRT"))
		ftp_reply("502 Passive mode only, please.", NULL);
	else if (!strcmp(line, "LIST") || !strcmp(line, "NLST"))
		ftp_list(argument, line[0] == 'N');
	else if (!strcmp(line, "RETR"))
		ftp_retrieve(argument);
	else if (!strcmp(line, "STOR") || !strcmp(line, "APPE"))
		ftp_store(argument, line[0] == 'A');
	else if (!strcmp(line, "REST"))
	{
		unsigned long offset = 0;
		while (*argument >= '0' && *argument <= '9')
			offset = offset * 10 + (unsigned long)(*argument++ - '0');
		ftp.rest = offset;
		ftp_reply("350 Restarting there.", NULL);
	}
	else if (!strcmp(line, "SIZE"))
		ftp_size(argument);
	else if (!strcmp(line, "MDTM"))
		ftp_reply("550 No times here.", NULL);
	else if (!strcmp(line, "DELE"))
	{
		if (ftp_real(argument, real) && !ftp_is_folder(real) && DeleteFileA(real))
		{
			ftp_changed(real);
			platform_log("ftp: deleted %s", real);
			ftp_reply("250 Deleted.", NULL);
		}
		else
			ftp_reply("550 Can't delete that (the game may have it open).", NULL);
	}
	else if (!strcmp(line, "MKD") || !strcmp(line, "XMKD"))
	{
		if (ftp_real(argument, real) && CreateDirectoryA(real, NULL))
		{
			ftp_changed(real);
			ftp_reply("257 Folder made.", NULL);
		}
		else
			ftp_reply("550 Can't make that folder.", NULL);
	}
	else if (!strcmp(line, "RMD") || !strcmp(line, "XRMD"))
	{
		if (ftp_real(argument, real) && real[3] && RemoveDirectoryA(real))
		{
			ftp_changed(real);
			ftp_reply("250 Folder removed.", NULL);
		}
		else
			ftp_reply("550 Can't remove that folder (it must be empty).", NULL);
	}
	else if (!strcmp(line, "RNFR"))
	{
		if (ftp_real(argument, real) && GetFileAttributesA(real) != NO_ATTRIBUTES)
		{
			strcpy(ftp.rename_from, real);
			ftp_reply("350 And its new name?", NULL);
		}
		else
			ftp_reply("550 No such file.", NULL);
	}
	else if (!strcmp(line, "RNTO"))
	{
		if (ftp.rename_from[0] && ftp_real(argument, real) && MoveFileA(ftp.rename_from, real))
		{
			ftp_changed(ftp.rename_from);
			ftp_changed(real);
			ftp_reply("250 Renamed.", NULL);
		}
		else
			ftp_reply("550 Can't rename that.", NULL);
		ftp.rename_from[0] = 0;
	}
	else if (!strcmp(line, "ABOR"))
	{
		ftp_close(&ftp.data);
		ftp_close(&ftp.passive);
		ftp_reply("226 Nothing to stop.", NULL);
	}
	else
		ftp_reply("502 Not here.", NULL);
	return 1;
}

/* ---------- the thread */

static void ftp_session(void)
{
	unsigned long idle = 0;

	ftp.logged_in = ftp.user_given = 0;
	strcpy(ftp.cwd, "/");
	ftp.rename_from[0] = 0;
	ftp.rest = 0;
	ftp.line_length = 0;
	ftp_reply("220 Halo 1 Unlimited's FTP (the game's folder; in the menus only).", NULL);
	while (!ftp.stop)
	{
		fd_set read_set;
		struct timeval timeout;
		int got, k;
		char incoming[256];

		FD_ZERO(&read_set);
		FD_SET(ftp.control, &read_set);
		timeout.tv_sec = 0;
		timeout.tv_usec = 250000;
		got = lwip_select(ftp.control + 1, &read_set, NULL, NULL, &timeout);
		if (got < 0)
			break;
		if (got == 0)
		{
			if (++idle > FTP_IDLE_SECONDS * 4)
				break;
			continue;
		}
		idle = 0;
		got = lwip_recv(ftp.control, incoming, sizeof(incoming), 0);
		if (got <= 0)
			break;
		for (k = 0; k < got; k++)
		{
			char c = incoming[k];
			if (c == '\r')
				continue;
			if (c == '\n')
			{
				ftp.line[ftp.line_length] = 0;
				ftp.line_length = 0;
				if (ftp.line[0] && !ftp_command(ftp.line))
					return;
				continue;
			}
			if (ftp.line_length < FTP_LINE - 1)
				ftp.line[ftp.line_length++] = c;
		}
	}
}

static DWORD NTAPI ftp_thread(PVOID parameter)
{
	(void)parameter;
	while (!ftp.stop)
	{
		fd_set read_set;
		struct timeval timeout;

		FD_ZERO(&read_set);
		FD_SET(ftp.listener, &read_set);
		timeout.tv_sec = 0;
		timeout.tv_usec = 250000;
		if (lwip_select(ftp.listener + 1, &read_set, NULL, NULL, &timeout) <= 0)
			continue;
		ftp.control = lwip_accept(ftp.listener, NULL, NULL);
		if (ftp.control < 0)
			continue;
		ftp_session();
		ftp_close(&ftp.data);
		ftp_close(&ftp.passive);
		ftp_close(&ftp.control);
	}
	ftp_close(&ftp.listener);
	ftp.running = 0;
	return 0;
}

/* ---------- public code (oxbox_sdk.c) */

/* the server up, on port `port` (FALSE when the network isn't up yet: tried again later) */
int oxbox_ftp_start(unsigned short port, char const *password)
{
	struct sockaddr_in address;
	int dhcp, one = 1;

	if (ftp.running)
		return 1;
	if (ftp.thread)                      /* (an earlier stop that waited out: its thread has ended since) */
	{
		CloseHandle(ftp.thread);
		ftp.thread = NULL;
		if (ftp.buffer)
			LocalFree(ftp.buffer);
		ftp.buffer = NULL;
	}
	if (!oxbox_lwip_address(&ftp.address, &dhcp))
		return 0;
	ftp.control = ftp.passive = ftp.data = -1;
	ftp.listener = lwip_socket(AF_INET, SOCK_STREAM, 0);
	if (ftp.listener < 0)
		return 0;
	lwip_setsockopt(ftp.listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	if (lwip_bind(ftp.listener, (struct sockaddr *)&address, sizeof(address)) < 0 || lwip_listen(ftp.listener, 1) < 0)
	{
		platform_log("ftp: port %u is taken", (unsigned)port);
		ftp_close(&ftp.listener);
		return 0;
	}
	ftp.buffer = (unsigned char *)LocalAlloc(0, FTP_BUFFER);
	if (!ftp.buffer)
	{
		ftp_close(&ftp.listener);
		return 0;
	}
	ftp.port = port;
	strncpy(ftp.password, password, sizeof(ftp.password) - 1);
	ftp.password[sizeof(ftp.password) - 1] = 0;
	ftp.stop = 0;
	ftp.running = 1;
	ftp.thread = CreateThread(NULL, FTP_STACK, (PVOID)ftp_thread, NULL, 0, NULL);
	if (!ftp.thread)
	{
		ftp.running = 0;
		ftp_close(&ftp.listener);
		LocalFree(ftp.buffer);
		ftp.buffer = NULL;
		return 0;
	}
	halo_linux_snprintf(oxbox_ftp_status, 64, "FTP: %u.%u.%u.%u port %u", (unsigned)(ftp.address & 255),
		(unsigned)(ftp.address >> 8 & 255), (unsigned)(ftp.address >> 16 & 255), (unsigned)(ftp.address >> 24 & 255),
		(unsigned)port);
	platform_log("ftp: on at %s (the game's folder, in the menus)", oxbox_ftp_status + 5);
	return 1;
}

/* the server down: its connections closed, its thread ended, its memory freed */
void oxbox_ftp_stop(void)
{
	unsigned long waited;

	if (!ftp.running && !ftp.thread)
		return;
	ftp.stop = 1;
	for (waited = 0; ftp.running && waited < 5000; waited += 10)
		Sleep(10);
	if (ftp.running)
	{
		platform_log("ftp: the server's thread hasn't stopped yet (a file operation): it will on its own");
		return;                          /* (its memory kept until it has: oxbox_ftp_start frees nothing it uses) */
	}
	if (ftp.thread)
		CloseHandle(ftp.thread);
	ftp.thread = NULL;
	if (ftp.buffer)
		LocalFree(ftp.buffer);
	ftp.buffer = NULL;
	oxbox_ftp_status[0] = 0;
	platform_log("ftp: off (a map is loading: the menus only)");
}
