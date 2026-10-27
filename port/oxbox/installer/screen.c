/* screen.c: the installer's text screen (screen.h) */
#include "screen.h"

#include <hal/video.h>
#include <xboxkrnl/xboxkrnl.h>

#include <stdarg.h>
#include <string.h>

#include "text.h"

static unsigned char const font[] = {
#include <hal/font_unscii_16.h>
};

#define LEFT ((640 - SCREEN_COLUMNS * 8) / 2)
#define TOP ((480 - SCREEN_ROWS * 16) / 2)

struct cell
{
	unsigned char c;
	unsigned long colour, background;
};

static struct cell wanted[SCREEN_ROWS][SCREEN_COLUMNS], shown[SCREEN_ROWS][SCREEN_COLUMNS];
static unsigned long row_background[SCREEN_ROWS];

void screen_init(void)
{
	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
	memset(shown, 0, sizeof(shown));
	screen_clear();
}

void screen_clear(void)
{
	int r, c;
	for (r = 0; r < SCREEN_ROWS; r++)
	{
		row_background[r] = COLOUR_BLACK;
		for (c = 0; c < SCREEN_COLUMNS; c++)
		{
			wanted[r][c].c = ' ';
			wanted[r][c].colour = COLOUR_WHITE;
			wanted[r][c].background = COLOUR_BLACK;
		}
	}
}

void screen_row(int row, unsigned long background)
{
	int c;
	if (row < 0 || row >= SCREEN_ROWS)
		return;
	row_background[row] = background;
	for (c = 0; c < SCREEN_COLUMNS; c++)
	{
		wanted[row][c].c = ' ';
		wanted[row][c].background = background;
	}
}

int screen_text(int row, int column, unsigned long colour, char const *format, ...)
{
	char text[256];
	va_list list;
	int i;
	if (row < 0 || row >= SCREEN_ROWS)
		return column;
	va_start(list, format);
	text_vprint(text, sizeof(text), format, list);
	va_end(list);
	for (i = 0; text[i] && column < SCREEN_COLUMNS; i++, column++)
	{
		if (column < 0)
			continue;
		wanted[row][column].c = (unsigned char)text[i];
		wanted[row][column].colour = colour;
		wanted[row][column].background = row_background[row];
	}
	return column;
}

static void draw_cell(unsigned char *fb, int row, int column, struct cell const *cell)
{
	unsigned char const *glyph = font + cell->c * 16;
	unsigned long *p = (unsigned long *)fb + (TOP + row * 16) * 640 + LEFT + column * 8;
	int y, x;
	for (y = 0; y < 16; y++, p += 640)
	{
		unsigned char bits = glyph[y];
		for (x = 0; x < 8; x++)
			p[x] = (bits & (0x80 >> x)) ? cell->colour : cell->background;
	}
}

void screen_show(void)
{
	unsigned char *fb = XVideoGetFB();
	int r, c;
	for (r = 0; r < SCREEN_ROWS; r++)
	{
		for (c = 0; c < SCREEN_COLUMNS; c++)
		{
			struct cell *w = &wanted[r][c], *s = &shown[r][c];
			if (w->c != s->c || w->colour != s->colour || w->background != s->background ||
				(!s->c && !s->colour && !s->background))
			{
				draw_cell(fb, r, c, w);
				*s = *w;
			}
		}
	}
	XVideoFlushFB();
}

void screen_hand_over(void)
{
	unsigned char *fb = XVideoGetFB();
	ULONG size = MmQueryAllocationSize(fb);
	memset(fb, 0, size ? size : 640 * 480 * 4);
	XVideoFlushFB();
	/* the black picture kept through the reboot, as the retail D3DDevice_PersistDisplay keeps one: its memory isn't
	reused for the next XBE, and the next XBE's video set-up frees it (Halo's does: it starts with the same free
	memory as when it's started from a dashboard). Without this, the next XBE's bytes showed in the old picture */
	if (size)
	{
		MmPersistContiguousMemory(fb, size, TRUE);
		AvSetSavedDataAddress(fb);
	}
}
