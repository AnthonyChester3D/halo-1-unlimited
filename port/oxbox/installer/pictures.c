/* pictures.c: OpenCE's own menu pictures, made from the art in the player's own ui.map (pictures.h). Built for the
Xbox (the installer) and for the PC (mkpictures), with the same results: the floating point is single precision and
done in the order written (-ffp-contract=off, and SSE rather than the x87 on the Xbox). This was tools/mkword.py,
mkicons.py and mkheader.py (Python, with numpy and Pillow); the blur and the resize are Pillow's, done the same way. */
#include "pictures.h"
#include "text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

pictures_progress_t pictures_progress;

static void progress(char const *step)
{
	if (pictures_progress)
		pictures_progress(step);
}

/* ---- ui.map ---- */

#define MAP_HEADER 2048
#define MAP_BASE 0x803A6000u

static unsigned long u32(unsigned char const *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int s16(unsigned char const *p)
{
	return (short)(p[0] | (p[1] << 8));
}

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

/* the bytes of a decompressed map wanted: [offset, offset + size) of the whole file (its header included) */
struct map_range
{
	unsigned long offset, size;
	unsigned char *data;
};

/* reads the map once, copying each range wanted; 0 when done */
static int map_read(char const *path, struct map_range *ranges, int count, unsigned long *file_size, char *error)
{
	FILE *file = fopen(path, "rb");
	unsigned char *in = NULL, *out = NULL;
	unsigned char header[MAP_HEADER];
	z_stream z;
	unsigned long at = MAP_HEADER;
	int status = Z_OK, result = 1, i;

	if (!file)
	{
		text_print(error, 256, "can't open %s", path);
		return 1;
	}
	memset(&z, 0, sizeof(z));
	z.zalloc = z_alloc;
	z.zfree = z_free;
	in = malloc(65536);
	out = malloc(65536);
	if (!in || !out || inflateInit(&z) != Z_OK)
	{
		text_print(error, 256, "out of memory");
		goto done;
	}
	if (fread(header, 1, MAP_HEADER, file) != MAP_HEADER || u32(header) != 0x68656164 || u32(header + 4) != 5)
	{
		text_print(error, 256, "%s isn't an Xbox Halo map", path);
		goto done;
	}
	*file_size = u32(header + 8);
	for (i = 0; i < count; i++)
	{
		struct map_range *range = &ranges[i];
		unsigned long from = range->offset, to = range->offset + range->size;
		if (from < MAP_HEADER)
		{
			unsigned long end = to < MAP_HEADER ? to : MAP_HEADER;
			memcpy(range->data, header + from, end - from);
		}
	}
	while (status != Z_STREAM_END)
	{
		if (z.avail_in == 0)
		{
			z.avail_in = (unsigned)fread(in, 1, 65536, file);
			z.next_in = in;
			if (z.avail_in == 0)
				break;
		}
		z.next_out = out;
		z.avail_out = 65536;
		status = inflate(&z, Z_NO_FLUSH);
		if (status != Z_OK && status != Z_STREAM_END)
			break;
		{
			unsigned long got = 65536 - z.avail_out, end = at + got;
			for (i = 0; i < count; i++)
			{
				struct map_range *range = &ranges[i];
				unsigned long from = range->offset > at ? range->offset : at;
				unsigned long to = range->offset + range->size < end ? range->offset + range->size : end;
				if (from < to)
					memcpy(range->data + (from - range->offset), out + (from - at), to - from);
			}
			at = end;
		}
	}
	if (status != Z_STREAM_END || at != *file_size)
	{
		text_print(error, 256, "%s didn't decompress (%lu of %lu bytes)", path, at, *file_size);
		goto done;
	}
	for (i = 0; i < count; i++)
	{
		if (ranges[i].offset + ranges[i].size > at)
		{
			text_print(error, 256, "%s is shorter than its tags say", path);
			goto done;
		}
	}
	result = 0;
done:
	inflateEnd(&z);
	free(in);
	free(out);
	fclose(file);
	return result;
}

/* a bitmap tag's bitmap wanted (where it is in the map, found from its tag) */
struct wanted
{
	char const *tag;
	int index, width, height;
	struct map_range range;
};
#define WANT(tag, index, width, height) {tag, index, width, height, {0, 0, NULL}}

/* the art: each a DXT3 bitmap of the size given */
enum
{
	SET, SET_H, CAMP, CAMP_H, DEMO, DEMO_H, MP, MP_H,
	RULES_0, RULES_1, RULES_2, RULES_3, RULES_4,
	FOUND, PROFILE,
	WANTED
};

#define MAIN_MENU "ui\\shell\\main_menu\\"
static struct wanted wanted[WANTED] = {
	WANT(MAIN_MENU "menu_settings", 0, 256, 64), WANT(MAIN_MENU "menu_settings", 1, 256, 64),
	WANT(MAIN_MENU "menu_load_campaign", 0, 256, 64), WANT(MAIN_MENU "menu_load_campaign", 1, 256, 64),
	WANT(MAIN_MENU "menu_game_demos", 0, 256, 64), WANT(MAIN_MENU "menu_game_demos", 1, 256, 64),
	WANT(MAIN_MENU "menu_multiplayer", 0, 256, 64), WANT(MAIN_MENU "menu_multiplayer", 1, 256, 64),
#define RULES MAIN_MENU "settings_select\\multiplayer_setup\\playlist_edit\\game_type_select\\rules_options"
	WANT(RULES, 0, 256, 256), WANT(RULES, 1, 256, 256), WANT(RULES, 2, 256, 256), WANT(RULES, 3, 256, 256),
	WANT(RULES, 4, 256, 256),
	WANT(MAIN_MENU "multiplayer_type_select\\connected\\server_list\\header_found_games", 0, 512, 64),
	WANT(MAIN_MENU "multiplayer_type_select\\connected\\4way_profile_select\\header_select_profile", 0, 512, 64),
};

/* the tags' data (a copy of the map's tag data), and where each bitmap wanted is */
static int map_find(unsigned char const *tags, unsigned long tags_offset, unsigned long tags_size, char *error)
{
	unsigned long tag_array, count, i;
	int w;

#define AT(pointer) ((pointer) - MAP_BASE)
#define IN(pointer, size) ((pointer) >= MAP_BASE && AT(pointer) <= tags_size && (size) <= tags_size - AT(pointer))
	(void)tags_offset;
	if (tags_size < 16)
		goto bad;
	tag_array = u32(tags);
	count = u32(tags + 12);
	if (count > 65536 || !IN(tag_array, count * 32))
		goto bad;
	for (w = 0; w < WANTED; w++)
		wanted[w].range.size = 0;
	for (i = 0; i < count; i++)
	{
		unsigned char const *entry = tags + AT(tag_array) + i * 32;
		unsigned long name, data, n_bitmaps, bitmaps;
		size_t length;
		if (u32(entry) != 0x6269746D) /* 'bitm' */
			continue;
		name = u32(entry + 16);
		data = u32(entry + 20);
		if (!IN(name, 1))
			goto bad;
		for (length = 0; AT(name) + length < tags_size && tags[AT(name) + length]; length++)
			;
		for (w = 0; w < WANTED; w++)
		{
			struct wanted *want = &wanted[w];
			unsigned char const *bitmap;
			if (length != strlen(want->tag) || memcmp(tags + AT(name), want->tag, length))
				continue;
			if (!IN(data, 0x68))
				goto bad;
			n_bitmaps = u32(tags + AT(data) + 0x60);
			bitmaps = u32(tags + AT(data) + 0x64);
			if ((unsigned long)want->index >= n_bitmaps || n_bitmaps > 1024 || !IN(bitmaps, n_bitmaps * 48))
				goto bad;
			bitmap = tags + AT(bitmaps) + want->index * 48;
			/* (format 15: DXT3) */
			if (s16(bitmap + 4) != want->width || s16(bitmap + 6) != want->height || s16(bitmap + 0xc) != 15 ||
				u32(bitmap + 0x1c) != (unsigned long)want->width * want->height)
			{
				text_print(error, 256, "ui.map's %s isn't the picture expected", want->tag);
				return 1;
			}
			want->range.offset = u32(bitmap + 0x18);
			want->range.size = u32(bitmap + 0x1c);
		}
	}
	for (w = 0; w < WANTED; w++)
	{
		if (!wanted[w].range.size)
		{
			text_print(error, 256, "ui.map has no %s", wanted[w].tag);
			return 1;
		}
	}
	return 0;
bad:
	text_print(error, 256, "ui.map's tags don't read");
	return 1;
#undef AT
#undef IN
}

/* ---- DXT3 ---- */

static int expand5(int v) { return v * 255 / 31; }
static int expand6(int v) { return v * 255 / 63; }

/* RGBA bytes from DXT3 */
static void dxt3_decode(unsigned char const *data, int width, int height, unsigned char *rgba)
{
	int bx, by, px, py, k;
	for (by = 0; by < height; by += 4)
	{
		for (bx = 0; bx < width; bx += 4, data += 16)
		{
			unsigned c0 = data[8] | (data[9] << 8), c1 = data[10] | (data[11] << 8);
			unsigned long bits = u32(data + 12);
			int p[4][3];
			p[0][0] = expand5(c0 >> 11), p[0][1] = expand6((c0 >> 5) & 63), p[0][2] = expand5(c0 & 31);
			p[1][0] = expand5(c1 >> 11), p[1][1] = expand6((c1 >> 5) & 63), p[1][2] = expand5(c1 & 31);
			for (k = 0; k < 3; k++)
			{
				p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
				p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
			}
			for (py = 0; py < 4; py++)
			{
				for (px = 0; px < 4; px++)
				{
					int i = py * 4 + px;
					int alpha = (data[i / 2] >> ((i & 1) * 4)) & 15;
					int const *c = p[(bits >> (2 * i)) & 3];
					unsigned char *o = rgba + ((by + py) * width + bx + px) * 4;
					o[0] = (unsigned char)c[0], o[1] = (unsigned char)c[1], o[2] = (unsigned char)c[2];
					o[3] = (unsigned char)(alpha * 17);
				}
			}
		}
	}
}

static unsigned encode565(int const *c)
{
	return ((c[0] * 31 + 127) / 255 << 11) | ((c[1] * 63 + 127) / 255 << 5) | ((c[2] * 31 + 127) / 255);
}

static void decode565(unsigned v, int *c)
{
	c[0] = expand5(v >> 11), c[1] = expand6((v >> 5) & 63), c[2] = expand5(v & 31);
}

/* DXT3 from RGBA bytes: each block's colours between its darkest and brightest pixels (of those not transparent) */
static void dxt3_encode(unsigned char const *rgba, int width, int height, unsigned char *data)
{
	int bx, by, k, j;
	for (by = 0; by < height; by += 4)
	{
		for (bx = 0; bx < width; bx += 4, data += 16)
		{
			int px[16][4], lo = -1, hi = -1, any = 0;
			unsigned c0, c1;
			unsigned long bits = 0;
			double lum_lo = 0, lum_hi = 0;
			for (k = 0; k < 16; k++)
			{
				unsigned char const *p = rgba + ((by + k / 4) * width + bx + k % 4) * 4;
				for (j = 0; j < 4; j++)
					px[k][j] = p[j];
				any |= px[k][3];
			}
			memset(data, 0, 8);
			for (k = 0; k < 16; k++)
				data[k / 2] |= ((px[k][3] * 2 + 17) / 34) << ((k & 1) * 4);
			for (k = 0; k < 16; k++)
			{
				double lum;
				if (any && !px[k][3])
					continue;
				lum = px[k][0] * .3 + px[k][1] * .59 + px[k][2] * .11;
				if (lo < 0 || lum < lum_lo)
					lo = k, lum_lo = lum;
				if (hi < 0 || lum > lum_hi)
					hi = k, lum_hi = lum;
			}
			c0 = encode565(px[hi]);
			c1 = encode565(px[lo]);
			if (c0 < c1)
			{
				unsigned t = c0;
				c0 = c1, c1 = t;
			}
			if (c0 != c1)
			{
				/* (the distances to the palette's colours times 3, so they're whole) */
				int p0[3], p1[3], pal[4][3];
				decode565(c0, p0);
				decode565(c1, p1);
				for (j = 0; j < 3; j++)
				{
					pal[0][j] = 3 * p0[j];
					pal[1][j] = 3 * p1[j];
					pal[2][j] = 2 * p0[j] + p1[j];
					pal[3][j] = p0[j] + 2 * p1[j];
				}
				for (k = 0; k < 16; k++)
				{
					long best = -1;
					int pick = 0, n;
					for (n = 0; n < 4; n++)
					{
						long d = 0;
						for (j = 0; j < 3; j++)
						{
							long e = 3 * px[k][j] - pal[n][j];
							d += e * e;
						}
						if (best < 0 || d < best)
							best = d, pick = n;
					}
					bits |= (unsigned long)pick << (2 * k);
				}
			}
			data[8] = (unsigned char)c0, data[9] = (unsigned char)(c0 >> 8);
			data[10] = (unsigned char)c1, data[11] = (unsigned char)(c1 >> 8);
			data[12] = (unsigned char)bits, data[13] = (unsigned char)(bits >> 8);
			data[14] = (unsigned char)(bits >> 16), data[15] = (unsigned char)(bits >> 24);
		}
	}
}

/* ---- pictures made of letters (RGBA bytes, rows of `width`) ---- */

/* copies column `from` of source (height 64) over column `to` of out: a pixel where it's more opaque */
static void letter_column(unsigned char *out, int out_width, int to, unsigned char const *source, int source_width,
	int from)
{
	int y;
	for (y = 0; y < 64; y++)
	{
		unsigned char const *s = source + (y * source_width + from) * 4;
		unsigned char *o = out + (y * out_width + to) * 4;
		if (s[3] > o[3])
			memcpy(o, s, 4);
	}
}

/* the main menu's words: letters of CAMPAIGN, MULTIPLAYER, SETTINGS and GAME DEMOS (normal and highlighted,
256 x 64). F is SETTINGS' E without its lower bar */
enum { W_SET, W_CAMP, W_DEMO, W_MP };
static int const word_cores[4][8][2] = {
	{{60, 78}, {80, 96}, {97, 113}, {115, 131}, {133, 137}, {141, 158}, {161, 179}, {181, 199}},
	{{53, 69}, {70, 89}, {91, 113}, {117, 134}, {135, 155}, {157, 161}, {164, 181}, {185, 202}},
	{{126, 143}, {146, 161}, {165, 186}, {189, 209}, {210, 228}},
	{{34, 55}, {60, 75}, {80, 92}, {94, 109}, {112, 115}, {121, 136}, {210, 226}},
};
static int const word_core_count[4] = {8, 8, 5, 7};
struct word_letter
{
	int word, index, f;
};
static struct word_letter const word_options[] = {
	{W_DEMO, 3, 0}, {W_CAMP, 3, 0}, {W_SET, 2, 0}, {W_SET, 4, 0}, {W_DEMO, 3, 0}, {W_SET, 5, 0}, {W_SET, 7, 0},
	{-1, 0, 0}};
static struct word_letter const word_profiles[] = {
	{W_MP, 5, 0}, {W_MP, 6, 0}, {W_DEMO, 3, 0}, {W_SET, 1, 1}, {W_SET, 4, 0}, {W_MP, 2, 0}, {W_SET, 1, 0},
	{W_SET, 7, 0}, {-1, 0, 0}};
#define WORD_GAP 3

/* (letters whose neighbours in their word aren't all in the cores: MULTIPLAYER's L, A and Y run together) */
static int word_bounds(int word, int index, int *left, int *right)
{
	static int const bounds[3][4] = {{W_MP, 2, 78, 93}, {W_MP, 5, 118, 138}, {W_MP, 6, 208, 229}};
	int i;
	for (i = 0; i < 3; i++)
	{
		if (bounds[i][0] == word && bounds[i][1] == index)
		{
			*left = bounds[i][2], *right = bounds[i][3];
			return 1;
		}
	}
	return 0;
}

/* a letter's column, F made from E: the stem goes straight down from row 20; nothing of the lower bar */
static void letter_column_f(unsigned char *column, unsigned char const *source, int x, int f)
{
	int y;
	for (y = 0; y < 64; y++)
	{
		int from = f && y >= 21 && y <= 25 ? 20 : y;
		if (f && y == 26)
			memset(column + y * 4, 0, 4);
		else
			memcpy(column + y * 4, source + (from * 256 + x) * 4, 4);
	}
}

static int word_width(struct word_letter const *letters)
{
	int total = 0, n = 0;
	for (; letters->word >= 0; letters++, n++)
	{
		int const *core = word_cores[letters->word][letters->index];
		total += core[1] - core[0] + 1;
	}
	return total + WORD_GAP * (n - 1);
}

static void word_normal(struct word_letter const *letters, unsigned char *const *sources, unsigned char *out)
{
	int x = (256 - word_width(letters)) / 2;
	unsigned char column[64 * 4];
	memset(out, 0, 256 * 64 * 4);
	for (; letters->word >= 0; letters++)
	{
		int const (*cores)[2] = word_cores[letters->word];
		int i = letters->index, count = word_core_count[letters->word];
		int x0 = cores[i][0], x1 = cores[i][1], left, right, cx;
		if (!word_bounds(letters->word, i, &left, &right))
		{
			left = i > 0 ? (cores[i - 1][1] + x0) / 2 + 1 : x0 - 8;
			right = i + 1 < count ? (x1 + cores[i + 1][0]) / 2 : x1 + 8;
		}
		if (left < 0)
			left = 0;
		if (right > 255)
			right = 255;
		for (cx = left; cx <= right; cx++)
		{
			int to = x - (x0 - left) + (cx - left);
			if (to < 0 || to >= 256)
				continue;
			letter_column_f(column, sources[letters->word], cx, letters->f);
			letter_column(out, 256, to, column, 1, 0);
		}
		x += x1 - x0 + 1 + WORD_GAP;
	}
}

/* ---- Pillow's Gaussian blur: 3 box blurs each way, the box's ends weighted (ImagingGaussianBlur) ---- */

static void box_blur_line(unsigned char *out, unsigned char const *in, int lastx, int radius, int edge_a,
	int edge_b, unsigned long ww, unsigned long fw)
{
	unsigned long acc, bulk;
	int x;
#define BLUR_MOVE(subtract, add) acc += in[add] - in[subtract]
#define BLUR_FAR(left, right) bulk = acc * ww + (in[left] + in[right]) * fw
#define BLUR_SAVE(x) out[x] = (unsigned char)((bulk + (1 << 23)) >> 24)
	acc = in[0] * (radius + 1);
	for (x = 0; x < edge_a - 1; x++)
		acc += in[x];
	acc += in[lastx] * (radius - edge_a + 1);
	if (edge_a <= edge_b)
	{
		for (x = 0; x < edge_a; x++)
		{
			BLUR_MOVE(0, x + radius);
			BLUR_FAR(0, x + radius + 1);
			BLUR_SAVE(x);
		}
		for (x = edge_a; x < edge_b; x++)
		{
			BLUR_MOVE(x - radius - 1, x + radius);
			BLUR_FAR(x - radius - 1, x + radius + 1);
			BLUR_SAVE(x);
		}
		for (x = edge_b; x <= lastx; x++)
		{
			BLUR_MOVE(x - radius - 1, lastx);
			BLUR_FAR(x - radius - 1, lastx);
			BLUR_SAVE(x);
		}
	}
	else
	{
		for (x = 0; x < edge_b; x++)
		{
			BLUR_MOVE(0, x + radius);
			BLUR_FAR(0, x + radius + 1);
			BLUR_SAVE(x);
		}
		for (x = edge_b; x < edge_a; x++)
		{
			BLUR_MOVE(0, lastx);
			BLUR_FAR(0, lastx);
			BLUR_SAVE(x);
		}
		for (x = edge_a; x <= lastx; x++)
		{
			BLUR_MOVE(x - radius - 1, lastx);
			BLUR_FAR(x - radius - 1, lastx);
			BLUR_SAVE(x);
		}
	}
#undef BLUR_MOVE
#undef BLUR_FAR
#undef BLUR_SAVE
}

/* blurs each row of a (w x h, one byte a pixel) in place, `passes` times */
static void box_blur_rows(unsigned char *image, int width, int height, float float_radius, int passes)
{
	int radius = (int)float_radius, y, pass;
	unsigned long ww = (unsigned long)((float)(1 << 24) / (float_radius * 2 + 1));
	unsigned long fw = ((1ul << 24) - (radius * 2 + 1) * ww) / 2;
	int edge_a = radius + 1 < width ? radius + 1 : width;
	int edge_b = width - radius - 1 > 0 ? width - radius - 1 : 0;
	unsigned char line[512];
	for (pass = 0; pass < passes; pass++)
	{
		for (y = 0; y < height; y++)
		{
			box_blur_line(line, image + y * width, width - 1, radius, edge_a, edge_b, ww, fw);
			memcpy(image + y * width, line, width);
		}
	}
}

static float gaussian_box_radius(float radius, int passes)
{
	float sigma2, big_l, l, a;
	sigma2 = radius * radius / passes;
	big_l = (float)sqrt(12.0 * sigma2 + 1.0);
	l = (float)floor((big_l - 1.0) / 2.0);
	a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);
	a /= 6 * (sigma2 - (l + 1) * (l + 1));
	return l + a;
}

