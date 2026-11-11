/* mapstream.c: a cache file read and written a piece at a time, and a map's blocks found (mapstream.h) */
#include "mapstream.h"

#include "ngz.h"
#include "text.h"

#include <stdlib.h>
#include <string.h>
#ifdef STOCKMAPS_HOST
#include <time.h>
#endif

#define MB (1024u * 1024u)

static unsigned char const zeros[1];
/* (1 MB at a time each way: the swap reads a retail map and writes its small map in turns, and on the Xbox's disk 64 KB
reads and deflate's ~24 KB writes ran at 20 and 5 MB/s, against 29 and 28 for 1 MB ones. One writer at a time: its output
waits in io_out until that's full or it finishes. A reader reads into io_in and io_in2 in turns: one's read ahead while
the other's unpacked) */
static unsigned char io_in[MB], io_in2[MB], io_out[MB];

/* (zlib built without its own allocator on the Xbox: nxdk's Z_SOLO) */
static voidpf z_alloc(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return calloc(items, size);
}

static void z_free(voidpf opaque, voidpf address)
{
	(void)opaque;
	free(address);
}

struct stream_times stream_times;

uint64_t stream_now(void)
{
#ifdef STOCKMAPS_HOST
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#else
	static uint64_t frequency;
	uint64_t now = KeQueryPerformanceCounter();
	if (!frequency)
		frequency = KeQueryPerformanceFrequency();
	return now / frequency * 1000000u + now % frequency * 1000000u / frequency;
#endif
}

/* ---- the disk thread: while it's on (stockmaps.c's swap and restore), writes are queued and made behind, and a reader's
next 1 MB is read ahead, while zlib works on what's here (on the Xbox's one CPU, the disk works on its own while it waits).
Everything goes in the order it was queued, so each file's writes, seeks and reads are as they were made; anything else
that touches a file (a read not queued, a flush, a close, a rename) first waits for the queue to empty ---- */

#define JOBS 512
#define RING (4u * MB)                  /* (a power of 2: the counts below go round 2^32 evenly) */
#define MOST_FAILED 8

enum { JOB_WRITE, JOB_SEEK, JOB_READ };
struct job
{
	int kind;
	HANDLE file;
	unsigned char *data;
	uint32_t size, at, ring_end;
	struct ahead_read *result;      /* (a read's) */
};
static struct job jobs[JOBS];
static unsigned char ring[RING];        /* what's waiting to be written */
static volatile uint32_t queued, done, ring_freed;
static uint32_t ring_used;              /* (counts: of jobs, and of the ring's bytes, all the way since it began) */
static HANDLE volatile failed[MOST_FAILED];  /* files a queued write or seek failed for (until closed) */
static volatile int failed_all;         /* (more than that: all of them) */
static int disk_running, disk_started;

#define BARRIER() __asm__ __volatile__("" ::: "memory")

#ifdef STOCKMAPS_HOST
#include <pthread.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int work_count, progress_set;
static void work_post(void)
{
	pthread_mutex_lock(&lock);
	work_count++;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
}
static void work_wait(void)
{
	pthread_mutex_lock(&lock);
	while (!work_count)
		pthread_cond_wait(&changed, &lock);
	work_count--;
	pthread_mutex_unlock(&lock);
}
static void progress_post(void)
{
	pthread_mutex_lock(&lock);
	progress_set = 1;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
}
static void progress_wait(void)
{
	pthread_mutex_lock(&lock);
	while (!progress_set)
		pthread_cond_wait(&changed, &lock);
	progress_set = 0;
	pthread_mutex_unlock(&lock);
}
#else
static HANDLE work, progress;           /* (a semaphore: jobs queued; an event, auto-reset: a job's done) */
static void work_post(void)
{
	ReleaseSemaphore(work, 1, NULL);
}
static void work_wait(void)
{
	WaitForSingleObject(work, INFINITE);
}
static void progress_post(void)
{
	SetEvent(progress);
}
static void progress_wait(void)
{
	WaitForSingleObject(progress, INFINITE);
}
#endif

