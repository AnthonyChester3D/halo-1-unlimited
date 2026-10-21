/*
OXBOX_LIBC.C

The C library functions the game calls, for the Xbox: strings, character classes, conversions, memory (the
XAPI heap, as the retail game's own C runtime uses), sorting, MSVC's rand, errno and time. The game was compiled
against glibc's headers (as the Linux port is), so these keep glibc's ABI where it shows (the ctype tables, the
ISO C99 scanf names, struct tm), with MSVC's behaviour where the game relies on it (rand's 15 bits).
*/

#define __NO_CTYPE 1
#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- memory: the XAPI heap (LocalAlloc and friends in the retail XBE) */

void oxbox_log_allocation_failure(unsigned long size);

/* (memory measurement: live heap bytes, its peak, and every allocation of 64 KB or more with its caller) */
void platform_log(const char *format, ...);
unsigned long oxbox_heap_live, oxbox_heap_peak, oxbox_heap_blocks;
static void heap_note(void *p, void *caller)
{
	unsigned long s;

	if (!p)
		return;
	s = LocalSize(p);
	oxbox_heap_live += s;
	oxbox_heap_blocks++;
	if (oxbox_heap_live > oxbox_heap_peak)
		oxbox_heap_peak = oxbox_heap_live;
	if (s >= 65536)
		platform_log("alloc %lu KB by %p (heap %lu KB)", s / 1024, caller, oxbox_heap_live / 1024);
}
static void heap_forget(void *p)
{
	if (p)
	{
		oxbox_heap_live -= LocalSize(p);
		oxbox_heap_blocks--;
	}
}

void *malloc(size_t size)
{
	void *p = LocalAlloc(LMEM_FIXED, size ? size : 1);

	if (!p)
		oxbox_log_allocation_failure(size);
	heap_note(p, __builtin_return_address(0));
	return p;
}

void *calloc(size_t count, size_t size)
{
	size_t total = count * size;
	void *p;

	if (size && total / size != count)
		return NULL;
	p = LocalAlloc(LMEM_ZEROINIT, total ? total : 1);
	heap_note(p, __builtin_return_address(0));
	return p;
}

void free(void *pointer)
{
	if (pointer)
	{
		heap_forget(pointer);
		LocalFree(pointer);
	}
}

void *realloc(void *pointer, size_t size)
{
	void *p;

	if (!pointer)
	{
		p = LocalAlloc(LMEM_FIXED, size ? size : 1);
		heap_note(p, __builtin_return_address(0));
		return p;
	}
	if (!size)
	{
		free(pointer);
		return NULL;
	}
	heap_forget(pointer);
	p = LocalReAlloc(pointer, size, LMEM_MOVEABLE);
	heap_note(p ? p : pointer, __builtin_return_address(0));
	return p;
}

size_t _msize(void *pointer)
{
	return LocalSize(pointer);
}

/* ---------- memory and strings */

void *memcpy(void *destination, const void *source, size_t count)
{
	void *d = destination;
	const void *s = source;
	size_t dwords = count >> 2, bytes = count & 3;

	__asm__ volatile ("rep movsl" : "+D"(d), "+S"(s), "+c"(dwords) : : "memory");
	__asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(bytes) : : "memory");
	return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
	unsigned char *d = destination;
	const unsigned char *s = source;

	if (d == s || !count)
		return destination;
	if (d < s || d >= s + count)
		return memcpy(destination, source, count);
	d += count;
	s += count;
	while (count--)
		*--d = *--s;
	return destination;
}

void *memset(void *destination, int value, size_t count)
{
	void *d = destination;
	size_t c = count;

	__asm__ volatile ("rep stosb" : "+D"(d), "+c"(c) : "a"(value) : "memory");
	return destination;
}