static void gaussian_blur(unsigned char *image, int width, int height, float radius)
{
	float box = gaussian_box_radius(radius, 3);
	unsigned char *t = malloc((size_t)width * height);
	int x, y;
	box_blur_rows(image, width, height, box, 3);
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			t[x * height + y] = image[y * width + x];
	box_blur_rows(t, height, width, box, 3);
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			image[y * width + x] = t[x * height + y];
	free(t);
}

/* the highlighted words: the letters' white faces with a blue glow, from the highlighted originals */
static void word_highlighted(struct word_letter const *letters, unsigned char *const *sources, unsigned char *out)
{
	static float cover[64 * 256];
	static unsigned char blurred[64 * 256];
	static float const blue[3] = {38, 149, 255};
	int x = (256 - word_width(letters)) / 2, i;
	unsigned char column[64 * 4];
	memset(cover, 0, sizeof(cover));
	for (; letters->word >= 0; letters++)
	{
		int const *core = word_cores[letters->word][letters->index];
		int cx, y;
		for (cx = core[0]; cx <= core[1]; cx++)
		{
			int to = x + cx - core[0];
			letter_column_f(column, sources[letters->word], cx, letters->f);
			if (to < 0 || to >= 256)
				continue;
			for (y = 0; y < 64; y++)
			{
				float c = ((float)column[y * 4] - 41.0f) / 159.0f;
				c = c < 0 ? 0 : c > 1 ? 1 : c;
				if (c > cover[y * 256 + to])
					cover[y * 256 + to] = c;
			}
		}
		x += core[1] - core[0] + 1 + WORD_GAP;
	}
	for (i = 0; i < 64 * 256; i++)
		blurred[i] = (unsigned char)(int)(cover[i] * 255.0f);
	gaussian_blur(blurred, 256, 64, 2.0f);
	for (i = 0; i < 64 * 256; i++)
	{
		float c = cover[i], g = (float)blurred[i] / 255.0f, glow = g * 2.4f, alpha;
		float const top = (float)(153.0 / 255.0);
		int k;
		glow = glow < 0 ? 0 : glow > top ? top : glow;
		for (k = 0; k < 3; k++)
		{
			float v = blue[k] * (1.0f - c) + 255.0f * c;
			v = v < 0 ? 0 : v > 255 ? 255 : v;
			out[i * 4 + k] = (unsigned char)(int)v;
		}
		alpha = (glow > c ? glow : c) * 255.0f;
		if (alpha < 8)
			alpha = 0;
		out[i * 4 + 3] = (unsigned char)(int)(alpha > 255 ? 255 : alpha);
	}
}

