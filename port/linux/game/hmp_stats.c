/*
HMP_STATS.C

Halo Universal's play stats, favorites, hidden maps and the remembered sort (hmp.h; Halo Universal's stats.c). Each
console keeps T:\hmp_stats.txt (E:\TDATA\4d530004\hmp_stats.txt), one line per map:

	CEX\Abyss  plays=14  last=2026-10-12  first=2026-10-02  fav=0  hide=0  order=37

- the name is the map's folder ("Precipice", "CEX\Abyss" for a pack's map) or a stock map's ("bloodgulch");
- plays: matches started on the map on this console (counted once a match has run a second);
- last / first: the day of the last play, the day the map first showed up in the list ("-" while the clock isn't set);
- fav, hide: from the map list (Y, Black held) and the postgame (Y);
- order: when it was last played, as a count (the Recently played sort needs no clock).
A line "*settings  sort=genres" remembers the list's sort. Lines for maps not in the list now are kept; lines
starting with # are notes. Read once (the first scan); written after a scan finds new maps and when a match starts,
by the background thread (hmp_share.c), as hmp_stats.tmp and then the swap.
*/

#ifdef HALO_XBOX_TARGET

/* ---------- headers */

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "hmp.h"

#include <string.h>
#include <xtl.h>

/* ---------- constants */

#define STATS_PATH "T:\\hmp_stats.txt"
#define STATS_TEMPORARY_PATH "T:\\hmp_stats.tmp"
#define MAXIMUM_STATS 4096
#define LINE_FIELDS 104
#define INVALID_FILE ((HANDLE)-1)

/* ---------- structures */

struct stat_line
{
	char key[32];
	unsigned long plays, last, first, order;
	unsigned char favorite, hidden, pad[2];
};

/* ---------- prototypes */

void *__stdcall LocalAlloc(unsigned int flags, unsigned int bytes);
void *__stdcall LocalFree(void *memory);
int __stdcall FlushFileBuffers(HANDLE file);
void *oxbox_kernel_export(unsigned long ordinal);
int __stdcall FileTimeToLocalFileTime(FILETIME const *file_time, FILETIME *local_file_time);
char const *cache_files_loaded_map_name(void);

/* ---------- globals */

static struct
{
	struct stat_line *lines;
	long count, capacity;
	boolean loaded, dirty, counted;
	short frozen;                       /* the file is there but none of it could be read: left alone */
	char *buffer;                       /* the file as it will be written (Halo's thread fills it) */
	unsigned long buffer_capacity;
	volatile unsigned long buffer_length;
	volatile boolean buffer_ready;      /* the background thread owns the buffer until it has written it */
	volatile short write_error;
	boolean write_error_told;
	unsigned long last_ticks;
	unsigned long order_maximum;
	short sort;
	unsigned short *hash;               /* key -> line + 1 (open addressing) */
	long hash_size;
} hmp_stats_globals = { NULL, 0, 0, FALSE, FALSE, FALSE, 0, NULL, 0, 0, FALSE, 0, FALSE, 0, 0, NONE };

static char const *const sort_names[NUMBER_OF_HMP_SORTS] =
{
	"packs", "favorites", "played", "recent", "players", "genres", "gametypes", "name"
};

/* ---------- private code */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ieq(char const *a, char const *b)
{
	while (*a && lower(*a) == lower(*b)) { a++; b++; }
	return lower(*a) - lower(*b);
}
static char *scpy(char *d, char const *s) { while ((*d = *s) != 0) { d++; s++; } return d; }
static char *put_number(char *o, unsigned long v)
{
	char d[11];
	long k = 0;
	do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (k) *o++ = d[--k];
	return o;
}
static char *put_date(char *o, unsigned long v)
{
	if (!v) { *o++ = '-'; return o; }
	o = put_number(o, v / 10000);
	*o++ = '-';
	*o++ = (char)('0' + v / 1000 % 10); *o++ = (char)('0' + v / 100 % 10);
	*o++ = '-';
	*o++ = (char)('0' + v / 10 % 10); *o++ = (char)('0' + v % 10);
	return o;
}
static unsigned long get_date(char const *s)
{
	unsigned long y = 0, m = 0, d = 0;
	long k;
	for (k = 0; k < 4; k++) { if (s[k] < '0' || s[k] > '9') return 0; y = y * 10 + (unsigned long)(s[k] - '0'); }
	if (s[4] != '-' || s[7] != '-') return 0;
	for (k = 5; k < 7; k++) { if (s[k] < '0' || s[k] > '9') return 0; m = m * 10 + (unsigned long)(s[k] - '0'); }
	for (k = 8; k < 10; k++) { if (s[k] < '0' || s[k] > '9') return 0; d = d * 10 + (unsigned long)(s[k] - '0'); }
	return m >= 1 && m <= 12 && d >= 1 && d <= 31 ? y * 10000 + m * 100 + d : 0;
}

