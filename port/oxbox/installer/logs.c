/* logs.c: "Send logs" (logs.h; docs/log_uploads.md): the logs gathered, each file's last part when it's long, packed
(zlib) into one bundle with a code the player can quote, and POSTed to the log uploads' Worker.

The bundle (OXLG, version 1, little-endian): a 0x40-byte header ("OXLG", 1, the code's 8 characters, the source (2: the
installer), the game's build, the installer's version, the file count, the bundle's size, the CRC32 of all after the
header), then each file: its name (64 bytes, "H1U/perf/sweep_p4.csv"), its whole size, where the part kept starts, how
much is kept, how much that packs to, then the packed bytes. A settings file's password lines are blanked. */
#include "logs.h"
#include "mapstream.h"
#include "sha256.h"
#include "text.h"

#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <zlib.h>

#include <stdlib.h>
#include <string.h>

#define BUNDLE_HEADER 0x40u
#define ENTRY 80u
#define MOST_BUNDLE (6u * 1024u * 1024u)
#define MOST_KEPT (3u * 1024u * 1024u)   /* (a file's last part, at most) */
#define MOST_FILES 48
#define MOST_PERF_FILES 16      /* (the sweeps' files) */
#define MOST_PERF_LOGS 8        /* (the newest of the rest: perf_log = 1's, one a map, and a profile build's
                                   profile.bin; installer 75: 8, as 4 left out a10's after three games) */

static char const alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

struct pick
{
	char name[64];
	char path[260];
	unsigned long most;
};

static struct pick picks[MOST_FILES];
static int pick_count;

static void pick(char const *name, char const *path, unsigned long most)
{
	struct pick *p;
	if (pick_count >= MOST_FILES)
		return;
	p = &picks[pick_count++];
	text_print(p->name, sizeof(p->name), "%s", name);
	text_print(p->path, sizeof(p->path), "%s", path);
	p->most = most < MOST_KEPT ? most : MOST_KEPT;
}

static void pick_in(char const *folder, char const *file, unsigned long most)
{
	char name[64], path[260];
	text_print(name, sizeof(name), "H1U/%s", file);
	text_print(path, sizeof(path), "%s\\%s", folder, file);
	pick(name, path, most);
}

static voidpf logs_alloc(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return calloc(items, size);
}

static void logs_free(voidpf opaque, voidpf address)
{
	(void)opaque;
	free(address);
}

/* a settings file's "password" lines: their values blanked (the same length) */
static void blank_passwords(unsigned char *p, unsigned long size)
{
	unsigned long i = 0;
	while (i < size)
	{
		unsigned long end = i, k;
		int password = 0;
		while (end < size && p[end] != '\n')
			end++;
		for (k = i; k + 8 <= end; k++)
			if (!_strnicmp((char const *)p + k, "password", 8))
				password = 1;
		if (password)
		{
			for (k = i; k < end && p[k] != '='; k++)
				;
			for (k++; k < end && p[k] != '\r'; k++)
				p[k] = '*';
		}
		i = end + 1;
	}
}