int memcmp(const void *a, const void *b, size_t count)
{
	const unsigned char *x = a, *y = b;

	for (; count; count--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

void *memchr(const void *memory, int value, size_t count)
{
	const unsigned char *p = memory;

	for (; count; count--, p++)
		if (*p == (unsigned char)value)
			return (void *)p;
	return NULL;
}

size_t strlen(const char *string)
{
	const char *p = string;

	while (*p)
		p++;
	return (size_t)(p - string);
}

/* (strnlen is the game's own: cseries.c) */

char *strcpy(char *destination, const char *source)
{
	char *d = destination;

	while ((*d++ = *source++) != 0)
		;
	return destination;
}

char *strncpy(char *destination, const char *source, size_t count)
{
	size_t i = 0;

	for (; i < count && source[i]; i++)
		destination[i] = source[i];
	for (; i < count; i++)
		destination[i] = 0;
	return destination;
}

char *strcat(char *destination, const char *source)
{
	strcpy(destination + strlen(destination), source);
	return destination;
}

char *strncat(char *destination, const char *source, size_t count)
{
	char *d = destination + strlen(destination);

	while (count-- && *source)
		*d++ = *source++;
	*d = 0;
	return destination;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t count)
{
	for (; count; count--, a++, b++)
	{
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (!*a)
			break;
	}
	return 0;
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int _stricmp(const char *a, const char *b)
{
	int x, y;

	do
	{
		x = lower((unsigned char)*a++);
		y = lower((unsigned char)*b++);
	} while (x && x == y);
	return x - y;
}

int _strnicmp(const char *a, const char *b, size_t count)
{
	int x, y;

	for (; count; count--)
	{
		x = lower((unsigned char)*a++);
		y = lower((unsigned char)*b++);
		if (x != y)
			return x - y;
		if (!x)
			break;
	}
	return 0;
}

int strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
int strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }

char *strchr(const char *string, int c)
{
	for (;; string++)
	{
		if (*string == (char)c)
			return (char *)string;
		if (!*string)
			return NULL;
	}
}

char *strrchr(const char *string, int c)
{
	const char *found = NULL;

	for (;; string++)
	{
		if (*string == (char)c)
			found = string;
		if (!*string)
			return (char *)found;
	}
}

size_t strspn(const char *string, const char *accept)
{
	size_t n = 0;

	while (string[n] && strchr(accept, string[n]))
		n++;
	return n;
}

size_t strcspn(const char *string, const char *reject)
{
	size_t n = 0;

	while (string[n] && !strchr(reject, string[n]))
		n++;
	return n;
}

char *strpbrk(const char *string, const char *accept)
{
	string += strcspn(string, accept);
	return *string ? (char *)string : NULL;
}

char *strstr(const char *haystack, const char *needle)
{
	size_t n = strlen(needle);

	if (!n)
		return (char *)haystack;
	for (; *haystack; haystack++)
		if (*haystack == *needle && !strncmp(haystack, needle, n))
			return (char *)haystack;
	return NULL;
}

char *strtok(char *string, const char *delimiters)
{
	static char *next;
	char *start;

	if (string)
		next = string;
	if (!next)
		return NULL;
	next += strspn(next, delimiters);
	if (!*next)
	{
		next = NULL;
		return NULL;
	}
	start = next;
	next += strcspn(next, delimiters);
	if (*next)
		*next++ = 0;
	else
		next = NULL;
	return start;
}

char *strdup(const char *string)
{
	size_t n = strlen(string) + 1;
	char *copy = malloc(n);

	if (copy)
		memcpy(copy, string, n);
	return copy;
}


char *strerror(int error)
{
	static char text[32];
	const char *s = "error";

	switch (error)
	{
	case 0: s = "no error"; break;
	case ENOENT: s = "no such file or directory"; break;
	case EACCES: s = "permission denied"; break;
	case ENOMEM: s = "out of memory"; break;
	case EINVAL: s = "invalid argument"; break;
	case EIO: s = "input/output error"; break;
	}
	strncpy(text, s, sizeof(text) - 1);
	return text;
}

/* ---------- errno */

static int error_number;

int *_errno(void) { return &error_number; }
int *__errno_location(void) { return &error_number; }

/* ---------- character classes (ASCII; glibc's table interface for toupper and tolower) */

int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isprint(int c) { return c >= 0x20 && c < 0x7F; }
int isgraph(int c) { return c > 0x20 && c < 0x7F; }
int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7F; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }

static int32_t lower_table[384], upper_table[384];
static const int32_t *lower_pointer, *upper_pointer;

static void ctype_tables(void)
{
	int c;

	if (lower_pointer)
		return;
	for (c = -128; c < 256; c++)
	{
		lower_table[c + 128] = c >= 0 ? tolower(c) : c;
		upper_table[c + 128] = c >= 0 ? toupper(c) : c;
	}
	lower_pointer = lower_table + 128;
	upper_pointer = upper_table + 128;
}

const int32_t **__ctype_tolower_loc(void) { ctype_tables(); return &lower_pointer; }

static unsigned short class_table[384];
static const unsigned short *class_pointer;

const unsigned short **__ctype_b_loc(void)
{
	int c;

	if (!class_pointer)
	{
		for (c = 0; c < 256; c++)
		{
			unsigned short bits = 0;

			if (isupper(c)) bits |= _ISupper;
			if (islower(c)) bits |= _ISlower;
			if (isalpha(c)) bits |= _ISalpha;
			if (isdigit(c)) bits |= _ISdigit;
			if (isxdigit(c)) bits |= _ISxdigit;
			if (isspace(c)) bits |= _ISspace;
			if (isprint(c)) bits |= _ISprint;
			if (isgraph(c)) bits |= _ISgraph;
			if (c == ' ' || c == '\t') bits |= _ISblank;
			if (iscntrl(c)) bits |= _IScntrl;
			if (ispunct(c)) bits |= _ISpunct;
			if (isalnum(c)) bits |= _ISalnum;
			class_table[c + 128] = bits;
		}
		class_pointer = class_table + 128;
	}
	return &class_pointer;
}
const int32_t **__ctype_toupper_loc(void) { ctype_tables(); return &upper_pointer; }

/* ---------- conversions */

unsigned long strtoul(const char *string, char **end, int base)
{
	const char *p = string;
	unsigned long value = 0;
	int negative = 0, any = 0, digit;

	while (isspace((unsigned char)*p))
		p++;
	if (*p == '+' || *p == '-')
		negative = *p++ == '-';
	if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2]))
	{
		p += 2;
		base = 16;
	}
	else if (base == 0)
		base = *p == '0' ? 8 : 10;
	for (;; p++)
	{
		if (isdigit((unsigned char)*p)) digit = *p - '0';
		else if (isalpha((unsigned char)*p)) digit = lower((unsigned char)*p) - 'a' + 10;
		else break;
		if (digit >= base)
			break;
		value = value * base + digit;
		any = 1;
	}
	if (end)
		*end = (char *)(any ? p : string);
	return negative ? (unsigned long)-(long)value : value;
}