/* a map's name in the file: a folder map's key without the '+' and with '\' ("CEX\Abyss"), or a stock map's name */
static void normalize(char *out, char const *name)
{
	char const *s = name, *q;
	long k = 0;
	if (*s == '+') s++;
	else for (q = s; *q; q++) if (*q == '\\') s = q + 1;
	for (; *s && k < 31; s++) out[k++] = *s == '/' ? '\\' : *s;
	out[k] = 0;
}

static unsigned long hash_key(char const *k)
{
	unsigned long h = 2166136261u;
	while (*k) h = (h ^ (unsigned long)lower(*k++)) * 16777619u;
	return h;
}
static void hash_put(long i)
{
	unsigned long h = hash_key(hmp_stats_globals.lines[i].key) & (hmp_stats_globals.hash_size - 1);
	while (hmp_stats_globals.hash[h]) h = (h + 1) & (hmp_stats_globals.hash_size - 1);
	hmp_stats_globals.hash[h] = (unsigned short)(i + 1);
}
/* the hash table sized for the lines (at least twice as many slots), rebuilt when it fills */
static void hash_rebuild(void)
{
	long size = 256, i;
	while (size < hmp_stats_globals.capacity * 2) size *= 2;
	if (hmp_stats_globals.hash && hmp_stats_globals.hash_size == size) return;
	if (hmp_stats_globals.hash) LocalFree(hmp_stats_globals.hash);
	hmp_stats_globals.hash = (unsigned short *)LocalAlloc(LMEM_ZEROINIT, (unsigned int)size * 2);
	hmp_stats_globals.hash_size = hmp_stats_globals.hash ? size : 0;
	if (hmp_stats_globals.hash) for (i = 0; i < hmp_stats_globals.count; i++) hash_put(i);
}
static struct stat_line *find(char const *key)
{
	long i;
	if (hmp_stats_globals.hash)
	{
		unsigned long h = hash_key(key) & (hmp_stats_globals.hash_size - 1);
		for (; hmp_stats_globals.hash[h]; h = (h + 1) & (hmp_stats_globals.hash_size - 1))
		{
			if (!ieq(hmp_stats_globals.lines[hmp_stats_globals.hash[h] - 1].key, key)) return &hmp_stats_globals.lines[hmp_stats_globals.hash[h] - 1];
		}
		return NULL;
	}
	for (i = 0; i < hmp_stats_globals.count; i++) if (!ieq(hmp_stats_globals.lines[i].key, key)) return &hmp_stats_globals.lines[i];
	return NULL;
}
static struct stat_line *add(char const *key)
{
	struct stat_line *line;
	if (hmp_stats_globals.count == hmp_stats_globals.capacity)
	{
		long capacity = hmp_stats_globals.capacity ? hmp_stats_globals.capacity * 2 : 64;
		struct stat_line *lines;
		if (capacity > MAXIMUM_STATS) capacity = MAXIMUM_STATS;
		if (hmp_stats_globals.count >= capacity) return NULL;
		lines = (struct stat_line *)LocalAlloc(LMEM_ZEROINIT, (unsigned int)capacity * sizeof(struct stat_line));
		if (!lines) return NULL;
		if (hmp_stats_globals.lines)
		{
			memcpy(lines, hmp_stats_globals.lines, hmp_stats_globals.count * sizeof(struct stat_line));
			LocalFree(hmp_stats_globals.lines);
		}
		hmp_stats_globals.lines = lines;
		hmp_stats_globals.capacity = capacity;
		hash_rebuild();
	}
	line = &hmp_stats_globals.lines[hmp_stats_globals.count++];
	memset(line, 0, sizeof(*line));
	scpy(line->key, key);
	if (hmp_stats_globals.hash) hash_put(hmp_stats_globals.count - 1);
	return line;
}

