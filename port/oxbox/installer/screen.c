/* screen.c: the installer's text screen (screen.h) */
#include "screen.h"

#include <hal/video.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <math.h>

#include <stdarg.h>
#include <string.h>

#include "text.h"

static unsigned char const font[] = {
#include <hal/font_unscii_16.h>
};

#define TOP ((480 - SCREEN_ROWS * 16) / 2)

/* a cell's width on the screen: 8 pixels (draw_cell can draw narrower ones; installer 4 did for widescreen) */
static int cell_width = 8, left = (640 - SCREEN_COLUMNS * 8) / 2;

struct cell
{
	unsigned char c;
	unsigned long colour, background;
};

static struct cell wanted[SCREEN_ROWS][SCREEN_COLUMNS], shown[SCREEN_ROWS][SCREEN_COLUMNS];
static unsigned long row_background[SCREEN_ROWS];

/* the spinner's corner, which it draws itself while it runs (screen_show leaves it alone) */
#define SPIN_ROWS 6
#define SPIN_COLUMNS 12
#define SPIN_TOP (SCREEN_ROWS - SPIN_ROWS)
#define SPIN_LEFT (SCREEN_COLUMNS - SPIN_COLUMNS)
static volatile LONG spinner_running, spinner_wanted;
static HANDLE spinner_thread;

static int in_spinner(int row, int column)
{
	return spinner_running && row >= SPIN_TOP && column >= SPIN_LEFT;
}

