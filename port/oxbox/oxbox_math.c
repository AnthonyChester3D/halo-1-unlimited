/*
OXBOX_MATH.C

The maths the game calls beyond musl's (port/third_party/musl-math gives halo_sin and the rest): the exact
functions on the x87 (sqrt, floor, ceil, rint, fmod, scalbn), float versions, and MSVC's _control87 over the x87
control word. Written in assembly where a C body could be turned back into a call to itself.
*/

#include "platform.h"

#include <float.h>
#include <math.h>

double halo_atan(double x);
double halo_tan(double x);

static unsigned short control_word(void)
{
	unsigned short word;

	__asm__ volatile ("fnstcw %0" : "=m"(word));
	return word;
}

static void set_control_word(unsigned short word)
{
	__asm__ volatile ("fldcw %0" : : "m"(word));
}

double sqrt(double x) { __asm__ ("fsqrt" : "+t"(x)); return x; }
float sqrtf(float x) { double d = x; __asm__ ("fsqrt" : "+t"(d)); return (float)d; }
double fabs(double x) { __asm__ ("fabs" : "+t"(x)); return x; }
float fabsf(float x) { double d = x; __asm__ ("fabs" : "+t"(d)); return (float)d; }

long lrint(double x) { long r; __asm__ ("fistpl %0" : "=m"(r) : "t"(x) : "st"); return r; }
long lrintf(float x) { long r; double d = x; __asm__ ("fistpl %0" : "=m"(r) : "t"(d) : "st"); return r; }

/* ---------- whole numbers (v1.183): without FRNDINT or a rounding mode change. On the Pentium III FRNDINT is ~30
micro-ops and each FLDCW 10 cycles that stall the x87 after it: the old floor (two FLDCWs around a FRNDINT) took ~3% of
the processor in a10's intro hangar shot, rintf ~1.5% (the console's sampling profile). Now: rounded in whatever mode
is set (FISTP, then FILD of the same 8 bytes, which the Pentium III forwards), stepped by one on the x87 when that went
past (exact at the game's 53-bit precision, CW_DEFAULT, or 64). Every result is the old one bit for bit, -0.0
included (checked over 48 million inputs in every rounding mode); from 2^52 up, infinities and NaNs are already whole
and come back as they are. One routine for all eight: the resident image had no room for more code than before */
#define WHOLE_DOUBLE 4503599627370496.0     /* 2^52 */

/* (way: 0 to nearest, -1 down, 1 up, 2 toward zero; the float versions through it too, exact for floats) */
static __attribute__((noinline)) double whole(double x, int way)
{
	long long i;
	double r;

	if (!(x < WHOLE_DOUBLE && x > -WHOLE_DOUBLE))
		return x;
	__asm__ ("fistpll %0" : "=m"(i) : "t"(x) : "st");
	r = (double)i;
	if (way == 2)
		way = x >= 0.0 ? -1 : 1;
	if (way < 0 && r > x)
		r -= 1.0;
	else if (way > 0 && r < x)
		r += 1.0;
	return r == 0.0 ? x * 0.0 : r;
}

#if defined(OXBOX_ROUND_COUNT) || defined(OXBOX_CALL_COUNT)
/* (lab builds only: calls counted by their caller, a table a kind (0 these rounding functions; OXBOX_CALL_COUNT adds
datum_get, object_get_and_verify_type, tag_get and tag_loaded, counted where they're defined); oxbox_round_dump logs
each kind's most frequent callers every 10 s) */
int snprintf(char *, unsigned int, char const *, ...);
void platform_log(char const *, ...);
enum { COUNT_KINDS = 5, COUNT_SLOTS = 1024 };
static char const *const count_names[COUNT_KINDS] = { "round", "datum_get", "object_get_and_verify_type", "tag_get",
	"tag_loaded" };
static struct { unsigned long caller, count; } count_table[COUNT_KINDS][COUNT_SLOTS];
static unsigned long count_calls[COUNT_KINDS], count_lost[COUNT_KINDS];

void oxbox_count_call(int kind, unsigned long caller)
{
	unsigned long slot = (caller * 2654435761u) >> 22, k;

	count_calls[kind]++;
	for (k = 0; k < COUNT_SLOTS; k++, slot = (slot + 1) & (COUNT_SLOTS - 1))
	{
		if (count_table[kind][slot].caller == caller)
		{
			count_table[kind][slot].count++;
			return;
		}
		if (!count_table[kind][slot].caller)
		{
			count_table[kind][slot].caller = caller;
			count_table[kind][slot].count = 1;
			return;
		}
	}
	count_lost[kind]++;
}

