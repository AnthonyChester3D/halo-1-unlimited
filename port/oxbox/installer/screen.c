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
#define SPIN_ROWS SCREEN_SPINNER_ROWS
#define SPIN_COLUMNS SCREEN_SPINNER_COLUMNS
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

/* ---- the spinner: a Halo ring rising into the sky, turning, with clouds drifting over it, among stars that thin out
towards the corner's edges (Llama's design: ringpy.py), drawn in small characters (3 x 6 pixels: 53 x 32 of them in
the corner's 20 x 12 cells) by a thread of its own every 60 ms, so it moves while the installer is busy reading,
writing or downloading */

#define SPIN_GLYPH_WIDE 3
#define SPIN_GLYPH_HIGH 6
#define SPIN_WIDE (SPIN_COLUMNS * 8 / SPIN_GLYPH_WIDE)
#define SPIN_HIGH (SPIN_ROWS * 16 / SPIN_GLYPH_HIGH)
/* (ringpy.py's view of 100 x 60 characters, scaled to the corner's) */
#define SPIN_FOV_X (42.0f * SPIN_WIDE / 100.0f)
#define SPIN_FOV_Y (24.0f * SPIN_HIGH / 60.0f)
#define SPIN_BAND 0.25f                 /* the ring's half width over its radius (5 / 20) */
#define SPIN_STEP 0.02f                 /* its turn each frame (60 ms) */
#define SPIN_BLUE 0x2E5FD0
#define SPIN_CYAN 0x40C8D8
#define SPIN_GREEN 0x50E060
#define SPIN_STONE 0xA8A8A8
#define SPIN_SNOW 0xF4F4F4
#define SPIN_METAL 0x7A7A7A
#define SPIN_STAR 0x5A5A5A
#define SPIN_BRIGHT_STAR 0x8C8C8C

static float spinner_screen_x(int x)
{
	return ((float)x + 0.5f - (float)SPIN_WIDE / 2.0f) / (SPIN_FOV_X * 0.5f);
}

static float spinner_screen_y(int y)
{
	return ((float)SPIN_HIGH / 2.0f - (float)y - 0.5f) / (SPIN_FOV_Y * 0.5f);
}

/* where a small character's place is across the ring (phi), and whether it's on it */
static float spinner_phi(int x, int y)
{
	return spinner_screen_x(x) * (1.0f + spinner_screen_y(y) * 0.4f);
}

/* a steady random number for a place, 0 to 1 */
static float spinner_hash(int x, int y, int k)
{
	float v = sinf((float)x * 12.9898f + (float)y * 78.233f + (float)k * 37.719f) * 43758.5453f;
	return v - floorf(v);
}

/* the stars, placed once: thinning out from the middle of the corner towards its edges, unevenly, so they end in no
straight line */
static unsigned char spinner_stars[SPIN_HIGH][SPIN_WIDE];

static void spinner_place_stars(void)
{
	int x, y;
	for (y = 0; y < SPIN_HIGH; y++)
	{
		for (x = 0; x < SPIN_WIDE; x++)
		{
			float dx = ((float)x + 0.5f - SPIN_WIDE / 2.0f) / (SPIN_WIDE / 2.0f);
			float dy = ((float)y + 0.5f - SPIN_HIGH / 2.0f) / (SPIN_HIGH / 2.0f);
			float r = sqrtf(dx * dx + dy * dy) + (spinner_hash(x, y, 1) - 0.5f) * 0.35f;
			float fade = (r - 0.55f) / 0.5f, density, v = spinner_hash(x, y, 0);
			fade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
			density = 1.0f - fade * fade * (3.0f - 2.0f * fade);
			spinner_stars[y][x] = v > 1.0f - 0.05f * density ? '.' : v < 0.02f * density ? '+' : ' ';
		}
	}
}