static int file_failed(HANDLE file)
{
	int i;
	if (failed_all)
		return 1;
	for (i = 0; i < MOST_FAILED; i++)
		if (failed[i] == file)
			return 1;
	return 0;
}

static void fail(HANDLE file)
{
	int i;
	if (file_failed(file))
		return;
	for (i = 0; i < MOST_FAILED; i++)
		if (!failed[i])
		{
			failed[i] = file;
			return;
		}
	failed_all = 1;
}

#ifdef STOCKMAPS_HOST
static void *disk_thread(void *unused)
#else
static DWORD WINAPI disk_thread(LPVOID unused)
#endif
{
	(void)unused;
	for (;;)
	{
		struct job *j;
		uint64_t start;
		DWORD n = 0;
		work_wait();
		BARRIER();
		j = &jobs[done % JOBS];
		start = stream_now();
		if (j->kind == JOB_READ)
		{
			int ok = ReadFile(j->file, j->data, j->size, &n, NULL);
			stream_times.read += stream_now() - start;
			stream_times.read_bytes += n;
			j->result->ok = ok;
			j->result->got = n;
		}
		else if (file_failed(j->file))
			; /* (what comes after a failed write isn't written) */
		else if (j->kind == JOB_SEEK)
		{
			if (SetFilePointer(j->file, (LONG)j->at, NULL, FILE_BEGIN) != j->at)
				fail(j->file);
		}
		else
		{
			if (!WriteFile(j->file, j->data, j->size, &n, NULL) || n != j->size)
				fail(j->file);
			stream_times.write += stream_now() - start;
			stream_times.write_bytes += n;
		}
		if (j->kind == JOB_WRITE)
			ring_freed = j->ring_end;
		BARRIER();
		done++;
		progress_post();
	}
#ifdef STOCKMAPS_HOST
	return NULL;
#else
	return 0;
#endif
}

/* (the main thread waiting for the disk: the time goes in stream_times.wait) */
static void wait_until_done(uint32_t count)
{
	uint64_t start = stream_now();
	while ((int32_t)(done - count) < 0)
		progress_wait();
	BARRIER();
	stream_times.wait += stream_now() - start;
}

static struct job *job_new(void)
{
	if (queued - done >= JOBS)
		wait_until_done(queued - JOBS + 1);
	return &jobs[queued % JOBS];
}

static uint32_t job_queue(void)
{
	BARRIER();
	queued++;
	work_post();
	return queued;
}

void disk_on(void)
{
	if (!disk_started)
	{
#ifdef STOCKMAPS_HOST
		pthread_t thread;
		if (pthread_create(&thread, NULL, disk_thread, NULL))
			return;
		pthread_detach(thread);
#else
		HANDLE thread;
		work = CreateSemaphore(NULL, 0, JOBS + 1, NULL);
		progress = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (!work || !progress)
			return;
		thread = CreateThread(NULL, 64 * 1024, disk_thread, NULL, 0, NULL);
		if (!thread)
			return;
		/* (above the main thread: it gives the disk its next job as soon as one's done) */
		SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
		CloseHandle(thread);
#endif
		disk_started = 1;
	}
	disk_running = 1;
}

void disk_sync(void)
{
	if (disk_running)
		wait_until_done(queued);
}

void disk_off(void)
{
	int i;
	disk_sync();
	disk_running = 0;
	for (i = 0; i < MOST_FAILED; i++)
		failed[i] = 0;
	failed_all = 0;
}

int disk_seek(HANDLE file, uint32_t at)
{
	struct job *j;
	if (!disk_running)
		return SetFilePointer(file, (LONG)at, NULL, FILE_BEGIN) != at;
	j = job_new();
	j->kind = JOB_SEEK;
	j->file = file;
	j->at = at;
	job_queue();
	return file_failed(file);
}

