/*
OXBOX_STDIO.C

C streams for the game on the Xbox: FILE over XAPI file handles (paths are the game's own Xbox paths), the
printf family (stb_sprintf, which also reads MSVC's %I64 and %I32), and the little scanf the game uses. stdout
and stderr go to the log.
*/

#define __NO_CTYPE 1
#include "platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define STB_SPRINTF_IMPLEMENTATION
#define STB_SPRINTF_NOUNALIGNED
#include "stb_sprintf.h"

void oxbox_log_text(const char *text, unsigned long length);

/* ---------- streams */

struct oxbox_file
{
	HANDLE handle;
	int console;            /* stdout/stderr: lines go to the log */
	int mirror;             /* debug.txt: also copied to the log's ring */
	int readable, writable, append;
	int error, eof;
	int unget;              /* -1: none */
	unsigned char *buffer;  /* read buffer */
	unsigned long buffer_size, buffer_position, buffer_length;
	unsigned char *write_buffer;
	unsigned long write_length;
	char line[512];
	unsigned long line_length;
};

#define BUFFER_SIZE 0x4000

static struct oxbox_file console_out = { INVALID_HANDLE_VALUE, 1, 0, 1, 0, 0, 0, -1 };
static struct oxbox_file console_err = { INVALID_HANDLE_VALUE, 1, 0, 1, 0, 0, 0, -1 };
static struct oxbox_file console_in = { INVALID_HANDLE_VALUE, 1, 1, 0, 0, 0, 1, -1 };
FILE *stdout = (FILE *)&console_out;
FILE *stderr = (FILE *)&console_err;
FILE *stdin = (FILE *)&console_in;

static int flush_writes(struct oxbox_file *f)
{
	unsigned long written;

	if (f->write_length && f->handle != INVALID_HANDLE_VALUE)
	{
		if (!WriteFile(f->handle, f->write_buffer, f->write_length, &written, NULL) || written != f->write_length)
			f->error = 1;
	}
	f->write_length = 0;
	return f->error ? EOF : 0;
}

/* drop read-ahead: put the file pointer where the reader is */
static void drop_reads(struct oxbox_file *f)
{
	long back = (long)(f->buffer_length - f->buffer_position) + (f->unget >= 0 ? 1 : 0);

	if (back && f->handle != INVALID_HANDLE_VALUE)
		SetFilePointer(f->handle, -back, NULL, FILE_CURRENT);
	f->buffer_position = f->buffer_length = 0;
	f->unget = -1;
}