static void spinner_cell(struct cell *cell, int x, int y, float t)
{
	float phi = spinner_phi(x, y), theta, value;
	cell->background = COLOUR_BLACK;
	cell->c = ' ';
	cell->colour = SPIN_METAL;
	if (fabsf(phi) >= SPIN_BAND)
	{
		cell->c = spinner_stars[y][x];
		cell->colour = cell->c == '+' ? SPIN_BRIGHT_STAR : SPIN_STAR;
		return;
	}
	/* (its rims: the ring's edge on each row) */
	if (fabsf(phi) > SPIN_BAND * 0.9f || x == 0 || x == SPIN_WIDE - 1 || fabsf(spinner_phi(x - 1, y)) >= SPIN_BAND ||
		fabsf(spinner_phi(x + 1, y)) >= SPIN_BAND)
	{
		cell->c = y >= SPIN_HIGH / 2 ? '|' : '^';
		return;
	}
	theta = atan2f(spinner_screen_y(y), 1.0f) + t;
	/* (clouds drifting over the ground, at their own speed) */
	if (sinf(theta * 8.0f + t * 0.4f) * cosf(phi * 5.0f) > 0.65f)
	{
		cell->c = '~', cell->colour = SPIN_SNOW;
		return;
	}
	value = sinf(theta * 6.0f) * cosf(phi * 4.0f) + sinf(theta * 12.0f + phi * 3.0f) * 0.4f + cosf(theta * 3.0f) * 0.7f;
	if (value > 0.8f) cell->c = '@', cell->colour = SPIN_SNOW;
	else if (value > 0.3f) cell->c = '#', cell->colour = SPIN_STONE;
	else if (value > -0.1f) cell->c = '*', cell->colour = SPIN_GREEN;
	else if (value > -0.6f) cell->c = '.', cell->colour = SPIN_CYAN;
	else cell->c = ' ', cell->colour = SPIN_BLUE;   /* (deep water) */
}

/* a small character at (x, y) of the corner: the font's character (its rows 2 to 13, where its strokes are) in the
top 5 of its 6 rows, each pixel lit if any of the font's pixels it stands for is, and the last row left empty, as a
line's spacing (without it the rows ran together into bars) */
static void draw_small_cell(unsigned char *fb, int x, int y, struct cell const *cell)
{
	unsigned char const *glyph = font + cell->c * 16;
	unsigned long *p = (unsigned long *)fb + (TOP + SPIN_TOP * 16 + y * SPIN_GLYPH_HIGH) * 640 + left +
		SPIN_LEFT * cell_width + x * SPIN_GLYPH_WIDE;
	int row, column, k;
	for (row = 0; row < SPIN_GLYPH_HIGH; row++, p += 640)
	{
		unsigned char bits = 0;
		for (k = 2 + row * 12 / (SPIN_GLYPH_HIGH - 1);
			row < SPIN_GLYPH_HIGH - 1 && k < 2 + ((row + 1) * 12 + SPIN_GLYPH_HIGH - 2) / (SPIN_GLYPH_HIGH - 1) && k < 16; k++)
			bits |= glyph[k];
		for (column = 0; column < SPIN_GLYPH_WIDE; column++)
		{
			int on = 0;
			for (k = column * 8 / SPIN_GLYPH_WIDE; k < ((column + 1) * 8 + SPIN_GLYPH_WIDE - 1) / SPIN_GLYPH_WIDE && k < 8; k++)
				on |= bits & (0x80 >> k);
			p[column] = on ? cell->colour : cell->background;
		}
	}
}

static DWORD WINAPI spinner_main(LPVOID parameter)
{
	static struct cell drawn[SPIN_HIGH][SPIN_WIDE];
	static int stars_placed;
	static float t;
	(void)parameter;
	if (!stars_placed)
	{
		spinner_place_stars();
		stars_placed = 1;
	}
	memset(drawn, 0, sizeof(drawn));
	/* (the whole corner cleared first: its characters leave a pixel or two at its edges, which kept what was there) */
	{
		unsigned long *p = (unsigned long *)XVideoGetFB() + (TOP + SPIN_TOP * 16) * 640 + left + SPIN_LEFT * cell_width;
		int x, y;
		for (y = 0; y < SPIN_ROWS * 16; y++, p += 640)
			for (x = 0; x < SPIN_COLUMNS * cell_width; x++)
				p[x] = COLOUR_BLACK;
	}
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
		t -= SPIN_STEP;
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