int file_close(HANDLE file)
{
	int ok, i;
	disk_sync();
	ok = !file_failed(file);
	for (i = 0; i < MOST_FAILED; i++)
		if (failed[i] == file)
			failed[i] = 0;
	return CloseHandle(file) && ok;
}

int timed_read(HANDLE file, void *to, DWORD size, DWORD *got)
{
	uint64_t start, took;
	int ok;
	disk_sync();
	start = stream_now();
	ok = ReadFile(file, to, size, got, NULL);
	took = stream_now() - start;
	stream_times.read += took;
	stream_times.wait += took;
	stream_times.read_bytes += *got;
	return ok;
}

int timed_write(HANDLE file, void const *from, DWORD size, DWORD *put)
{
	uint64_t start, took;
	int ok;
	if (disk_running && size <= MB)
	{
		struct job *j;
		uint32_t at = ring_used % RING;
		if (at + size > RING)
			ring_used += RING - at; /* (each write's bytes in one piece) */
		while (ring_used + size - ring_freed > RING)
		{
			uint64_t began = stream_now();
			progress_wait();
			stream_times.wait += stream_now() - began;
		}
		BARRIER();
		j = job_new();
		j->kind = JOB_WRITE;
		j->file = file;
		j->data = ring + ring_used % RING;
		j->size = (uint32_t)size;
		memcpy(j->data, from, size);
		ring_used += size;
		j->ring_end = ring_used;
		job_queue();
		*put = size;
		return !file_failed(file);
	}
	disk_sync();
	start = stream_now();
	ok = WriteFile(file, from, size, put, NULL);
	took = stream_now() - start;
	stream_times.write += took;
	stream_times.wait += took;
	stream_times.write_bytes += *put;
	return ok && !file_failed(file);
}

int timed_flush(HANDLE file)
{
	IO_STATUS_BLOCK status;
	uint64_t start, took;
	int ok;
	disk_sync();
	start = stream_now();
	ok = NT_SUCCESS(NtFlushBuffersFile(file, &status)) && !file_failed(file);
	took = stream_now() - start;
	stream_times.write += took;
	stream_times.wait += took;
	return !ok;
}

void stream_times_get(struct stream_times *t)
{
	/* (the disk thread adds to it as it goes: a copy made twice the same isn't half one and half the other) */
	struct stream_times again;
	do
	{
		*t = stream_times;
		again = stream_times;
	} while (memcmp(t, &again, sizeof(again)));
}

/* ---- a file read 1 MB at a time, the next read ahead while the disk thread is on ---- */

static void ahead_queue(struct ahead *a, int i)
{
	struct job *j = job_new();
	j->kind = JOB_READ;
	j->file = a->file;
	j->data = a->buffer[i];
	j->size = MB;
	j->result = &a->read[i];
	a->read[i].job = job_queue();
}

void ahead_open(struct ahead *a, HANDLE file, unsigned char *one, unsigned char *two)
{
	memset(a, 0, sizeof(*a));
	a->file = file;
	a->buffer[0] = one;
	a->buffer[1] = two;
	a->handed = -1;
	a->queued = disk_running;
	if (a->queued)
	{
		ahead_queue(a, 0);
		ahead_queue(a, 1);
	}
}

int ahead_next(struct ahead *a, unsigned char **data, DWORD *got)
{
	int i = a->next;
	*data = a->buffer[i];
	*got = 0;
	if (!a->queued)
	{
		if (a->ended)
			return 0;
		if (!timed_read(a->file, a->buffer[0], MB, got))
			return 1;
		*data = a->buffer[0];
		a->ended = *got < MB;
		return 0;
	}
	if (a->handed >= 0 && !a->ended)
		ahead_queue(a, a->handed);
	a->handed = -1;
	if (!a->read[i].job)
		return 0; /* (the end: nothing more was asked for) */
	wait_until_done(a->read[i].job);
	a->read[i].job = 0;
	if (!a->read[i].ok)
	{
		a->ended = 1;
		return 1;
	}
	*got = a->read[i].got;
	if (*got < MB)
		a->ended = 1;
	a->handed = i;
	a->next = i ^ 1;
	return 0;
}