FILE *halo_linux_fopen(const char *path, const char *mode)
{
	struct oxbox_file *f;
	DWORD access = 0, disposition = OPEN_EXISTING;
	int plus = strchr(mode, '+') != NULL;
	HANDLE handle;

	switch (mode[0])
	{
	case 'r': access = GENERIC_READ | (plus ? GENERIC_WRITE : 0); disposition = OPEN_EXISTING; break;
	case 'w': access = GENERIC_WRITE | (plus ? GENERIC_READ : 0); disposition = CREATE_ALWAYS; break;
	case 'a': access = GENERIC_WRITE | (plus ? GENERIC_READ : 0); disposition = OPEN_ALWAYS; break;
	default: return NULL;
	}
	handle = CreateFileA(path, access, FILE_SHARE_READ, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
	{
		/* (the optional files, which are normally absent: d:\init.txt's console commands, d:\oxbox_input.txt's
		scripted presses and the last-used settings z:\last*.txt before their first save) */
		DWORD error = GetLastError();
		if (!(error == ERROR_FILE_NOT_FOUND && mode[0] == 'r' &&
			(strstr(path, "init.txt") || strstr(path, "oxbox_input.txt") || strstr(path, "z:\\last"))))
		{
			platform_log("fopen(\"%s\", \"%s\") failed: error %lu", path, mode, error);
		}
		return NULL;
	}
	f = calloc(1, sizeof(*f));
	if (!f)
	{
		CloseHandle(handle);
		return NULL;
	}
	f->handle = handle;
	f->readable = (access & GENERIC_READ) != 0;
	f->writable = (access & GENERIC_WRITE) != 0;
	f->append = mode[0] == 'a';
	f->unget = -1;
	f->mirror = strstr(path, "debug.txt") != NULL;
	if (f->append)
		SetFilePointer(handle, 0, NULL, FILE_END);
	return (FILE *)f;
}

FILE *fopen(const char *path, const char *mode)
{
	return halo_linux_fopen(path, mode);
}

FILE *halo_linux_freopen(const char *path, const char *mode, FILE *stream)
{
	if (stream && ((struct oxbox_file *)stream)->console)
		return stream;
	if (stream)
		fclose(stream);
	return path ? halo_linux_fopen(path, mode) : NULL;
}

int fclose(FILE *stream)
{
	struct oxbox_file *f = (struct oxbox_file *)stream;
	int result;

	if (!f || f->console)
		return 0;
	result = flush_writes(f);
	CloseHandle(f->handle);
	free(f->buffer);
	free(f->write_buffer);
	free(f);
	return result;
}

int fflush(FILE *stream)
{
	struct oxbox_file *f = (struct oxbox_file *)stream;

	if (!f || f->console)
		return 0;
	return flush_writes(f);
}

static void console_write(struct oxbox_file *f, const char *data, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
	{
		if (data[i] == '\n' || f->line_length >= sizeof(f->line) - 1)
		{
			f->line[f->line_length] = 0;
			platform_log("%s", f->line);
			f->line_length = 0;
			if (data[i] == '\n')
				continue;
		}
		f->line[f->line_length++] = data[i];
	}
}

size_t fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
	struct oxbox_file *f = (struct oxbox_file *)stream;
	unsigned long total = (unsigned long)(size * count), written;

	if (!f || !total)
		return 0;
	if (f->console)
	{
		console_write(f, data, total);
		return count;
	}
	if (!f->writable)
	{
		f->error = 1;
		return 0;
	}
	if (f->mirror)
		oxbox_log_text(data, total);
	if (f->buffer_length || f->unget >= 0)
		drop_reads(f);
	if (total >= BUFFER_SIZE)
	{
		flush_writes(f);
		if (!WriteFile(f->handle, data, total, &written, NULL))
		{
			f->error = 1;
			return 0;
		}
		return written / size;
	}
	if (!f->write_buffer && !(f->write_buffer = malloc(BUFFER_SIZE)))
		return 0;
	if (f->write_length + total > BUFFER_SIZE)
		flush_writes(f);
	memcpy(f->write_buffer + f->write_length, data, total);
	f->write_length += total;
	return count;
}

static int fill(struct oxbox_file *f)
{
	unsigned long got = 0;

	if (f->console || f->handle == INVALID_HANDLE_VALUE)
		return 0;
	flush_writes(f);
	if (!f->buffer && !(f->buffer = malloc(BUFFER_SIZE)))
		return 0;
	if (!ReadFile(f->handle, f->buffer, BUFFER_SIZE, &got, NULL))
		f->error = 1;
	f->buffer_position = 0;
	f->buffer_length = got;
	if (!got)
		f->eof = 1;
	return (int)got;
}

int fgetc(FILE *stream)
{
	struct oxbox_file *f = (struct oxbox_file *)stream;
	int c;

	if (!f || !f->readable)
		return EOF;
	if (f->unget >= 0)
	{
		c = f->unget;
		f->unget = -1;
		return c;
	}
	if (f->buffer_position >= f->buffer_length && !fill(f))
		return EOF;
	return f->buffer[f->buffer_position++];
}

int getc(FILE *stream) { return fgetc(stream); }
int _IO_getc(FILE *stream) { return fgetc(stream); }

int ungetc(int c, FILE *stream)
{
	struct oxbox_file *f = (struct oxbox_file *)stream;

	if (!f || c == EOF)
		return EOF;
	if (f->buffer_position > 0 && f->unget < 0)
	{
		f->buffer[--f->buffer_position] = (unsigned char)c;
		return c;
	}
	f->unget = (unsigned char)c;
	f->eof = 0;
	return c;
}