static boolean is_field(char const *s)
{
	long n = 0;
	while (*s == ' ' || *s == '\t') s++;
	while (*s >= 'a' && *s <= 'z') { s++; n++; }
	return n && *s == '=';
}
static unsigned long number(char const *s)
{
	unsigned long v = 0;
	while (*s >= '0' && *s <= '9' && v < 100000000) v = v * 10 + (unsigned long)(*s++ - '0');
	return v;
}
static void parse_line(char *s)
{
	char *e, key[32];
	struct stat_line *line;
	long k;
	while (*s == ' ' || *s == '\t') s++;
	if (!*s || *s == '#' || *s == ';') return;
	for (e = s; *e && !((*e == ' ' || *e == '\t') && is_field(e)); e++) ;
	if (!*e) return;                                 /* (no "name=value": not a map's line) */
	while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
	if (e == s || e - s > 31) return;
	for (k = 0; s + k < e; k++) key[k] = s[k] == '/' ? '\\' : s[k];
	key[k] = 0;
	if (key[0] == '*')                               /* "*settings  sort=genres" */
	{
		char *v = e;
		while (*v)
		{
			while (*v == ' ' || *v == '\t') v++;
			if (!strncmp(v, "sort=", 5))
			{
				char name[16];
				long j = 0;
				for (v += 5; *v && *v != ' ' && *v != '\t' && j < 15; v++) name[j++] = *v;
				name[j] = 0;
				for (j = 0; j < NUMBER_OF_HMP_SORTS; j++) if (!ieq(name, sort_names[j])) hmp_stats_globals.sort = (short)j;
			}
			while (*v && *v != ' ' && *v != '\t') v++;
		}
		return;
	}
	if (find(key) || !(line = add(key))) return;     /* (a name listed twice: the first line counts) */
	while (*e)
	{
		char *name, *value;
		while (*e == ' ' || *e == '\t') e++;
		name = e;
		while (*e && *e != '=' && *e != ' ' && *e != '\t') e++;
		if (*e != '=') { while (*e && *e != ' ' && *e != '\t') e++; continue; }
		*e++ = 0;
		value = e;
		while (*e && *e != ' ' && *e != '\t') e++;
		if (*e) *e++ = 0;
		if (!ieq(name, "plays")) line->plays = number(value);
		else if (!ieq(name, "last")) line->last = get_date(value);
		else if (!ieq(name, "first")) line->first = get_date(value);
		else if (!ieq(name, "fav")) line->favorite = number(value) != 0;
		else if (!ieq(name, "hide")) line->hidden = number(value) != 0;
		else if (!ieq(name, "order"))
		{
			line->order = number(value);
			if (line->order > hmp_stats_globals.order_maximum) hmp_stats_globals.order_maximum = line->order;
		}
	}
}

static boolean load_file(char const *path)
{
	unsigned char raw[512];
	char line[160];
	long n = 0, k, before = hmp_stats_globals.count;
	boolean first = TRUE, utf16 = FALSE, half = FALSE;
	unsigned long got, total = 0;
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_FILE) return FALSE;
	for (;;)
	{
		got = 0;
		if (!ReadFile(file, raw, sizeof(raw), &got, NULL) || !got) break;
		k = 0;
		total += got;
		if (first)
		{
			first = FALSE;
			if (got >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) k = 3;
			else if (got >= 2 && raw[0] == 0xff && raw[1] == 0xfe) { k = 2; utf16 = TRUE; }
		}
		for (; k < (long)got; k++)
		{
			char c = (char)raw[k];
			if (utf16 && (half = !half) == FALSE) continue;
			if (c == '\n' || c == '\r') { line[n] = 0; parse_line(line); n = 0; }
			else if (n < (long)sizeof(line) - 1) line[n++] = c;
		}
	}
	line[n] = 0;
	parse_line(line);
	CloseHandle(file);
	if (total > 16 && hmp_stats_globals.count == before) hmp_stats_globals.frozen = 1;   /* (not ours, or damaged) */
	return TRUE;
}

static void load(void)
{
	hmp_stats_globals.loaded = TRUE;
	if (!load_file(STATS_PATH) && load_file(STATS_TEMPORARY_PATH)) hmp_stats_globals.dirty = TRUE;   /* (off mid-swap) */
}

