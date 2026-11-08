/* stockmaps_host.h: what stockmaps.c uses of the Xbox's Windows API, on a PC (stockmaps_test.c) */
#ifndef OXBOX_STOCKMAPS_HOST_H
#define OXBOX_STOCKMAPS_HOST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef FILE *HANDLE;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef struct { unsigned long long QuadPart; } ULARGE_INTEGER;
typedef struct { int unused; } IO_STATUS_BLOCK;
#define INVALID_HANDLE_VALUE ((HANDLE)NULL)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define GENERIC_READ 1
#define GENERIC_WRITE 2
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 1
#define CREATE_ALWAYS 2
#define FILE_ATTRIBUTE_NORMAL 0
#define FILE_BEGIN 0
#define NT_SUCCESS(status) ((status) == 0)

/* (a backslash path as the PC's; "X:\" isn't used: the test's folder is a plain relative or absolute path) */
static inline void host_path(char const *from, char *to)
{
	snprintf(to, 512, "%s", from);
	for (; *to; to++)
		if (*to == '\\')
			*to = '/';
}

static inline uint32_t GetFileAttributesA(char const *path)
{
	char p[512];
	struct stat s;
	host_path(path, p);
	return stat(p, &s) ? INVALID_FILE_ATTRIBUTES : 0;
}

static inline HANDLE CreateFileA(char const *path, int access, int share, void *security, int disposition, int flags,
	void *other)
{
	char p[512];
	(void)share, (void)security, (void)flags, (void)other;
	host_path(path, p);
	if (!(access & GENERIC_WRITE))
		return fopen(p, "rb");
	/* (read and write: an existing file as it is, else a new one) */
	return fopen(p, disposition == OPEN_EXISTING ? "r+b" : access & GENERIC_READ ? "w+b" : "wb");
}

static inline int ReadFile(HANDLE file, void *to, DWORD size, DWORD *got, void *overlapped)
{
	(void)overlapped;
	*got = (DWORD)fread(to, 1, size, file);
	return !ferror(file);
}

static inline int WriteFile(HANDLE file, void const *from, DWORD size, DWORD *put, void *overlapped)
{
	(void)overlapped;
	*put = (DWORD)fwrite(from, 1, size, file);
	return *put == size;
}

static inline DWORD SetFilePointer(HANDLE file, LONG to, void *high, int how)
{
	(void)high, (void)how;
	return fseek(file, (long)(uint32_t)to, SEEK_SET) ? 0xFFFFFFFFu : (DWORD)to;
}

static inline DWORD GetFileSize(HANDLE file, void *high)
{
	long at = ftell(file), size;
	(void)high;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, at, SEEK_SET);
	return (DWORD)size;
}

static inline int CloseHandle(HANDLE file)
{
	return !fclose(file);
}

static inline int NtFlushBuffersFile(HANDLE file, IO_STATUS_BLOCK *status)
{
	(void)status;
	return fflush(file);
}

/* (STOCKMAPS_TEST_CUT=N: the power cut before the Nth rename or delete; the count is said at the end) */
static int host_steps;
static inline void host_steps_say(void)
{
	if (getenv("STOCKMAPS_TEST_COUNT"))
		fprintf(stderr, "steps: %d\n", host_steps);
}
static inline void host_step(void)
{
	char const *cut = getenv("STOCKMAPS_TEST_CUT");
	if (!host_steps)
		atexit(host_steps_say);
	if (++host_steps && cut && host_steps == atoi(cut))
		_exit(99);
}

static inline int DeleteFileA(char const *path)
{
	char p[512];
	host_step();
	host_path(path, p);
	return !remove(p);
}

static inline int MoveFileA(char const *from, char const *to)
{
	char a[512], b[512];
	struct stat s;
	host_step();
	host_path(from, a);
	host_path(to, b);
	/* (as the Xbox's: it doesn't replace what's there) */
	if (!stat(b, &s))
		return 0;
	return !rename(a, b);
}

/* (STOCKMAPS_TEST_FREE_MB: how much the test's drive has free) */
static inline int GetDiskFreeSpaceExA(char const *root, ULARGE_INTEGER *available, ULARGE_INTEGER *total,
	ULARGE_INTEGER *free_total)
{
	char const *mb = getenv("STOCKMAPS_TEST_FREE_MB");
	(void)root, (void)total, (void)free_total;
	available->QuadPart = (mb ? strtoull(mb, NULL, 10) : 1024 * 1024) * 1024 * 1024;
	return 1;
}

#endif