void oxbox_round_dump(void)
{
	int kind;

	for (kind = 0; kind < COUNT_KINDS; kind++)
	{
		char line[600];
		int used, n, k;

		if (!count_calls[kind])
			continue;
		used = snprintf(line, sizeof(line), "count %s: %lu calls in 10 s (%lu not counted); by caller:", count_names[kind],
			count_calls[kind], count_lost[kind]);
		for (n = 0; n < 24; n++)
		{
			int best = -1;

			for (k = 0; k < COUNT_SLOTS; k++)
				if (count_table[kind][k].count && (best < 0 || count_table[kind][k].count > count_table[kind][best].count))
					best = k;
			if (best < 0)
				break;
			if (used < (int)sizeof(line) - 24)
				used += snprintf(line + used, sizeof(line) - used, " %lx:%lu", count_table[kind][best].caller,
					count_table[kind][best].count);
			count_table[kind][best].count = 0;
		}
		platform_log("%s", line);
		for (k = 0; k < COUNT_SLOTS; k++)
			count_table[kind][k].caller = count_table[kind][k].count = 0;
		count_calls[kind] = count_lost[kind] = 0;
	}
}
#define COUNTED oxbox_count_call(0, (unsigned long)__builtin_return_address(0)),
#else
#define COUNTED
#endif

double rint(double x) { return COUNTED whole(x, 0); }
float rintf(float x) { return COUNTED (float)whole(x, 0); }
double floor(double x) { return COUNTED whole(x, -1); }
double ceil(double x) { return COUNTED whole(x, 1); }
double trunc(double x) { return COUNTED whole(x, 2); }
float floorf(float x) { return COUNTED (float)whole(x, -1); }
float ceilf(float x) { return COUNTED (float)whole(x, 1); }
float truncf(float x) { return COUNTED (float)whole(x, 2); }

double fmod(double x, double y)
{
	double r;

	__asm__ ("1: fprem\n\tfnstsw %%ax\n\tsahf\n\tjp 1b" : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
	return r;
}

float fmodf(float x, float y) { return (float)fmod(x, y); }

double scalbn(double x, int n)
{
	double r, e = n;

	__asm__ ("fscale" : "=t"(r) : "0"(x), "u"(e));
	return r;
}

double ldexp(double x, int n) { return scalbn(x, n); }
float ldexpf(float x, int n) { return (float)scalbn(x, n); }

double frexp(double x, int *exponent)
{
	union { double d; unsigned long long u; } v;
	int e;

	v.d = x;
	e = (int)((v.u >> 52) & 0x7FF);
	if (!e)
	{
		if (x == 0.0) { *exponent = 0; return x; }
		v.d = x * 18446744073709551616.0;   /* 2^64: a denormal */
		e = (int)((v.u >> 52) & 0x7FF) - 64;
	}
	if (e == 0x7FF) { *exponent = 0; return x; }
	*exponent = e - 1022;
	v.u = (v.u & ~(0x7FFULL << 52)) | (1022ULL << 52);
	return v.d;
}

double modf(double x, double *integer)
{
	*integer = trunc(x);
	return x - *integer;
}

float atanf(float x) { return (float)halo_atan(x); }
float tanf(float x) { return (float)halo_tan(x); }

/* ---------- MSVC's floating point control word, over the x87's */

static const struct { unsigned int msvc; unsigned short x87; } exception_masks[] =
{
	{ _EM_INVALID, 0x0001 }, { _EM_DENORMAL, 0x0002 }, { _EM_ZERODIVIDE, 0x0004 },
	{ _EM_OVERFLOW, 0x0008 }, { _EM_UNDERFLOW, 0x0010 }, { _EM_INEXACT, 0x0020 },
};

static unsigned int control_word_to_msvc(unsigned short word)
{
	unsigned int result = 0, index;

	for (index = 0; index < sizeof(exception_masks) / sizeof(exception_masks[0]); index++)
		if (word & exception_masks[index].x87)
			result |= exception_masks[index].msvc;
	switch ((word >> 8) & 3)
	{
	case 0: result |= _PC_24; break;
	case 2: result |= _PC_53; break;
	default: result |= _PC_64; break;
	}
	result |= (unsigned int)((word >> 10) & 3) << 8;
	if (word & 0x1000)
		result |= _IC_AFFINE;
	return result;
}

static unsigned short msvc_to_control_word(unsigned int value, unsigned short word)
{
	unsigned int index;

	word &= (unsigned short)~0x1f3f;
	for (index = 0; index < sizeof(exception_masks) / sizeof(exception_masks[0]); index++)
		if (value & exception_masks[index].msvc)
			word |= exception_masks[index].x87;
	switch (value & _MCW_PC)
	{
	case _PC_24: break;
	case _PC_53: word |= 2 << 8; break;
	default: word |= 3 << 8; break;
	}
	word |= (unsigned short)(((value & _MCW_RC) >> 8) << 10);
	if (value & _IC_AFFINE)
		word |= 0x1000;
	return word;
}

unsigned int _control87(unsigned int new_value, unsigned int mask)
{
	unsigned short word = control_word();
	unsigned int current = control_word_to_msvc(word);

	if (mask)
	{
		current = (current & ~mask) | (new_value & mask);
		set_control_word(msvc_to_control_word(current, word));
	}
	return current;
}

unsigned int _controlfp(unsigned int new_value, unsigned int mask)
{
	return _control87(new_value, mask & ~_EM_DENORMAL);
}

unsigned int _statusfp(void)
{
	unsigned short status;

	__asm__ volatile ("fnstsw %0" : "=m"(status));
	return status & 0x3F;
}

unsigned int _clearfp(void)
{
	unsigned int status = _statusfp();

	__asm__ volatile ("fnclex");
	return status;
}

int _isnan(double value) { return value != value; }
int _finite(double value) { return value == value && value - value == 0.0; }