static void tenths(char *to, int size, uint64_t us)
{
	text_print(to, (size_t)size, "%lu.%lu s", (unsigned long)(us / 1000000u), (unsigned long)(us / 100000u % 10u));
}

void stream_times_text(char *text, int size, uint64_t since, struct stream_times const *then)
{
	struct stream_times now;
	uint64_t all = stream_now() - since, read, write, unzip, zip, wait, busy;
	char t[6][24];
	stream_times_get(&now);
	read = now.read - then->read, write = now.write - then->write, unzip = now.unzip - then->unzip,
		zip = now.zip - then->zip, wait = now.wait - then->wait, busy = unzip + zip + wait;
	tenths(t[0], 24, all);
	tenths(t[1], 24, read);
	tenths(t[2], 24, write);
	tenths(t[3], 24, unzip);
	tenths(t[4], 24, zip);
	tenths(t[5], 24, wait);
	text_print(text, (size_t)size, "%s: read %s (%lu MB), write %s (%lu MB), unzip %s (%lu MB), zip %s (%lu MB), "
		"waited for the disk %s, other %lu.%lu s", t[0], t[1], (unsigned long)((now.read_bytes - then->read_bytes) >> 20), t[2],
		(unsigned long)((now.write_bytes - then->write_bytes) >> 20), t[3], (unsigned long)((now.unzipped - then->unzipped) >> 20),
		t[4], (unsigned long)((now.zipped - then->zipped) >> 20), t[5], (unsigned long)((all > busy ? all - busy : 0) / 1000000u),
		(unsigned long)((all > busy ? all - busy : 0) / 100000u % 10u));
}

/* ---- CRC-32 (zlib's, the same numbers): 8 bytes a step. nxdk's zlib is built Z_SOLO, which leaves out its own fast CRC,
so its crc32() goes a byte a step: about 65 MB/s on the console, ~100 s of the swap's CPU time ---- */

static uint32_t crc_table[8][256];