size_t fread(void *data, size_t size, size_t count, FILE *stream)
{
	struct oxbox_file *f = (struct oxbox_file *)stream;
	unsigned char *out = data;
	unsigned long total = (unsigned long)(size * count), done = 0, got;

	if (!f || !total || !f->readable)
		return 0;
	if (f->unget >= 0 && done < total)
	{
		out[done++] = (unsigned char)f->unget;
		f->unget = -1;
	}
	while (done < total)
	{
		unsigned long available = f->buffer_length - f->buffer_position;

		if (available)
		{
			unsigned long n = available < total - done ? available : total - done;

			memcpy(out + done, f->buffer + f->buffer_position, n);
			f->buffer_position += n;
			done += n;
			continue;
		}
		if (total - done >= BUFFER_SIZE)
		{
			/* large reads go straight into the caller's memory */
			flush_writes(f);
			if (!ReadFile(f->handle, out + done, total - done, &got, NULL))
				f->error = 1;
			if (!got)
			{
				f->eof = 1;
				break;
			}
			done += got;
			continue;
		}
		if (!fill(f))
			break;
	}
	return done / size;
}

int fputc(int c, FILE *stream)
{
	unsigned char byte = (unsigned char)c;

	return fwrite(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

int putc(int c, FILE *stream) { return fputc(c, stream); }
int _IO_putc(int c, FILE *stream) { return fputc(c, stream); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *string, FILE *stream)
{
	size_t n = strlen(string);

	return fwrite(string, 1, n, stream) == n ? 0 : EOF;
}

int puts(const char *string)
{
	fputs(string, stdout);
	return fputc('\n', stdout);
}

char *fgets(char *buffer, int size, FILE *stream)
{
	int i = 0, c;

	if (size <= 0)
		return NULL;
	while (i < size - 1)
	{
		c = fgetc(stream);
		if (c == EOF)
			break;
		buffer[i++] = (char)c;
		if (c == '\n')
			break;
	}
	if (!i)
		return NULL;
	buffer[i] = 0;
	return buffer;
}

int fseek(FILE *stream, long offset, int origin)
{
	struct oxbox_file *f = (struct oxbox_file *)stream;
	DWORD method = origin == SEEK_SET ? FILE_BEGIN : origin == SEEK_CUR ? FILE_CURRENT : FILE_END;

	if (!f || f->console)
		return -1;
	flush_writes(f);
	drop_reads(f);
	f->eof = 0;
	return SetFilePointer(f->handle, offset, NULL, method) == INVALID_SET_FILE_POINTER ? -1 : 0;
}

long ftell(FILE *stream)
{
	struct oxbox_file *f = (struct oxbox_file *)stream;
	DWORD position;

	if (!f || f->console)
		return -1;
	position = SetFilePointer(f->handle, 0, NULL, FILE_CURRENT);
	if (position == INVALID_SET_FILE_POINTER)
		return -1;
	return (long)position + (long)f->write_length - (long)(f->buffer_length - f->buffer_position) -
		(f->unget >= 0 ? 1 : 0);
}

void rewind(FILE *stream)
{
	fseek(stream, 0, SEEK_SET);
	if (stream)
		((struct oxbox_file *)stream)->error = 0;
}

int ferror(FILE *stream) { return stream ? ((struct oxbox_file *)stream)->error : 1; }
int feof(FILE *stream) { return stream ? ((struct oxbox_file *)stream)->eof && ((struct oxbox_file *)stream)->unget < 0 : 1; }
void clearerr(FILE *stream) { if (stream) ((struct oxbox_file *)stream)->error = ((struct oxbox_file *)stream)->eof = 0; }
int fileno(FILE *stream) { return -1; }

int halo_linux_remove(const char *path) { return DeleteFileA(path) ? 0 : -1; }
int remove(const char *path) { return halo_linux_remove(path); }
int halo_linux_rename(const char *from, const char *to) { return MoveFileA(from, to) ? 0 : -1; }

/* ---------- printf */

int halo_linux_vsnprintf(char *buffer, size_t count, const char *format, va_list arguments)
{
	return stbsp_vsnprintf(buffer, (int)(count > 0x7FFFFFFF ? 0x7FFFFFFF : count), format, arguments);
}

int vsnprintf(char *buffer, size_t count, const char *format, va_list arguments)
{
	return halo_linux_vsnprintf(buffer, count, format, arguments);
}

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vsnprintf(buffer, count, format, arguments);
	va_end(arguments);
	return result;
}

int snprintf(char *buffer, size_t count, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vsnprintf(buffer, count, format, arguments);
	va_end(arguments);
	return result;
}

int halo_linux_vsprintf(char *buffer, const char *format, va_list arguments)
{
	return stbsp_vsprintf(buffer, format, arguments);
}

int vsprintf(char *buffer, const char *format, va_list arguments)
{
	return stbsp_vsprintf(buffer, format, arguments);
}

int halo_linux_sprintf(char *buffer, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = stbsp_vsprintf(buffer, format, arguments);
	va_end(arguments);
	return result;
}

int sprintf(char *buffer, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = stbsp_vsprintf(buffer, format, arguments);
	va_end(arguments);
	return result;
}

static char *stream_callback(const char *buffer, void *user, int length)
{
	fwrite(buffer, 1, length, (FILE *)user);
	return (char *)buffer;
}

int halo_linux_vfprintf(FILE *stream, const char *format, va_list arguments)
{
	char buffer[STB_SPRINTF_MIN];

	return stbsp_vsprintfcb(stream_callback, stream, buffer, format, arguments);
}

int vfprintf(FILE *stream, const char *format, va_list arguments)
{
	return halo_linux_vfprintf(stream, format, arguments);
}

int halo_linux_fprintf(FILE *stream, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vfprintf(stream, format, arguments);
	va_end(arguments);
	return result;
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vfprintf(stream, format, arguments);
	va_end(arguments);
	return result;
}

int halo_linux_vprintf(const char *format, va_list arguments)
{
	return halo_linux_vfprintf(stdout, format, arguments);
}

int halo_linux_printf(const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vfprintf(stdout, format, arguments);
	va_end(arguments);
	return result;
}

int printf(const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_linux_vfprintf(stdout, format, arguments);
	va_end(arguments);
	return result;
}

/* ---------- scanf: numbers, strings, characters and sets, over a string or a stream */

struct scan_source
{
	const char *string;
	FILE *file;
	int count;
};

static int scan_get(struct scan_source *s)
{
	int c;

	if (s->string)
		c = *s->string ? (unsigned char)*s->string++ : EOF;
	else
		c = fgetc(s->file);
	if (c != EOF)
		s->count++;
	return c;
}

static void scan_unget(struct scan_source *s, int c)
{
	if (c == EOF)
		return;
	s->count--;
	if (s->string)
		s->string--;
	else
		ungetc(c, s->file);
}

static int scan(struct scan_source *s, const char *format, va_list arguments)
{
	int assigned = 0, c, any_input = 0;

	for (; *format; format++)
	{
		int suppress = 0, width = 0, length = 0;   /* length: -2 hh, -1 h, 0, 1 l, 2 ll */
		char conversion;

		if (isspace((unsigned char)*format))
		{
			do c = scan_get(s); while (c != EOF && isspace(c));
			scan_unget(s, c);
			continue;
		}
		if (*format != '%' || format[1] == '%')
		{
			if (*format == '%')
				format++;
			c = scan_get(s);
			if (c != (unsigned char)*format)
			{
				scan_unget(s, c);
				break;
			}
			continue;
		}
		format++;
		if (*format == '*')
		{
			suppress = 1;
			format++;
		}
		while (isdigit((unsigned char)*format))
			width = width * 10 + (*format++ - '0');
		while (*format == 'h' || *format == 'l' || *format == 'L' || *format == 'I')
		{
			if (*format == 'I' && format[1] == '6' && format[2] == '4') { length = 2; format += 3; continue; }
			if (*format == 'h') length = length == -1 ? -2 : -1;
			else length = length >= 1 ? 2 : 1;
			format++;
		}
		conversion = *format;
		if (conversion == 'n')
		{
			if (!suppress)
				*va_arg(arguments, int *) = s->count;
			continue;
		}
		if (conversion != 'c' && conversion != '[')
		{
			do c = scan_get(s); while (c != EOF && isspace(c));
			scan_unget(s, c);
		}
		if (!width)
			width = conversion == 'c' ? 1 : 0x7FFFFFFF;
		switch (conversion)
		{
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p':
		case 'f': case 'e': case 'g': case 'E': case 'G': case 'a':
		{
			char text[128];
			int n = 0, floating = strchr("feEgGa", conversion) != NULL;
			char *end;

			while (n < width && n < (int)sizeof(text) - 1)
			{
				c = scan_get(s);
				if (c == EOF)
					break;
				if (isalnum(c) || c == '+' || c == '-' || (floating && c == '.'))
				{
					/* stop a number at its end: a sign only first (or after an exponent) */
					if ((c == '+' || c == '-') && n && !(floating && (text[n - 1] == 'e' || text[n - 1] == 'E')))
					{
						scan_unget(s, c);
						break;
					}
					if (!floating && !(isxdigit(c) || c == 'x' || c == 'X' || c == '+' || c == '-'))
					{
						scan_unget(s, c);
						break;
					}
					if (floating && isalpha(c) && c != 'e' && c != 'E' && !(n == 0) &&
						!(text[0] == 'i' || text[0] == 'n' || text[0] == 'I' || text[0] == 'N'))
					{
						scan_unget(s, c);
						break;
					}
					text[n++] = (char)c;
				}
				else
				{
					scan_unget(s, c);
					break;
				}
			}
			text[n] = 0;
			if (!n)
				return any_input || c != EOF ? assigned : (assigned ? assigned : EOF);
			any_input = 1;
			if (floating)
			{
				double v = strtod(text, &end);

				if (end == text)
					return assigned;
				if (!suppress)
				{
					if (length >= 1) *va_arg(arguments, double *) = v;
					else *va_arg(arguments, float *) = (float)v;
					assigned++;
				}
			}
			else
			{
				int base = conversion == 'x' || conversion == 'X' || conversion == 'p' ? 16 :
					conversion == 'o' ? 8 : conversion == 'i' ? 0 : 10;
				unsigned long v = strtoul(text, &end, base);

				if (end == text)
					return assigned;
				if (!suppress)
				{
					if (length == -2) *va_arg(arguments, char *) = (char)v;
					else if (length == -1) *va_arg(arguments, short *) = (short)v;
					else if (length == 2) *va_arg(arguments, long long *) = (long)v;
					else *va_arg(arguments, long *) = (long)v;
					assigned++;
				}
			}
			break;
		}
		case 's':
		{
			char *out = suppress ? NULL : va_arg(arguments, char *);
			int n = 0;

			while (n < width)
			{
				c = scan_get(s);
				if (c == EOF || isspace(c))
				{
					scan_unget(s, c);
					break;
				}
				if (out) out[n] = (char)c;
				n++;
			}
			if (!n)
				return assigned ? assigned : EOF;
			any_input = 1;
			if (out)
			{
				out[n] = 0;
				assigned++;
			}
			break;
		}
		case 'c':
		{
			char *out = suppress ? NULL : va_arg(arguments, char *);
			int n;

			for (n = 0; n < width; n++)
			{
				c = scan_get(s);
				if (c == EOF)
					return assigned ? assigned : EOF;
				if (out) out[n] = (char)c;
			}
			any_input = 1;
			if (out)
				assigned++;
			break;
		}
		case '[':
		{
			char set[256];
			int invert = 0, n = 0, i;
			char *out;

			memset(set, 0, sizeof(set));
			format++;
			if (*format == '^') { invert = 1; format++; }
			if (*format == ']') { set[']'] = 1; format++; }
			for (; *format && *format != ']'; format++)
			{
				if (format[1] == '-' && format[2] && format[2] != ']')
				{
					for (i = (unsigned char)format[0]; i <= (unsigned char)format[2]; i++) set[i] = 1;
					format += 2;
				}
				else
					set[(unsigned char)*format] = 1;
			}
			out = suppress ? NULL : va_arg(arguments, char *);
			while (n < width)
			{
				c = scan_get(s);
				if (c == EOF || !(set[c] ^ invert))
				{
					scan_unget(s, c);
					break;
				}
				if (out) out[n] = (char)c;
				n++;
			}
			if (!n)
				return assigned;
			any_input = 1;
			if (out)
			{
				out[n] = 0;
				assigned++;
			}
			break;
		}
		default:
			return assigned;
		}
	}
	return assigned;
}

int vsscanf(const char *string, const char *format, va_list arguments)
{
	struct scan_source s = { string, NULL, 0 };

	return scan(&s, format, arguments);
}

int __isoc99_sscanf(const char *string, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vsscanf(string, format, arguments);
	va_end(arguments);
	return result;
}

int sscanf(const char *string, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vsscanf(string, format, arguments);
	va_end(arguments);
	return result;
}

int __isoc99_fscanf(FILE *stream, const char *format, ...)
{
	struct scan_source s = { NULL, stream, 0 };
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = scan(&s, format, arguments);
	va_end(arguments);
	return result;
}

int fscanf(FILE *stream, const char *format, ...)
{
	struct scan_source s = { NULL, stream, 0 };
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = scan(&s, format, arguments);
	va_end(arguments);
	return result;
}