/* the file's text, for the background thread to write */
static void format(void)
{
	static char const head[] = "# Halo Universal play stats (one line per map): plays = matches started on this Xbox, last = last "
		"played, first = first seen, fav = favorite (Y), hide = hidden (hold Black), order = when last played (higher = more "
		"recent)\r\n";
	unsigned long need = sizeof(head) + 40;
	char *o;
	long i;
	if (hmp_stats_globals.frozen) { hmp_stats_globals.dirty = FALSE; return; }
	for (i = 0; i < hmp_stats_globals.count; i++) need += (unsigned long)strlen(hmp_stats_globals.lines[i].key) + LINE_FIELDS;
	if (need > hmp_stats_globals.buffer_capacity)
	{
		char *buffer = (char *)LocalAlloc(0, need + 1024);
		if (!buffer) return;
		if (hmp_stats_globals.buffer) LocalFree(hmp_stats_globals.buffer);
		hmp_stats_globals.buffer = buffer;
		hmp_stats_globals.buffer_capacity = need + 1024;
	}
	o = scpy(hmp_stats_globals.buffer, head);
	if (hmp_stats_globals.sort >= 0 && hmp_stats_globals.sort < NUMBER_OF_HMP_SORTS)
	{
		o = scpy(o, "*settings  sort=");
		o = scpy(o, sort_names[hmp_stats_globals.sort]);
		o = scpy(o, "\r\n");
	}
	for (i = 0; i < hmp_stats_globals.count; i++)
	{
		struct stat_line *line = &hmp_stats_globals.lines[i];
		o = scpy(o, line->key);
		o = scpy(o, "  plays="); o = put_number(o, line->plays);
		o = scpy(o, "  last="); o = put_date(o, line->last);
		o = scpy(o, "  first="); o = put_date(o, line->first);
		o = scpy(o, "  fav="); *o++ = (char)('0' + line->favorite);
		o = scpy(o, "  hide="); *o++ = (char)('0' + line->hidden);
		if (line->order) { o = scpy(o, "  order="); o = put_number(o, line->order); }
		o = scpy(o, "\r\n");
	}
	hmp_stats_globals.buffer_length = (unsigned long)(o - hmp_stats_globals.buffer);
	hmp_stats_globals.dirty = FALSE;
	__asm__ __volatile__("" ::: "memory");
	hmp_stats_globals.buffer_ready = TRUE;
}

/* a match started on the loaded map: one more play */
static void played(void)
{
	char key[32];
	char const *map = cache_files_loaded_map_name();
	struct stat_line *line;
	unsigned long today = hmp_today();
#if defined(OXBOX_SHOWCASE)
	/* (the showcase screensaver's games aren't plays: showcase.c) */
	{
		extern boolean showcase_active(void);

		if (showcase_active()) return;
	}
#endif
	if (!hmp_stats_globals.loaded) load();
	normalize(key, map ? map : "");
	if (!key[0] || !ieq(key, "ui")) return;
	if (!(line = find(key)) && !(line = add(key))) return;
	if (!line->first) line->first = today;
	line->plays++;
	line->order = ++hmp_stats_globals.order_maximum;
	if (today) line->last = today;
	hmp_stats_globals.dirty = TRUE;
	hmp_log("stats: a match started on %s (%d %s)%s", key, line->plays, line->plays == 1 ? "play" : "plays",
		today ? "" : "; the clock isn't set");
}

/* ---------- public code */

/* today on the console's clock as yyyymmdd (0: the clock isn't set) */
unsigned long hmp_today(void)
{
	LARGE_INTEGER now;
	FILETIME utc, local;
	unsigned __int64 days;
	unsigned long z, era, doe, yoe, y, doy, mp, d, m;
	((void (__stdcall *)(LARGE_INTEGER *))oxbox_kernel_export(128))(&now);   /* KeQuerySystemTime */
	utc.dwLowDateTime = now.LowPart;
	utc.dwHighDateTime = (unsigned long)now.HighPart;
	if (!FileTimeToLocalFileTime(&utc, &local)) local = utc;
	days = (((unsigned __int64)local.dwHighDateTime << 32) | local.dwLowDateTime) / 10000000 / 86400;
	if (days < 134774 || days > 0xffffffu) return 0;
	z = (unsigned long)days - 134774 + 719468;       /* days since 0000-03-01 (from 1970-01-01) */
	era = z / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	y += m <= 2;
	if (y < 2024 || y > 2099) return 0;              /* (never set: an Xbox without its clock says 2001 or so) */
	return y * 10000 + m * 100 + d;
}

