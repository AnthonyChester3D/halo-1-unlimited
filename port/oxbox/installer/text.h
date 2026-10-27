/* text.h: snprintf that always ends the text it writes (nxdk's PDCLib leaves it unended when it cuts it short) */
#ifndef OXBOX_TEXT_H
#define OXBOX_TEXT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

static inline int text_vprint(char *to, size_t size, char const *format, va_list list)
{
	int n = vsnprintf(to, size, format, list);
	if (size)
		to[size - 1] = 0;
	return n;
}

static inline int text_print(char *to, size_t size, char const *format, ...)
{
	va_list list;
	int n;
	va_start(list, format);
	n = text_vprint(to, size, format, list);
	va_end(list);
	return n;
}

#endif