/* one file's last part (most bytes) into the bundle at *size: 1 when it's in, 0 when it isn't there or there's no
room for it */
static int add(struct pick const *p, unsigned char *bundle, unsigned long *size, unsigned char *scratch)
{
	HANDLE file = CreateFileA(p->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	unsigned long whole, kept, from, n;
	unsigned char *entry = bundle + *size;
	DWORD got = 0;
	z_stream z;
	int ok;
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	whole = GetFileSize(file, NULL);
	kept = whole > p->most ? p->most : whole;
	from = whole - kept;
	ok = whole != INVALID_FILE_SIZE && SetFilePointer(file, (LONG)from, NULL, FILE_BEGIN) == from &&
		(!kept || ReadFile(file, scratch, kept, &got, NULL));
	CloseHandle(file);
	if (!ok || MOST_BUNDLE - *size < ENTRY + 64)
		return 0;
	kept = got;
	n = (unsigned long)strlen(p->name);
	if (n > 4 && !_stricmp(p->name + n - 4, ".ini"))
		blank_passwords(scratch, kept);
	memset(entry, 0, ENTRY);
	memcpy(entry, p->name, n < 63 ? n : 63);
	put32(entry + 64, whole);
	put32(entry + 68, from);
	put32(entry + 72, kept);
	memset(&z, 0, sizeof(z));
	z.zalloc = logs_alloc;
	z.zfree = logs_free;
	if (deflateInit(&z, 6) != Z_OK)
		return 0;
	z.next_in = scratch;
	z.avail_in = kept;
	z.next_out = entry + ENTRY;
	z.avail_out = MOST_BUNDLE - *size - ENTRY;
	ok = deflate(&z, Z_FINISH) == Z_STREAM_END;
	n = (unsigned long)z.total_out;
	deflateEnd(&z);
	if (!ok)
		return 0;
	put32(entry + 76, n);
	*size += ENTRY + n;
	return 1;
}

int logs_send(char const *url, char const *install, char const *own_log, unsigned long build, unsigned long installer_version,
	char *code, int *files, unsigned long *kilobytes, struct download_report const *report, char *error)
{
	static char const *const top[] = {"oxbox_log.txt", "oxbox_log_previous.txt", "opence.ini", "opence_release.txt",
		"opence_showcase.txt", "stabbed.txt"};
	unsigned char *bundle = malloc(MOST_BUNDLE), *scratch = malloc(MOST_KEPT), digest[32];
	char address[64], answer[128], want[16];
	unsigned long size = BUNDLE_HEADER;
	uint64_t stamp[2];
	struct sha256 s;
	int i, result = 1;

	*files = 0;
	*kilobytes = 0;
	code[0] = 0;
	if (!bundle || !scratch)
	{
		text_print(error, 256, "not enough memory");
		goto done;
	}
	/* what's sent: the installer's log, the game's logs and settings, its perf files (most important first) */
	pick_count = 0;
	if (own_log)
		pick("installer/installer_log.txt", own_log, 2u * 1024u * 1024u);
	if (install)
	{
		char pattern[260];
		WIN32_FIND_DATAA found;
		HANDLE find;
		int perf = 0;
		for (i = 0; i < (int)(sizeof(top) / sizeof(top[0])); i++)
			pick_in(install, top[i], i < 2 ? MOST_KEPT : 256u * 1024u);
		pick_in(install, "perf\\sweep_hangs.txt", 512u * 1024u);
		pick_in(install, "debug.txt", 1024u * 1024u);
		/* perf's files: the sweeps' (sweep_*), and the newest MOST_PERF_LOGS of the rest (installer 74: perf_log = 1
		writes one a map played, which piled up) */
		text_print(pattern, sizeof(pattern), "%s\\perf\\*", install);
		find = FindFirstFileA(pattern, &found);
		if (find != INVALID_HANDLE_VALUE)
		{
			static struct { char name[64]; FILETIME time; } logs[MOST_PERF_LOGS];
			int count = 0, k;
			do
			{
				char name[64];
				if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !_stricmp(found.cFileName, "sweep_hangs.txt"))
					continue;
				text_print(name, sizeof(name), "perf\\%s", found.cFileName);
				if (!_strnicmp(found.cFileName, "sweep", 5))
				{
					if (perf++ < MOST_PERF_FILES)
						pick_in(install, name, 1024u * 1024u);
					continue;
				}
				/* (kept newest first: the oldest kept gives way) */
				for (k = count < MOST_PERF_LOGS ? count++ : MOST_PERF_LOGS;
					k > 0 && CompareFileTime(&logs[k - 1].time, &found.ftLastWriteTime) < 0; k--)
				{
					if (k < MOST_PERF_LOGS)
						logs[k] = logs[k - 1];
				}
				if (k < MOST_PERF_LOGS)
				{
					text_print(logs[k].name, sizeof(logs[k].name), "%s", name);
					logs[k].time = found.ftLastWriteTime;
				}
			} while (FindNextFileA(find, &found));
			FindClose(find);
			for (k = 0; k < count; k++)
				pick_in(install, logs[k].name, 1024u * 1024u);
		}
	}
	for (i = 0; i < pick_count; i++)
	{
		char *slash;
		while ((slash = strchr(picks[i].name, '\\')) != NULL)
			*slash = '/';
		*files += add(&picks[i], bundle, &size, scratch);
	}
	if (!*files)
	{
		text_print(error, 256, "there are no logs to send");
		goto done;
	}
	/* its code: from the clock's fine count and the bundle */
	stamp[0] = __builtin_ia32_rdtsc();
	stamp[1] = KeQueryPerformanceCounter();
	sha256_start(&s);
	sha256_add(&s, stamp, sizeof(stamp));
	sha256_add(&s, bundle + BUNDLE_HEADER, size - BUNDLE_HEADER);
	sha256_end(&s, digest);
	for (i = 0; i < 8; i++)
		code[i] = alphabet[digest[i] & 31];
	code[8] = 0;
	memset(bundle, 0, BUNDLE_HEADER);
	memcpy(bundle, "OXLG", 4);
	put32(bundle + 0x04, 1);
	memcpy(bundle + 0x08, code, 8);
	put32(bundle + 0x10, LOGS_SOURCE_INSTALLER);
	put32(bundle + 0x14, build);
	put32(bundle + 0x18, installer_version);
	put32(bundle + 0x1C, (uint32_t)*files);
	put32(bundle + 0x20, size);
	put32(bundle + 0x24, crc_add(crc_add(0, NULL, 0), bundle + BUNDLE_HEADER, size - BUNDLE_HEADER));
	*kilobytes = (size + 1023) / 1024;
	if (download_network(report, address, error) ||
		download_post(url, bundle, size, answer, sizeof(answer), report, error))
		goto done;
	text_print(want, sizeof(want), "ok %s", code);
	if (strcmp(answer, want))
	{
		text_print(error, 256, "the server's answer was \"%.60s\"", answer);
		goto done;
	}
	result = 0;
done:
	free(bundle);
	free(scratch);
	if (result)
		code[0] = 0;
	return result;
}