void hmp_stats_seen(char const *name)
{
	char key[32];
	struct stat_line *line;
	if (!hmp_stats_globals.loaded) load();
	normalize(key, name);
	if (!key[0] || find(key) || !(line = add(key))) return;
	line->first = hmp_today();
	hmp_stats_globals.dirty = TRUE;
}

boolean hmp_stats_flush(void)
{
	HANDLE file;
	unsigned long put = 0;
	if (!hmp_stats_globals.buffer_ready) return FALSE;
	__asm__ __volatile__("" ::: "memory");
	file = CreateFileA(STATS_TEMPORARY_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_FILE) hmp_stats_globals.write_error = 1;
	else
	{
		if (!WriteFile(file, hmp_stats_globals.buffer, hmp_stats_globals.buffer_length, &put, NULL) ||
			put != hmp_stats_globals.buffer_length)
		{
			hmp_stats_globals.write_error = 2;
		}
		FlushFileBuffers(file);
		CloseHandle(file);
		if (put == hmp_stats_globals.buffer_length)
		{
			DeleteFileA(STATS_PATH);
			if (!MoveFileA(STATS_TEMPORARY_PATH, STATS_PATH)) hmp_stats_globals.write_error = 3;
		}
	}
	__asm__ __volatile__("" ::: "memory");
	hmp_stats_globals.buffer_ready = FALSE;
	return TRUE;
}

void hmp_stats_tick(void)
{
	if (!game_engine || !game_in_progress()) { hmp_stats_globals.counted = FALSE; hmp_stats_globals.last_ticks = 0; }
	else
	{
		unsigned long ticks = (unsigned long)game_time_get();
		if (ticks < hmp_stats_globals.last_ticks) hmp_stats_globals.counted = FALSE;   /* (time started over: a new match) */
		hmp_stats_globals.last_ticks = ticks;
		if (!hmp_stats_globals.counted && ticks >= 30) { hmp_stats_globals.counted = TRUE; played(); }
	}
	if (hmp_stats_globals.frozen == 1)
	{
		hmp_stats_globals.frozen = 2;
		hmp_log("stats: couldn't read T:\\hmp_stats.txt; leaving it as it is (no stats this time)");
	}
	if (hmp_stats_globals.write_error && !hmp_stats_globals.write_error_told)
	{
		hmp_stats_globals.write_error_told = TRUE;
		hmp_log("stats: couldn't %s", hmp_stats_globals.write_error == 1 ? "create T:\\hmp_stats.tmp" :
			hmp_stats_globals.write_error == 2 ? "write T:\\hmp_stats.tmp (disk full?)" : "rename T:\\hmp_stats.tmp to hmp_stats.txt");
	}
	if (hmp_stats_globals.dirty && !hmp_stats_globals.buffer_ready && hmp_worker_running()) format();
}

void hmp_stats_get(char const *name, struct hmp_rating *rating)
{
	char key[32];
	struct stat_line *line;
	if (!hmp_stats_globals.loaded) load();
	memset(rating, 0, sizeof(*rating));
	normalize(key, name ? name : "");
	if (!key[0] || !(line = find(key))) return;
	rating->plays = line->plays;
	rating->order = line->order;
	rating->favorite = line->favorite;
	rating->hidden = line->hidden;
	rating->known = 1;
}

/* what: 0 favorite, 2 hidden */
void hmp_stats_set(char const *name, short what, long value)
{
	char key[32];
	struct stat_line *line;
	if (!hmp_stats_globals.loaded) load();
	normalize(key, name ? name : "");
	if (!key[0] || (!(line = find(key)) && !(line = add(key)))) return;
	if (what == 0) line->favorite = value != 0;
	else line->hidden = value != 0;
	hmp_stats_globals.dirty = TRUE;
	hmp_log("stats: %s: %s", key, what == 0 ? (line->favorite ? "favorite" : "not a favorite") :
		(line->hidden ? "hidden" : "shown again"));
}

short hmp_stats_sort(void)
{
	if (!hmp_stats_globals.loaded) load();
	return hmp_stats_globals.sort;
}

void hmp_stats_set_sort(short sort)
{
	if (!hmp_stats_globals.loaded) load();
	if (sort != hmp_stats_globals.sort) { hmp_stats_globals.sort = sort; hmp_stats_globals.dirty = TRUE; }
}

#endif /* HALO_XBOX_TARGET */