static void crc_tables(void)
{
	uint32_t n, k;
	for (n = 0; n < 256; n++)
	{
		uint32_t c = n;
		for (k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[0][n] = c;
	}
	for (n = 0; n < 256; n++)
		for (k = 1; k < 8; k++)
			crc_table[k][n] = (crc_table[k - 1][n] >> 8) ^ crc_table[0][crc_table[k - 1][n] & 0xFF];
}

uint32_t crc_add(uint32_t crc, unsigned char const *p, uint32_t size)
{
	if (!crc_table[0][1])
		crc_tables();
	if (!p)
		return 0;
	crc = ~crc;
	while (size && ((uintptr_t)p & 3))
		crc = (crc >> 8) ^ crc_table[0][(crc ^ *p++) & 0xFF], size--;
	while (size >= 8)
	{
		/* (p is on a 4-byte boundary here; little-endian, the Xbox's and the PC's. Not memcpy: nxdk's compiler makes each
		one a call) */
		uint32_t a = ((uint32_t const *)(void const *)p)[0] ^ crc, b = ((uint32_t const *)(void const *)p)[1];
		crc = crc_table[7][a & 0xFF] ^ crc_table[6][(a >> 8) & 0xFF] ^ crc_table[5][(a >> 16) & 0xFF] ^ crc_table[4][a >> 24] ^
			crc_table[3][b & 0xFF] ^ crc_table[2][(b >> 8) & 0xFF] ^ crc_table[1][(b >> 16) & 0xFF] ^ crc_table[0][b >> 24];
		p += 8;
		size -= 8;
	}
	while (size--)
		crc = (crc >> 8) ^ crc_table[0][(crc ^ *p++) & 0xFF];
	return ~crc;
}

uint32_t u32(unsigned char const *p)
{
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void put32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

HANDLE open_read(char const *path)
{
	return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

int write_all(HANDLE file, void const *from, uint32_t size)
{
	unsigned char const *at = from;
	while (size)
	{
		DWORD put = 0, part = size > MB ? MB : size;
		if (!timed_write(file, at, part, &put) || put != part)
			return 1;
		at += part;
		size -= part;
	}
	return 0;
}

int read_all(HANDLE file, void *to, uint32_t size)
{
	unsigned char *at = to;
	while (size)
	{
		DWORD got = 0, part = size > MB ? MB : size;
		if (!timed_read(file, at, part, &got) || got != part)
			return 1;
		at += part;
		size -= part;
	}
	return 0;
}


/* (zlib-ng's inflate: what comes out is the same as zlib's, faster) */
static int timed_inflate(struct reader *r, unsigned char **out, uint32_t *room)
{
	uint64_t start = stream_now();
	uint32_t before = *room;
	int status = ngz_run(r->z, Z_NO_FLUSH, &r->in, &r->in_size, out, room);
	stream_times.unzip += stream_now() - start;
	stream_times.unzipped += before - *room;
	return status;
}

void reader_close(struct reader *r)
{
	ngz_end(r->z);
	r->z = NULL;
	if (r->file != INVALID_HANDLE_VALUE)
		file_close(r->file);
	r->file = INVALID_HANDLE_VALUE;
}

/* a cache file opened, its header read */
int reader_open(struct reader *r, char const *path, unsigned char *header)
{
	memset(r, 0, sizeof(*r));
	r->file = open_read(path);
	if (r->file == INVALID_HANDLE_VALUE)
		return 1;
	if (read_all(r->file, header, HEADER) || memcmp(header, "daeh", 4) || u32(header + 4) != 5)
	{
		reader_close(r);
		return 1;
	}
	r->raw_crc = crc_add(crc_add(0, NULL, 0), header, HEADER);
	r->raw_size = HEADER;
	r->z = ngz_inflate();
	if (!r->z)
	{
		reader_close(r);
		return 1;
	}
	ahead_open(&r->ahead, r->file, io_in, io_in2);
	return 0;
}

/* (the next part of the file in, when what came is used up: 0 when there is one) */
static int reader_fill(struct reader *r)
{
	unsigned char *data;
	DWORD got = 0;
	if (ahead_next(&r->ahead, &data, &got) || !got)
		return 1;
	r->raw_crc = crc_add(r->raw_crc, data, got);
	r->raw_size += got;
	r->in = data;
	r->in_size = got;
	return 0;
}

/* exactly size bytes of what's unpacked; with size 0, 0 only if the stream ends here */
int reader_pull(struct reader *r, unsigned char *to, uint32_t size)
{
	unsigned char spare, *out;
	uint32_t room;
	int checking = !size;

	out = checking ? &spare : to;
	room = checking ? 1 : size;
	while (room)
	{
		int status;
		if (r->ended)
			return !checking;
		if (!r->in_size && reader_fill(r))
			return 1;
		status = timed_inflate(r, &out, &room);
		if (status == Z_STREAM_END)
			r->ended = 1;
		else if (status != Z_OK && !(status == Z_BUF_ERROR && !r->in_size))
			return 1;
	}
	return checking; /* (a byte more came: it doesn't end here) */
}


int reader_read(struct reader *r, unsigned char *to, uint32_t size, uint32_t *got)
{
	unsigned char *out = to;
	uint32_t room = size;
	while (room && !r->ended)
	{
		int status;
		if (!r->in_size && reader_fill(r))
			return 1;
		status = timed_inflate(r, &out, &room);
		if (status == Z_STREAM_END)
			r->ended = 1;
		else if (status != Z_OK && !(status == Z_BUF_ERROR && !r->in_size))
			return 1;
	}
	*got = size - room;
	return 0;
}

int reader_raw(struct reader *r, uint32_t *size, uint32_t *crc)
{
	unsigned char *data;
	DWORD got;
	while (!ahead_next(&r->ahead, &data, &got) && got)
	{
		r->raw_crc = crc_add(r->raw_crc, data, got);
		r->raw_size += got;
	}
	*size = r->raw_size;
	*crc = r->raw_crc;
	return 0;
}

/* one zlib stream (memLevel 8) to a file, or only measured; the CRC goes on from crc */
int writer_open(struct writer *w, int level, HANDLE to, uint32_t crc)
{
	memset(w, 0, sizeof(*w));
	w->z.zalloc = z_alloc;
	w->z.zfree = z_free;
	w->to = to;
	w->crc = crc;
	return deflateInit2(&w->z, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK;
}

int writer_open_fast(struct writer *w, HANDLE to, uint32_t crc)
{
	memset(w, 0, sizeof(*w));
	w->to = to;
	w->crc = crc;
	w->fast = ngz_deflate(WRITER_FAST_LEVEL);
	return !w->fast;
}

/* (one deflate call on whichever stream it is: out and room in, what came out and what's left to go in back) */
static int writer_deflate(struct writer *w, int flush, unsigned char *out, uint32_t room, uint32_t *made, uint32_t *left_in)
{
	uint64_t start = stream_now();
	uint32_t before;
	int status;
	if (w->fast)
	{
		uint32_t out_room = room;
		before = w->in_size;
		status = ngz_run(w->fast, flush, &w->in, &w->in_size, &out, &out_room);
		*made = room - out_room;
		*left_in = w->in_size;
	}
	else
	{
		w->z.next_out = out;
		w->z.avail_out = (uInt)room;
		before = w->z.avail_in;
		status = deflate(&w->z, flush);
		*made = room - w->z.avail_out;
		*left_in = w->z.avail_in;
	}
	stream_times.zip += stream_now() - start;
	stream_times.zipped += before - *left_in;
	return status;
}

static int writer_run(struct writer *w, int flush)
{
	int status;
	uint32_t room, n, left_in;
	do
	{
		room = (uint32_t)sizeof(io_out) - w->held;
		status = writer_deflate(w, flush, io_out + w->held, room, &n, &left_in);
		if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END)
			return 1;
		w->crc = crc_add(w->crc, io_out + w->held, n);
		w->written += n;
		w->held += n;
		if (w->held == sizeof(io_out) || (flush == Z_FINISH && status == Z_STREAM_END))
		{
			if (w->to != INVALID_HANDLE_VALUE && write_all(w->to, io_out, w->held))
				return 1;
			w->held = 0;
		}
	} while (flush == Z_FINISH ? status != Z_STREAM_END : n == room);
	(void)left_in;
	return 0;
}

int writer_put(struct writer *w, unsigned char const *from, uint32_t size)
{
	if (w->fast)
	{
		w->in = from;
		w->in_size = size;
	}
	else
	{
		w->z.next_in = (Bytef *)from;
		w->z.avail_in = size;
	}
	return writer_run(w, Z_NO_FLUSH);
}

void writer_abandon(struct writer *w)
{
	if (w->fast)
		ngz_end(w->fast);
	else
		deflateEnd(&w->z);
	w->fast = NULL;
}

int writer_finish(struct writer *w)
{
	int failed;
	if (w->fast)
	{
		w->in = zeros;
		w->in_size = 0;
	}
	else
	{
		w->z.next_in = (Bytef *)zeros;
		w->z.avail_in = 0;
	}
	failed = writer_run(w, Z_FINISH);
	writer_abandon(w);
	return failed;
}

/* ---- a map's blocks: a bitmap's pixels, a sound permutation's samples (as tools/shared_pass.py's Map) ---- */

/* a place in the map (in the tag data) */
static uint32_t tag32(struct map_view const *m, uint32_t offset)
{
	if (offset < m->tag_offset || offset - m->tag_offset > m->tag_size - 4)
		return 0;
	return u32(m->tags + (offset - m->tag_offset));
}

/* an address in the tag data, as an offset in the map, or 0 */
static uint32_t tag_at(struct map_view const *m, uint32_t address)
{
	uint32_t o = address - TAG_BASE;
	if (address < TAG_BASE || o >= m->tag_size)
		return 0;
	return m->tag_offset + o;
}

int find_blocks(struct map_view const *m, struct block *blocks, int most, int *count, struct map_bsp *bsps, uint32_t *bsp_count)
{
	uint32_t instances, scenario, tags, k, table, s;
	int n = 0;

	instances = tag32(m, m->tag_offset);
	scenario = tag32(m, m->tag_offset + 4);
	tags = tag32(m, m->tag_offset + 12);
	table = tag_at(m, instances);
	if (!table)
		return 1;
	for (k = 0; k < tags; k++)
	{
		uint32_t instance = table + k * 0x20, group = tag32(m, instance), base = tag32(m, instance + 0x14), b, i;
		if (!base || (group != BITMAP && group != SOUND))
			continue;
		if (!(b = tag_at(m, base)))
			return 1;
		if (group == BITMAP)
		{
			/* (each bitmap's own pixels: its pixels_offset is the file's) */
			uint32_t bitmaps = tag_at(m, tag32(m, b + 0x64)), bitmap_count = tag32(m, b + 0x60);
			for (i = 0; i < bitmap_count; i++)
			{
				uint32_t e = bitmaps + i * 0x30;
				if (!bitmaps || n == most)
					return 1;
				blocks[n].field = e + 0x18;
				blocks[n].offset = tag32(m, e + 0x18);
				blocks[n].size = tag32(m, e + 0x1C);
				n++;
			}
		}
		else
		{
			uint32_t ranges = tag_at(m, tag32(m, b + 0x9C)), range_count = tag32(m, b + 0x98);
			for (i = 0; i < range_count; i++)
			{
				uint32_t range = ranges + i * 0x48, q, permutation_count, permutations;
				if (!ranges)
					return 1;
				permutation_count = tag32(m, range + 0x3C);
				permutations = tag_at(m, tag32(m, range + 0x40));
				for (q = 0; q < permutation_count; q++)
				{
					uint32_t p = permutations + q * 0x7C;
					if (!permutations || n == most)
						return 1;
					blocks[n].field = p + 0x48;
					blocks[n].offset = tag32(m, p + 0x48);
					blocks[n].size = tag32(m, p + 0x40);
					n++;
				}
			}
		}
	}
	/* the ones in the map's own data, outside the tag data */
	for (k = 0, *count = 0; k < (uint32_t)n; k++)
	{
		struct block const *e = &blocks[k];
		if (e->size && e->offset >= HEADER && e->size <= m->length && e->offset <= m->length - e->size &&
			!(e->offset & 0x80000000u) &&
			!(m->tag_offset < e->offset + e->size && e->offset < m->tag_offset + m->tag_size))
			blocks[(*count)++] = *e;
	}
	/* the BSPs' offset fields */
	if (!(s = tag_at(m, tag32(m, table + (scenario & 0xFFFF) * 0x20 + 0x14))))
		return 1;
	*bsp_count = tag32(m, s + SCENARIO_BSPS);
	if (*bsp_count > MOST_BSPS)
		return 1;
	if (*bsp_count)
	{
		uint32_t references = tag_at(m, tag32(m, s + SCENARIO_BSPS + 4));
		if (!references)
			return 1;
		for (k = 0; k < *bsp_count; k++)
		{
			bsps[k].field = references + k * 0x20;
			bsps[k].offset = tag32(m, bsps[k].field);
		}
	}
	return 0;
}