/* ---- the gametype pictures: Pillow's Lanczos resize (ImagingResample, 8 bits a channel) ---- */

#define PRECISION_BITS (32 - 8 - 2)

static double sinc(double x)
{
	if (x == 0.0)
		return 1.0;
	x = x * M_PI;
	return sin(x) / x;
}

static double lanczos(double x)
{
	if (-3.0 <= x && x < 3.0)
		return sinc(x) * sinc(x / 3);
	return 0.0;
}

/* each output pixel's first input pixel, count and weights (fixed point) */
static int resample_coefficients(int in_size, int out_size, int *bounds, long *weights, int max_k)
{
	double scale = (double)in_size / out_size, filterscale = scale < 1.0 ? 1.0 : scale;
	double support = 3.0 * filterscale;
	int ksize = (int)ceil(support) * 2 + 1, xx, x;
	double k[64];
	if (ksize > max_k || ksize > 64)
		return 0;
	for (xx = 0; xx < out_size; xx++)
	{
		double center = (xx + 0.5) * scale, ww = 0.0, ss = 1.0 / filterscale;
		int xmin = (int)(center - support + 0.5), xmax = (int)(center + support + 0.5);
		if (xmin < 0)
			xmin = 0;
		if (xmax > in_size)
			xmax = in_size;
		xmax -= xmin;
		for (x = 0; x < xmax; x++)
		{
			double w = lanczos((x + xmin - center + 0.5) * ss);
			k[x] = w;
			ww += w;
		}
		for (x = 0; x < xmax; x++)
		{
			if (ww != 0.0)
				k[x] /= ww;
		}
		for (x = 0; x < ksize; x++)
		{
			double v = x < xmax ? k[x] : 0;
			weights[xx * ksize + x] = v < 0 ? (long)(-0.5 + v * (1 << PRECISION_BITS)) :
				(long)(0.5 + v * (1 << PRECISION_BITS));
		}
		bounds[xx * 2] = xmin;
		bounds[xx * 2 + 1] = xmax;
	}
	return ksize;
}