long strtol(const char *string, char **end, int base)
{
	const char *p = string;
	int negative;
	unsigned long value;

	while (isspace((unsigned char)*p))
		p++;
	negative = *p == '-';
	value = strtoul(string, end, base);
	if (negative)
		return (long)value;
	return value > LONG_MAX ? LONG_MAX : (long)value;
}

long long strtoll(const char *string, char **end, int base) { return strtol(string, end, base); }
unsigned long long strtoull(const char *string, char **end, int base) { return strtoul(string, end, base); }

int atoi(const char *string) { return (int)strtol(string, NULL, 10); }
long atol(const char *string) { return strtol(string, NULL, 10); }

double strtod(const char *string, char **end)
{
	const char *p = string;
	double value = 0.0, scale;
	int negative = 0, any = 0, exponent = 0, e_negative, e;

	while (isspace((unsigned char)*p))
		p++;
	if (*p == '+' || *p == '-')
		negative = *p++ == '-';
	if (!_strnicmp(p, "inf", 3))
	{
		p += _strnicmp(p, "infinity", 8) ? 3 : 8;
		if (end) *end = (char *)p;
		return negative ? -__builtin_inf() : __builtin_inf();
	}
	if (!_strnicmp(p, "nan", 3))
	{
		if (end) *end = (char *)p + 3;
		return __builtin_nan("");
	}
	for (; isdigit((unsigned char)*p); p++, any = 1)
		value = value * 10.0 + (*p - '0');
	if (*p == '.')
	{
		for (p++; isdigit((unsigned char)*p); p++, any = 1)
		{
			value = value * 10.0 + (*p - '0');
			exponent--;
		}
	}
	if (any && (*p == 'e' || *p == 'E'))
	{
		const char *q = p + 1;

		e_negative = 0;
		if (*q == '+' || *q == '-')
			e_negative = *q++ == '-';
		if (isdigit((unsigned char)*q))
		{
			for (e = 0; isdigit((unsigned char)*q); q++)
				if (e < 10000)
					e = e * 10 + (*q - '0');
			exponent += e_negative ? -e : e;
			p = q;
		}
	}
	if (end)
		*end = (char *)(any ? p : string);
	if (!any)
		return 0.0;
	/* scale by 10^exponent with few roundings */
	scale = 1.0;
	e = exponent < 0 ? -exponent : exponent;
	{
		double power = 10.0;

		while (e)
		{
			if (e & 1)
				scale *= power;
			power *= power;
			e >>= 1;
		}
	}
	value = exponent < 0 ? value / scale : value * scale;
	return negative ? -value : value;
}

