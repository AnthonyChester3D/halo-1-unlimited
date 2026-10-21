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

double rint(double x) { __asm__ ("frndint" : "+t"(x)); return x; }
float rintf(float x) { double d = x; __asm__ ("frndint" : "+t"(d)); return (float)d; }
long lrint(double x) { long r; __asm__ ("fistpl %0" : "=m"(r) : "t"(x) : "st"); return r; }
long lrintf(float x) { long r; double d = x; __asm__ ("fistpl %0" : "=m"(r) : "t"(d) : "st"); return r; }

/* round with the x87 rounding mode set to `mode` (0x0400 down, 0x0800 up, 0x0C00 toward zero) */
static double round_with(double x, unsigned short mode)
{
	unsigned short saved = control_word();

	set_control_word((unsigned short)((saved & ~0x0C00) | mode));
	__asm__ ("frndint" : "+t"(x));
	set_control_word(saved);
	return x;
}

double floor(double x) { return round_with(x, 0x0400); }
double ceil(double x) { return round_with(x, 0x0800); }
double trunc(double x) { return round_with(x, 0x0C00); }
float floorf(float x) { return (float)round_with(x, 0x0400); }
float ceilf(float x) { return (float)round_with(x, 0x0800); }
float truncf(float x) { return (float)round_with(x, 0x0C00); }

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