void screen_init(void)
{
	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
	/* (a widescreen dashboard: the TV stretches this 640x480 picture to 16:9, the same pixels. Text drawn 6 pixels wide
	to make up for it was unreadable (installer 4); it's drawn 8 wide, a little stretched, as before) */
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
	unsigned long *p = (unsigned long *)fb + (TOP + row * 16) * 640 + left + column * cell_width;
	int y, x;
	for (y = 0; y < 16; y++, p += 640)
	{
		unsigned char bits = glyph[y];
		for (x = 0; x < cell_width; x++)
		{
			/* (narrower: each pixel takes the glyph's columns it covers, any of them set, so no stroke is lost) */
			int from = x * 8 / cell_width, to = ((x + 1) * 8 + cell_width - 1) / cell_width, k, on = 0;
			for (k = from; k < to && k < 8; k++)
				on |= bits & (0x80 >> k);
			p[x] = on ? cell->colour : cell->background;
		}
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
			if (in_spinner(r, c))
				continue;
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
	/* the screen blanked (the encoder's output), as a retail title's Direct3D does when it shuts down without
	keeping a picture: AvSendTVEncoderOption(AV_OPTION_BLANK_SCREEN, TRUE). The next title's Direct3D blanks it too
	while it starts, and turns it back on at its first frame. Nothing in memory shows meanwhile (this picture's
	memory is the next XBE's to load into).
	(Installer tests 2 and 3 kept a black picture instead, as Direct3D's D3DDevice_PersistDisplay keeps one for the
	next title: MmPersistContiguousMemory and AvSetSavedDataAddress. But Direct3D's kept picture is one of its own
	surfaces, a description and then the pixels: given just the pixels, Halo's Direct3D pointed the screen at the
	wrong memory for a fifth of a second as it started, the scrambled frames the user saw) */
	unsigned char *fb = XVideoGetFB();
	ULONG size = MmQueryAllocationSize(fb);
	memset(fb, 0, size ? size : 640 * 480 * 4);
	XVideoFlushFB();
	AvSendTVEncoderOption((PVOID)VIDEO_BASE, VIDEO_ENC_VIDEOENABLE, TRUE, NULL);
}

/* ---- the spinner: a Halo ring rising into the sky, turning, in characters (Llama's design), drawn in half-size
characters (4 x 8 pixels: 24 x 12 of them in the corner's 12 x 6 cells) by a thread of its own every 60 ms, so it
moves while the installer is busy reading, writing or downloading */

#define SPIN_WIDE (SPIN_COLUMNS * 2)
#define SPIN_HIGH (SPIN_ROWS * 2)
#define SPIN_BLUE 0x2E5FD0
#define SPIN_CYAN 0x40C8D8
#define SPIN_GREEN 0x50E060
#define SPIN_STONE 0xA8A8A8
#define SPIN_SNOW 0xF4F4F4
#define SPIN_METAL 0x7A7A7A
#define SPIN_BAND 0.3f

/* where a half-size character's place is on the ring (phi), and whether it's on it */
static float spinner_phi(int x, int y)
{
	float sy = ((float)SPIN_HIGH / 2.0f - (float)y - 0.5f) / ((float)SPIN_HIGH * 0.6f);
	float sx = ((float)x - (float)SPIN_WIDE / 2.0f + 0.5f) / ((float)SPIN_WIDE * 0.5f);
	return sx * (1.0f + sy * 0.4f);
}

static void spinner_cell(struct cell *cell, int x, int y, float t)
{
	float sy = ((float)SPIN_HIGH / 2.0f - (float)y - 0.5f) / ((float)SPIN_HIGH * 0.6f);
	float phi = spinner_phi(x, y), theta, value;
	cell->background = COLOUR_BLACK;
	cell->c = ' ';
	cell->colour = SPIN_METAL;
	if (fabsf(phi) >= SPIN_BAND)
		return;
	/* (its rims: the ring's edge characters on each row) */
	if (x == 0 || x == SPIN_WIDE - 1 || fabsf(spinner_phi(x - 1, y)) >= SPIN_BAND ||
		fabsf(spinner_phi(x + 1, y)) >= SPIN_BAND)
	{
		cell->c = y >= SPIN_HIGH / 2 ? '|' : '^';
		return;
	}
	theta = atan2f(sy, 1.0f) + t;
	value = sinf(theta * 6.0f) * cosf(phi * 4.0f) + sinf(theta * 12.0f + phi * 3.0f) * 0.4f + cosf(theta * 3.0f) * 0.7f;
	if (value > 0.8f) cell->c = '@', cell->colour = SPIN_SNOW;
	else if (value > 0.3f) cell->c = '#', cell->colour = SPIN_STONE;
	else if (value > -0.1f) cell->c = '*', cell->colour = SPIN_GREEN;
	else if (value > -0.6f) cell->c = '.', cell->colour = SPIN_CYAN;
	else cell->c = '~', cell->colour = SPIN_BLUE;
}

/* a half-size character at (x, y) of the corner: each pixel any of the 2 x 2 of the font's it stands for */
static void draw_small_cell(unsigned char *fb, int x, int y, struct cell const *cell)
{
	unsigned char const *glyph = font + cell->c * 16;
	unsigned long *p = (unsigned long *)fb + (TOP + SPIN_TOP * 16 + y * 8) * 640 + left + SPIN_LEFT * cell_width +
		x * 4;
	int row, column;
	for (row = 0; row < 8; row++, p += 640)
	{
		unsigned char bits = glyph[row * 2] | glyph[row * 2 + 1];
		for (column = 0; column < 4; column++)
			p[column] = bits & (0xC0 >> (column * 2)) ? cell->colour : cell->background;
	}
}

static DWORD WINAPI spinner_main(LPVOID parameter)
{
	static struct cell drawn[SPIN_HIGH][SPIN_WIDE];
	float t = 0.0f;
	(void)parameter;
	memset(drawn, 0, sizeof(drawn));
	while (spinner_wanted)
	{
		unsigned char *fb = XVideoGetFB();
		int x, y;
		for (y = 0; y < SPIN_HIGH; y++)
		{
			for (x = 0; x < SPIN_WIDE; x++)
			{
				struct cell cell;
				spinner_cell(&cell, x, y, t);
				if (cell.c != drawn[y][x].c || cell.colour != drawn[y][x].colour || !drawn[y][x].colour)
				{
					draw_small_cell(fb, x, y, &cell);
					drawn[y][x] = cell;
				}
			}
		}
		XVideoFlushFB();
		t -= 0.05f;
		Sleep(60);
	}
	return 0;
}

void screen_spinner(int on)
{
	if (on && !spinner_running)
	{
		spinner_wanted = 1;
		spinner_running = 1;
		spinner_thread = CreateThread(NULL, 0, spinner_main, NULL, 0, NULL);
		if (!spinner_thread)
			spinner_running = spinner_wanted = 0;
	}
	else if (!on && spinner_running)
	{
		int r, c;
		spinner_wanted = 0;
		WaitForSingleObject(spinner_thread, INFINITE);
		CloseHandle(spinner_thread);
		spinner_thread = NULL;
		spinner_running = 0;
		/* (the corner drawn again as the screen has it) */
		for (r = SPIN_TOP; r < SCREEN_ROWS; r++)
			for (c = SPIN_LEFT; c < SCREEN_COLUMNS; c++)
				memset(&shown[r][c], 0, sizeof(shown[r][c]));
		screen_show();
	}
}