static unsigned char clip8(long v)
{
	v >>= PRECISION_BITS;
	return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* RGBA (premultiplied) side x side down to 64 x 64 */
static int resize_64(unsigned char const *in, int side, unsigned char *out)
{
	static int bounds[64 * 2];
	static long weights[64 * 64];
	int ksize = resample_coefficients(side, 64, bounds, weights, 64);
	int first, last, x, y, c, i;
	unsigned char *temp;
	if (!ksize)
		return 1;
	first = bounds[0];
	last = bounds[63 * 2] + bounds[63 * 2 + 1];
	temp = malloc((size_t)64 * (last - first) * 4);
	if (!temp)
		return 1;
	/* across, the rows the vertical pass uses */
	for (y = first; y < last; y++)
	{
		for (x = 0; x < 64; x++)
		{
			long const *k = weights + x * ksize;
			for (c = 0; c < 4; c++)
			{
				long ss = 1 << (PRECISION_BITS - 1);
				for (i = 0; i < bounds[x * 2 + 1]; i++)
					ss += in[(y * side + bounds[x * 2] + i) * 4 + c] * k[i];
				temp[((y - first) * 64 + x) * 4 + c] = clip8(ss);
			}
		}
	}
	/* down (the same coefficients: the picture is square) */
	for (y = 0; y < 64; y++)
	{
		long const *k = weights + y * ksize;
		int ymin = bounds[y * 2] - first;
		for (x = 0; x < 64; x++)
		{
			for (c = 0; c < 4; c++)
			{
				long ss = 1 << (PRECISION_BITS - 1);
				for (i = 0; i < bounds[y * 2 + 1]; i++)
					ss += temp[((ymin + i) * 64 + x) * 4 + c] * k[i];
				out[(y * 64 + x) * 4 + c] = clip8(ss);
			}
		}
	}
	free(temp);
	return 0;
}

static unsigned morton(int x, int y)
{
	unsigned r = 0;
	int bit;
	for (bit = 0; bit < 6; bit++)
		r |= ((x >> bit) & 1u) << (2 * bit) | ((y >> bit) & 1u) << (2 * bit + 1);
	return r;
}

/* a gametype picture: the editor's picture alone (no background), cropped square, 64 x 64 A8R8G8B8 swizzled */
static int gametype_picture(unsigned char const *rgba, unsigned char *out)
{
	int x0 = 256, x1 = -1, y0 = 256, y1 = -1, x, y, k, side, cx, cy, bx, by;
	unsigned char *crop, small[64 * 64 * 4];
	for (y = 0; y < 256; y++)
	{
		for (x = 0; x < 256; x++)
		{
			if (rgba[(y * 256 + x) * 4 + 3] > 6)
			{
				x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
				y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
			}
		}
	}
	if (x1 < 0)
		return 1;
	side = (x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0) + 8;
	cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
	bx = cx - side / 2, by = cy - side / 2;
	crop = calloc((size_t)side * side, 4);
	if (!crop)
		return 1;
	/* premultiplied (so the glow's edge doesn't go dark), then again as Pillow's resize does it */
	for (y = 0; y < side; y++)
	{
		for (x = 0; x < side; x++)
		{
			unsigned char *o = crop + (y * side + x) * 4;
			unsigned char const *s;
			float f;
			if (x + bx < 0 || x + bx >= 256 || y + by < 0 || y + by >= 256)
				continue;
			s = rgba + ((y + by) * 256 + x + bx) * 4;
			f = (float)s[3] / 255.0f;
			for (k = 0; k < 3; k++)
			{
				unsigned v = (unsigned char)(int)((float)s[k] * f), t = v * s[3] + 128;
				o[k] = (unsigned char)(((t >> 8) + t) >> 8);
			}
			o[3] = s[3];
		}
	}
	k = resize_64(crop, side, small);
	free(crop);
	if (k)
		return 1;
	for (y = 0; y < 64; y++)
	{
		for (x = 0; x < 64; x++)
		{
			unsigned char *s = small + (y * 64 + x) * 4, *o = out + morton(x, y) * 4;
			int alpha = s[3], c[3];
			for (k = 0; k < 3; k++)
			{
				/* (Pillow's back from premultiplied, then the picture's own) */
				int v = s[k];
				float f;
				if (alpha != 255 && alpha != 0)
				{
					v = 255 * v / alpha;
					v = v > 255 ? 255 : v;
				}
				f = alpha > 0 ? (float)v * 255.0f / (float)(alpha > 1 ? alpha : 1) : 0.0f;
				f = f < 0 ? 0 : f > 255 ? 255 : f;
				c[k] = (int)f;
			}
			o[0] = (unsigned char)c[2], o[1] = (unsigned char)c[1], o[2] = (unsigned char)c[0];
			o[3] = (unsigned char)alpha;
		}
	}
	return 0;
}

/* ---- the ONLINE GAMES header: letters of SYSTEM LINK GAMES and SELECT PROFILE (512 x 64) ---- */

static int const found_cores[][2] = {{30, 49}, {51, 90}, {93, 110}, {113, 129}, {134, 157}, {174, 187}, {191, 194},
	{200, 218}, {224, 241}, {254, 273}, {275, 296}, {299, 323}, {327, 343}, {347, 366}};
static int const profile_cores[][2] = {{30, 49}, {53, 69}, {74, 87}, {90, 106}, {110, 127}, {129, 146}, {161, 179},
	{183, 201}, {204, 224}, {229, 245}, {250, 253}, {258, 271}, {274, 291}};
/* ONLINE GAMES: each piece (a letter, or GAMES whole: its columns), from which header, and the space before it */
struct header_piece
{
	int profile, x0, x1, space;
};
static struct header_piece const header_pieces[] = {
	{1, 204, 224, 0}, /* O */
	{0, 200, 218, 4}, /* N */
	{0, 174, 187, 5}, /* L */
	{0, 191, 194, 3}, /* I */
	{0, 200, 218, 5}, /* N */
	{0, 113, 129, 4}, /* E */
	{0, 254, 366, 13}, /* GAMES */
};
#define HEADER_LEFT 30

static void header_compose(unsigned char const *found, unsigned char const *profile, unsigned char *out)
{
	int x = HEADER_LEFT, p;
	memset(out, 0, 512 * 64 * 4);
	for (p = 0; p < (int)(sizeof(header_pieces) / sizeof(*header_pieces)); p++)
	{
		struct header_piece const *piece = &header_pieces[p];
		int const (*cores)[2] = piece->profile ? profile_cores : found_cores;
		int count = piece->profile ? (int)(sizeof(profile_cores) / sizeof(*profile_cores)) :
			(int)(sizeof(found_cores) / sizeof(*found_cores));
		unsigned char const *image = piece->profile ? profile : found;
		int left = piece->x0 - 8, right = piece->x1 + 8, i, cx;
		/* (its glow: to halfway to the neighbours' cores, 8 at the ends) */
		for (i = 0; i < count; i++)
		{
			if (cores[i][1] < piece->x0)
				left = (cores[i][1] + piece->x0) / 2 + 1;
		}
		for (i = count - 1; i >= 0; i--)
		{
			if (cores[i][0] > piece->x1)
				right = (piece->x1 + cores[i][0]) / 2;
		}
		x += piece->space;
		for (cx = left; cx <= right; cx++)
		{
			int to = x - (piece->x0 - left) + (cx - left);
			if (to >= 0 && to < 512 && cx >= 0 && cx < 512)
				letter_column(out, 512, to, image, 512, cx);
		}
		x += piece->x1 - piece->x0 + 1;
	}
}

/* ---- all of it ---- */

int pictures_make(char const *ui_map_path, unsigned char *pictures, unsigned char *header, char *error)
{
	struct map_range tags_range;
	unsigned char head[MAP_HEADER], *tags = NULL, *art = NULL, *rgba[WANTED], *picture = NULL;
	struct map_range ranges[WANTED];
	unsigned long file_size, total = 0;
	int w, result = 1;

	memset(rgba, 0, sizeof(rgba));
	/* the header alone says where the tags are: read it, then the tags (the first time through) */
	{
		FILE *file = fopen(ui_map_path, "rb");
		if (!file)
		{
			text_print(error, 256, "can't open %s", ui_map_path);
			return 1;
		}
		w = fread(head, 1, MAP_HEADER, file) == MAP_HEADER;
		fclose(file);
		if (!w || u32(head) != 0x68656164 || u32(head + 4) != 5)
		{
			text_print(error, 256, "%s isn't an Xbox Halo map", ui_map_path);
			return 1;
		}
	}
	tags_range.offset = u32(head + 16);
	tags_range.size = u32(head + 20);
	if (tags_range.size > 16 * 1024 * 1024 || tags_range.offset < MAP_HEADER || tags_range.offset > u32(head + 8))
	{
		text_print(error, 256, "%s's header doesn't read", ui_map_path);
		return 1;
	}
	tags = malloc(tags_range.size);
	tags_range.data = tags;
	progress("reading ui.map's tags");
	if (!tags || map_read(ui_map_path, &tags_range, 1, &file_size, error))
	{
		if (!tags)
			text_print(error, 256, "out of memory");
		goto done;
	}
	if (map_find(tags, tags_range.offset, tags_range.size, error))
		goto done;
	/* the second time through, the art */
	for (w = 0; w < WANTED; w++)
		total += wanted[w].range.size;
	art = malloc(total);
	if (!art)
	{
		text_print(error, 256, "out of memory");
		goto done;
	}
	total = 0;
	for (w = 0; w < WANTED; w++)
	{
		ranges[w] = wanted[w].range;
		ranges[w].data = art + total;
		total += ranges[w].size;
	}
	progress("reading ui.map's pictures");
	if (map_read(ui_map_path, ranges, WANTED, &file_size, error))
		goto done;
	for (w = 0; w < WANTED; w++)
	{
		rgba[w] = malloc((size_t)wanted[w].width * wanted[w].height * 4);
		if (!rgba[w])
		{
			text_print(error, 256, "out of memory");
			goto done;
		}
		dxt3_decode(ranges[w].data, wanted[w].width, wanted[w].height, rgba[w]);
	}
	picture = malloc(512 * 64 * 4);
	if (!picture)
	{
		text_print(error, 256, "out of memory");
		goto done;
	}
	progress("making the menu's words");
	{
		unsigned char *normal[4], *highlighted[4];
		normal[W_SET] = rgba[SET], normal[W_CAMP] = rgba[CAMP], normal[W_DEMO] = rgba[DEMO], normal[W_MP] = rgba[MP];
		highlighted[W_SET] = rgba[SET_H], highlighted[W_CAMP] = rgba[CAMP_H];
		highlighted[W_DEMO] = rgba[DEMO_H], highlighted[W_MP] = rgba[MP_H];
		word_normal(word_options, normal, picture);
		dxt3_encode(picture, 256, 64, pictures);
		word_highlighted(word_options, highlighted, picture);
		dxt3_encode(picture, 256, 64, pictures + 16384);
		word_normal(word_profiles, normal, picture);
		dxt3_encode(picture, 256, 64, pictures + 2 * 16384);
		word_highlighted(word_profiles, highlighted, picture);
		dxt3_encode(picture, 256, 64, pictures + 3 * 16384);
	}
	progress("making the gametype pictures");
	for (w = 0; w < 5; w++)
	{
		if (gametype_picture(rgba[RULES_0 + w], pictures + (4 + w) * 16384))
		{
			text_print(error, 256, "ui.map's gametype picture %d is empty", w);
			goto done;
		}
	}
	progress("making the ONLINE GAMES header");
	header_compose(rgba[FOUND], rgba[PROFILE], picture);
	dxt3_encode(picture, 512, 64, header);
	result = 0;
done:
	for (w = 0; w < WANTED; w++)
		free(rgba[w]);
	free(picture);
	free(art);
	free(tags);
	return result;
}