float strtof(const char *string, char **end) { return (float)strtod(string, end); }
double atof(const char *string) { return strtod(string, NULL); }

/* ---------- sorting (Shell's sort: in place, no recursion) */

static void swap_bytes(unsigned char *a, unsigned char *b, size_t size)
{
	unsigned char t;

	while (size--)
	{
		t = *a;
		*a++ = *b;
		*b++ = t;
	}
}

void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
	unsigned char *array = base;
	size_t gap, i, j;

	for (gap = 1; gap < count / 3; gap = gap * 3 + 1)
		;
	for (; gap > 0; gap /= 3)
		for (i = gap; i < count; i++)
			for (j = i; j >= gap && compare(array + (j - gap) * size, array + j * size) > 0; j -= gap)
				swap_bytes(array + (j - gap) * size, array + j * size, size);
}

void *bsearch(const void *key, const void *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
	const unsigned char *array = base;
	size_t low = 0, high = count;

	while (low < high)
	{
		size_t middle = (low + high) / 2;
		int c = compare(key, array + middle * size);

		if (!c) return (void *)(array + middle * size);
		if (c < 0) high = middle; else low = middle + 1;
	}
	return NULL;
}

/* ---------- MSVC's rand: RAND_MAX 0x7FFF (random_numbers.c divides by 32767.0) */

static unsigned long random_state = 1;

void srand(unsigned int seed) { random_state = seed; }

int rand(void)
{
	random_state = random_state * 214013 + 2531011;
	return (int)((random_state >> 16) & 0x7FFF);
}

int abs(int value) { return value < 0 ? -value : value; }
long labs(long value) { return value < 0 ? -value : value; }

/* ---------- time: the Xbox's clock (UTC; the dashboard's time zone is not applied) */

static long long unix_time(void)
{
	SYSTEMTIME system;
	FILETIME file;
	unsigned long long ticks;

	GetSystemTime(&system);
	SystemTimeToFileTime(&system, &file);
	ticks = ((unsigned long long)file.dwHighDateTime << 32) | file.dwLowDateTime;
	return (long long)(ticks / 10000000ULL) - 11644473600LL;
}

time_t time(time_t *result)
{
	time_t now = (time_t)unix_time();

	if (result)
		*result = now;
	return now;
}

static struct tm *split_time(long long t, struct tm *out)
{
	static const int days_in_month[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	long long days = t / 86400, seconds = t % 86400;
	int year = 1970, month = 0, leap;

	if (seconds < 0) { seconds += 86400; days--; }
	memset(out, 0, sizeof(*out));
	out->tm_hour = (int)(seconds / 3600);
	out->tm_min = (int)(seconds / 60 % 60);
	out->tm_sec = (int)(seconds % 60);
	out->tm_wday = (int)((days + 4) % 7 + 7) % 7;
	for (;;)
	{
		leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
		if (days < 365 + leap)
			break;
		days -= 365 + leap;
		year++;
	}
	out->tm_yday = (int)days;
	for (month = 0; month < 12; month++)
	{
		int n = days_in_month[month] + (month == 1 && leap);

		if (days < n)
			break;
		days -= n;
	}
	out->tm_year = year - 1900;
	out->tm_mon = month;
	out->tm_mday = (int)days + 1;
	return out;
}

struct tm *gmtime(const time_t *t)
{
	static struct tm result;

	return split_time(*t, &result);
}

struct tm *localtime(const time_t *t)
{
	return gmtime(t);
}

time_t mktime(struct tm *t)
{
	static const int before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	long long year = t->tm_year + 1900, days;

	days = (year - 1970) * 365 + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;
	days += before[t->tm_mon % 12] + t->tm_mday - 1;
	if (t->tm_mon > 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
		days++;
	return (time_t)(days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
}

clock_t clock(void)
{
	return (clock_t)GetTickCount();
}

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);

size_t strftime(char *buffer, size_t size, const char *format, const struct tm *t)
{
	static const char *const days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
	static const char *const months[12] = { "January", "February", "March", "April", "May", "June", "July",
		"August", "September", "October", "November", "December" };
	size_t length = 0;
	char piece[64];

	if (!size)
		return 0;
	for (; *format; format++)
	{
		if (*format != '%')
		{
			piece[0] = *format;
			piece[1] = 0;
		}
		else
		{
			format++;
			if (*format == '#')
				format++;
			switch (*format)
			{
			case 'Y': halo_linux_snprintf(piece, sizeof(piece), "%d", t->tm_year + 1900); break;
			case 'y': halo_linux_snprintf(piece, sizeof(piece), "%02d", t->tm_year % 100); break;
			case 'm': halo_linux_snprintf(piece, sizeof(piece), "%02d", t->tm_mon + 1); break;
			case 'd': halo_linux_snprintf(piece, sizeof(piece), "%02d", t->tm_mday); break;
			case 'H': halo_linux_snprintf(piece, sizeof(piece), "%02d", t->tm_hour); break;
			case 'I': halo_linux_snprintf(piece, sizeof(piece), "%02d", (t->tm_hour + 11) % 12 + 1); break;
			case 'M': halo_linux_snprintf(piece, sizeof(piece), "%02d", t->tm_min); break;
			case 'S': halo_linux_snprintf(piece, sizeof(piece), "%02d", t->tm_sec); break;
			case 'p': strcpy(piece, t->tm_hour < 12 ? "AM" : "PM"); break;
			case 'j': halo_linux_snprintf(piece, sizeof(piece), "%03d", t->tm_yday + 1); break;
			case 'a': halo_linux_snprintf(piece, sizeof(piece), "%.3s", days[t->tm_wday % 7]); break;
			case 'A': strcpy(piece, days[t->tm_wday % 7]); break;
			case 'b': halo_linux_snprintf(piece, sizeof(piece), "%.3s", months[t->tm_mon % 12]); break;
			case 'B': strcpy(piece, months[t->tm_mon % 12]); break;
			case 'c': halo_linux_snprintf(piece, sizeof(piece), "%02d/%02d/%02d %02d:%02d:%02d", t->tm_mon + 1,
				t->tm_mday, t->tm_year % 100, t->tm_hour, t->tm_min, t->tm_sec); break;
			case 'x': halo_linux_snprintf(piece, sizeof(piece), "%02d/%02d/%02d", t->tm_mon + 1, t->tm_mday,
				t->tm_year % 100); break;
			case 'X': halo_linux_snprintf(piece, sizeof(piece), "%02d:%02d:%02d", t->tm_hour, t->tm_min,
				t->tm_sec); break;
			case '%': strcpy(piece, "%"); break;
			default: piece[0] = 0; break;
			}
		}
		if (length + strlen(piece) >= size)
			return 0;
		strcpy(buffer + length, piece);
		length += strlen(piece);
	}
	buffer[length] = 0;
	return length;
}

/* ---------- program end: back to the dashboard */

void exit(int status)
{
	platform_log("exit(%d)", status);
	XLaunchNewImageA(NULL, NULL);
	for (;;)
		Sleep(1000);
}

void abort(void)
{
	exit(3);
}

void __assert_fail(const char *assertion, const char *file, unsigned int line, const char *function)
{
	platform_log("assertion failed: %s (%s:%u, %s)", assertion, file, line, function);
	exit(3);
}

/* glibc's headers give the platform units (built with _GNU_SOURCE) C23's names for these, and the game's units
(ISO C) the plain ones: both link here */
unsigned long plain_strtoul(const char *string, char **end, int base) __asm__("strtoul");
unsigned long plain_strtoul(const char *string, char **end, int base) { return strtoul(string, end, base); }
long plain_strtol(const char *string, char **end, int base) __asm__("strtol");
long plain_strtol(const char *string, char **end, int base) { return strtol(string, end, base); }
long long plain_strtoll(const char *string, char **end, int base) __asm__("strtoll");
long long plain_strtoll(const char *string, char **end, int base) { return strtol(string, end, base); }
unsigned long long plain_strtoull(const char *string, char **end, int base) __asm__("strtoull");
unsigned long long plain_strtoull(const char *string, char **end, int base) { return strtoul(string, end, base); }
