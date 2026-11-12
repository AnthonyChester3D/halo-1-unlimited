/* packart.c: a map pack's art as the game shows it (packart.h), as Halo Map Porter makes it (its mappack.cpp:
decode_frame, to_preview, find_menu_logos' cutting, fit_logo, series_bin), so a pack imported on the console looks as
one HMP made: its maps' pictures from a ui.map bitmap of whatever format and size the pack used, and its logo, cut
from its main menu, as the stamp on its maps' pictures (series.bin: port/linux/game/hmp_maps.c). */
#include "packart.h"

#include <stdlib.h>
#include <string.h>

#define PICTURE_WIDTH 256
#define PICTURE_HEIGHT 128
#define SHOWN_WIDTH 140                        /* (what Halo's menus show of a map picture: its top left) */
#define SHOWN_HEIGHT 114

/* ---- decoding ---- */

static unsigned rd16(unsigned char const *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned long rd32(unsigned char const *p) { return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24; }

static void rgb565(unsigned c, int *o)
{
	o[0] = (int)(c >> 11 & 31) * 255 / 31;
	o[1] = (int)(c >> 5 & 63) * 255 / 63;
	o[2] = (int)(c & 31) * 255 / 31;
}

/* a DXT colour block's 16 pixels' RGB (and black-transparent's alpha when three_colour applies) */
static void colour_block(unsigned char const *b, unsigned char *out, int stride, int dxt1)
{
	unsigned c0 = rd16(b), c1 = rd16(b + 2);
	unsigned long bits = rd32(b + 4);
	int pal[4][4], k, c;
	rgb565(c0, pal[0]);
	rgb565(c1, pal[1]);
	pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
	for (c = 0; c < 3; c++)
	{
		if (c0 > c1 || !dxt1)
		{
			pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
			pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
		}
		else
		{
			pal[2][c] = (pal[0][c] + pal[1][c]) / 2;
			pal[3][c] = 0;
		}
	}
	if (dxt1 && c0 <= c1)
		pal[3][3] = 0;
	for (k = 0; k < 16; k++)
	{
		unsigned char *d = out + (k >> 2) * stride + (k & 3) * 4;
		int const *p = pal[bits >> (2 * k) & 3];
		d[0] = (unsigned char)p[0], d[1] = (unsigned char)p[1], d[2] = (unsigned char)p[2], d[3] = (unsigned char)p[3];
	}
}

static void alpha3_block(unsigned char const *b, unsigned char *out, int stride)
{
	int k;
	for (k = 0; k < 16; k++)
		out[(k >> 2) * stride + (k & 3) * 4 + 3] = (unsigned char)((b[k >> 1] >> ((k & 1) * 4) & 15) * 17);
}

static void alpha5_block(unsigned char const *b, unsigned char *out, int stride)
{
	int a[8], k;
	unsigned long long bits = 0;
	a[0] = b[0], a[1] = b[1];
	for (k = 0; k < 6; k++)
		bits |= (unsigned long long)b[2 + k] << (8 * k);
	if (a[0] > a[1])
		for (k = 1; k < 7; k++)
			a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
	else
	{
		for (k = 1; k < 5; k++)
			a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5;
		a[6] = 0, a[7] = 255;
	}
	for (k = 0; k < 16; k++)
		out[(k >> 2) * stride + (k & 3) * 4 + 3] = (unsigned char)a[bits >> (3 * k) & 7];
}

/* the Xbox's swizzled (Morton order) textures: where pixel x, y is */
static unsigned long unswizzle(int x, int y, int w, int h)
{
	unsigned long o = 0, bit = 0;
	int m;
	for (m = 1; m < w || m < h; m <<= 1)
	{
		if (m < w)
			o |= (unsigned long)((x & m) ? 1 : 0) << bit++;
		if (m < h)
			o |= (unsigned long)((y & m) ? 1 : 0) << bit++;
	}
	return o;
}

unsigned char *art_decode(struct art_frame const *f, unsigned char const *src)
{
	int w = f->width, h = f->height, x, y, swizzled = (f->flags & 8) != 0;
	unsigned char *rgba;
	/* (a million pixels at most: 4 MB decoded, as the console's memory allows; menus' pictures are smaller) */
	if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || (long)w * h > 1024L * 1024 || !src)
		return NULL;
	rgba = malloc((size_t)w * h * 4);
	if (!rgba)
		return NULL;
	memset(rgba, 255, (size_t)w * h * 4);
	if (f->format == 14 || f->format == 15 || f->format == 16)      /* DXT1, DXT3, DXT5 */
	{
		int bytes = f->format == 14 ? 8 : 16, tw = (w + 3) & ~3, th = (h + 3) & ~3;
		unsigned long k = 0, need = (unsigned long)(tw / 4) * (th / 4) * bytes;
		unsigned char *tmp;
		if (f->size < need || !(tmp = malloc((size_t)tw * th * 4)))
		{
			free(rgba);
			return NULL;
		}
		for (y = 0; y < th; y += 4)
			for (x = 0; x < tw; x += 4, k += bytes)
			{
				unsigned char *d = tmp + ((size_t)y * tw + x) * 4;
				colour_block(src + k + (bytes == 16 ? 8 : 0), d, tw * 4, f->format == 14);
				if (f->format == 15)
					alpha3_block(src + k, d, tw * 4);
				else if (f->format == 16)
					alpha5_block(src + k, d, tw * 4);
			}
		for (y = 0; y < h; y++)
			memcpy(rgba + (size_t)y * w * 4, tmp + (size_t)y * tw * 4, (size_t)w * 4);
		free(tmp);
		return rgba;
	}
	if (f->format == 10 || f->format == 11)                          /* X8R8G8B8, A8R8G8B8 */
	{
		if (f->size < (unsigned long)w * h * 4)
		{
			free(rgba);
			return NULL;
		}
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
			{
				unsigned char const *s = src + (swizzled ? unswizzle(x, y, w, h) : (unsigned long)y * w + x) * 4;
				unsigned char *d = rgba + ((size_t)y * w + x) * 4;
				d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = f->format == 11 ? s[3] : 255;
			}
		return rgba;
	}
	if (f->format == 6 || f->format == 8 || f->format == 9)          /* R5G6B5, A1R5G5B5, A4R4G4B4 */
	{
		if (f->size < (unsigned long)w * h * 2)
		{
			free(rgba);
			return NULL;
		}
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
			{
				unsigned v = rd16(src + (swizzled ? unswizzle(x, y, w, h) : (unsigned long)y * w + x) * 2);
				unsigned char *d = rgba + ((size_t)y * w + x) * 4;
				if (f->format == 6)
					d[0] = (unsigned char)((v >> 11 & 31) * 255 / 31), d[1] = (unsigned char)((v >> 5 & 63) * 255 / 63),
						d[2] = (unsigned char)((v & 31) * 255 / 31);
				else if (f->format == 8)
					d[0] = (unsigned char)((v >> 10 & 31) * 255 / 31), d[1] = (unsigned char)((v >> 5 & 31) * 255 / 31),
						d[2] = (unsigned char)((v & 31) * 255 / 31), d[3] = v & 0x8000 ? 255 : 0;
				else
					d[0] = (unsigned char)((v >> 8 & 15) * 17), d[1] = (unsigned char)((v >> 4 & 15) * 17),
						d[2] = (unsigned char)((v & 15) * 17), d[3] = (unsigned char)((v >> 12 & 15) * 17);
			}
		return rgba;
	}
	free(rgba);
	return NULL;
}

/* ---- a map picture ---- */

static unsigned to565(int const *c)
{
	return (unsigned)((c[0] * 31 + 127) / 255) << 11 | (unsigned)((c[1] * 63 + 127) / 255) << 5 |
		(unsigned)((c[2] * 31 + 127) / 255);
}

/* a 4x4 block's RGB as DXT1 (four colours): its range's ends, along the way its colours go, pulled in a sixteenth */
static void dxt1_encode(int px[16][3], unsigned char *b)
{
	int lo[3], hi[3], mean[3], pal[4][3], t, cov[3] = {0, 0, 0}, c, k, s;
	unsigned c0, c1;
	unsigned long bits = 0;
	for (c = 0; c < 3; c++)
	{
		lo[c] = 255, hi[c] = 0, mean[c] = 0;
		for (k = 0; k < 16; k++)
		{
			t = px[k][c];
			if (t < lo[c])
				lo[c] = t;
			if (t > hi[c])
				hi[c] = t;
			mean[c] += t;
		}
		mean[c] >>= 4;
	}
	for (k = 0; k < 16; k++)
		for (c = 1; c < 3; c++)
			cov[c] += (px[k][0] - mean[0]) * (px[k][c] - mean[c]);
	for (c = 1; c < 3; c++)
		if (cov[c] < 0)
			t = lo[c], lo[c] = hi[c], hi[c] = t;
	for (c = 0; c < 3; c++)
	{
		t = (hi[c] - lo[c]) / 16;
		hi[c] -= t, lo[c] += t;
	}
	c0 = to565(hi), c1 = to565(lo);
	if (c0 < c1)
		t = (int)c0, c0 = c1, c1 = (unsigned)t;
	if (c0 != c1)
	{
		rgb565(c0, pal[0]);
		rgb565(c1, pal[1]);
		for (c = 0; c < 3; c++)
		{
			pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
			pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
		}
		for (k = 0; k < 16; k++)
		{
			int best = 0, best_d = 0x7fffffff;
			for (s = 0; s < 4; s++)
			{
				int d = 0;
				for (c = 0; c < 3; c++)
					t = px[k][c] - pal[s][c], d += t * t;
				if (d < best_d)
					best_d = d, best = s;
			}
			bits |= (unsigned long)best << (2 * k);
		}
	}
	b[0] = (unsigned char)c0, b[1] = (unsigned char)(c0 >> 8), b[2] = (unsigned char)c1, b[3] = (unsigned char)(c1 >> 8);
	b[4] = (unsigned char)bits, b[5] = (unsigned char)(bits >> 8), b[6] = (unsigned char)(bits >> 16);
	b[7] = (unsigned char)(bits >> 24);
}

int art_picture(struct art_frame const *f, unsigned char const *pixels, unsigned char *out)
{
	unsigned char *rgba;
	int x, y, k, bx, by;
	double vw, vh;
	static unsigned char shown[SHOWN_HEIGHT][SHOWN_WIDTH][3];
	if (f->width == PICTURE_WIDTH && f->height == PICTURE_HEIGHT && f->format == 14 && f->size >= ART_PICTURE_SIZE)
	{
		memcpy(out, pixels, ART_PICTURE_SIZE);
		return 0;
	}
	if (!(rgba = art_decode(f, pixels)))
		return 1;
	/* (each pixel the average of the source pixels it covers) */
	vw = f->width * (double)SHOWN_WIDTH / PICTURE_WIDTH;
	vh = f->height * (double)SHOWN_HEIGHT / PICTURE_HEIGHT;
	for (y = 0; y < SHOWN_HEIGHT; y++)
		for (x = 0; x < SHOWN_WIDTH; x++)
		{
			double x0 = x * vw / SHOWN_WIDTH, x1 = (x + 1) * vw / SHOWN_WIDTH, y0 = y * vh / SHOWN_HEIGHT;
			double y1 = (y + 1) * vh / SHOWN_HEIGHT, acc[3] = {0, 0, 0}, sum = 0;
			int sx, sy;
			for (sy = (int)y0; sy < y1 && sy < f->height; sy++)
				for (sx = (int)x0; sx < x1 && sx < f->width; sx++)
				{
					double wx = (x1 < sx + 1.0 ? x1 : sx + 1.0) - (x0 > sx ? x0 : sx);
					double wy = (y1 < sy + 1.0 ? y1 : sy + 1.0) - (y0 > sy ? y0 : sy);
					unsigned char const *s = rgba + ((size_t)sy * f->width + sx) * 4;
					for (k = 0; k < 3; k++)
						acc[k] += s[k] * wx * wy;
					sum += wx * wy;
				}
			for (k = 0; k < 3; k++)
				shown[y][x][k] = (unsigned char)(sum > 0 ? acc[k] / sum + 0.5 : 0);
		}
	free(rgba);
	/* (256x128 DXT1: the shown part, black around it) */
	for (by = 0, k = 0; by < PICTURE_HEIGHT; by += 4)
		for (bx = 0; bx < PICTURE_WIDTH; bx += 4, k += 8)
		{
			int px[16][3], i, c;
			for (i = 0; i < 16; i++)
			{
				int xx = bx + (i & 3), yy = by + (i >> 2);
				for (c = 0; c < 3; c++)
					px[i][c] = xx < SHOWN_WIDTH && yy < SHOWN_HEIGHT ? shown[yy][xx][c] : 0;
			}
			dxt1_encode(px, out + k);
		}
	return 0;
}

unsigned long long art_hash(unsigned char const *p, unsigned long n)
{
	unsigned long long h = 0xcbf29ce484222325ull;
	unsigned long i;
	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001b3ull;
	return h;
}

/* ---- the pack's logo ---- */

int art_logo_cut(unsigned char *px, int w, int h, int named, int screen, int named_only, struct art_logo *logo)
{
	int x, y, x0 = w, y0 = h, x1 = -1, y1 = -1, opaque = 1, total = 0, p99 = 255, run, a, k;
	long hist[256];
	double ink = 0, fw, fh;
	logo->rgba = NULL;
	logo->score = 0;
	for (k = 3; k < w * h * 4 && opaque; k += 4)
		opaque = px[k] >= 250;
	if (opaque)
	{
		/* (no alpha: drawn added onto the menu, so black is see-through) */
		for (k = 0; k < w * h * 4; k += 4)
		{
			a = px[k] > px[k + 1] ? px[k] : px[k + 1];
			a = a > px[k + 2] ? a : px[k + 2];
			for (y = 0; y < 3; y++)
				px[k + y] = (unsigned char)(a ? px[k + y] * 255 / a : 0);
			px[k + 3] = (unsigned char)a;
		}
	}
	memset(hist, 0, sizeof(hist));
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			a = px[((size_t)y * w + x) * 4 + 3];
			if (a < 16)
				continue;
			x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
			ink += a / 255.0;
			hist[a]++;
		}
	if (x1 < 0 || ink < 150)
	{
		free(px);
		return 0;                       /* (empty, or a few faint specks) */
	}
	fw = (x1 - x0 + 1.0) / w, fh = (y1 - y0 + 1.0) / h;
	if (!named && ((fw > 0.85 && fh > 0.85) || (screen && fw > 0.5 && fh > 0.5)))
	{
		free(px);
		return 0;                       /* (a backdrop or a frame, or a full-screen overlay) */
	}
	logo->score = (named ? 1000 : 0) + (screen ? -200 : 100) + (int)(ink / 50 < 300 ? ink / 50 : 300) - (named_only ? 500 : 0);
	/* (cut out, 2 pixels spare, its strongest strokes made solid: menus often draw logos faint and brighten them) */
	for (a = 16; a < 256; a++)
		total += (int)hist[a];
	for (a = 255, run = 0; a >= 16; a--)
		if ((run += (int)hist[a]) * 100 >= total)
		{
			p99 = a;
			break;
		}
	x0 = x0 - 2 > 0 ? x0 - 2 : 0, y0 = y0 - 2 > 0 ? y0 - 2 : 0;
	x1 = x1 + 2 < w - 1 ? x1 + 2 : w - 1, y1 = y1 + 2 < h - 1 ? y1 + 2 : h - 1;
	logo->width = x1 - x0 + 1, logo->height = y1 - y0 + 1;
	logo->rgba = malloc((size_t)logo->width * logo->height * 4);
	if (!logo->rgba)
	{
		free(px);
		return 0;
	}
	for (y = 0; y < logo->height; y++)
		for (x = 0; x < logo->width; x++)
		{
			unsigned char const *s = px + ((size_t)(y + y0) * w + x + x0) * 4;
			unsigned char *d = logo->rgba + ((size_t)y * logo->width + x) * 4;
			int alpha = s[3] * 255 / (p99 > 1 ? p99 : 1);
			d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = (unsigned char)(alpha < 255 ? alpha : 255);
		}
	free(px);
	return 1;
}

static int alpha_p99(unsigned char const *px, size_t n)
{
	long hist[256], total = 0, run = 0;
	int a;
	size_t i;
	memset(hist, 0, sizeof(hist));
	for (i = 3; i < n; i += 4)
		if (px[i] >= 16)
			hist[px[i]]++, total++;
	for (a = 255; a >= 16; a--)
		if ((run += hist[a]) * 100 >= total)
			return a;
	return 0;
}

unsigned long art_series(struct art_logo const *l, int box, unsigned char *out)
{
	int w, h, x, y, k, want, got, px, py;
	double s;
	unsigned char *fit;
	unsigned long n;
	if (!l->rgba || l->width <= 0 || l->height <= 0)
		return 0;
	box = box < 4 ? 4 : box > 64 ? 64 : box;
	s = (double)box / l->width < (double)box / l->height ? (double)box / l->width : (double)box / l->height;
	w = (int)(l->width * s + 0.5), h = (int)(l->height * s + 0.5);
	w = w < 1 ? 1 : w > 64 ? 64 : w, h = h < 1 ? 1 : h > 64 ? 64 : h;
	if (!(fit = malloc((size_t)w * h * 4)))
		return 0;
	/* (each pixel the area average of what it covers, its colours weighted by their alpha; enlarging: nearest) */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			double x0 = x * (double)l->width / w, x1 = (x + 1) * (double)l->width / w;
			double y0 = y * (double)l->height / h, y1 = (y + 1) * (double)l->height / h, acc[4] = {0, 0, 0, 0}, sum = 0;
			int sx, sy, sx_end = (int)x1 < x1 ? (int)x1 + 1 : (int)x1, sy_end = (int)y1 < y1 ? (int)y1 + 1 : (int)y1;
			unsigned char *d = fit + ((size_t)y * w + x) * 4;
			if (sx_end < (int)x0 + 1)
				sx_end = (int)x0 + 1;
			if (sy_end < (int)y0 + 1)
				sy_end = (int)y0 + 1;
			for (sy = (int)y0; sy < sy_end && sy < l->height; sy++)
				for (sx = (int)x0; sx < sx_end && sx < l->width; sx++)
				{
					double wx = (x1 < sx + 1.0 ? x1 : sx + 1.0) - (x0 > sx ? x0 : sx);
					double wy = (y1 < sy + 1.0 ? y1 : sy + 1.0) - (y0 > sy ? y0 : sy);
					unsigned char const *q = l->rgba + ((size_t)sy * l->width + sx) * 4;
					double a = q[3] / 255.0, wgt = (wx > 1e-6 ? wx : 1e-6) * (wy > 1e-6 ? wy : 1e-6);
					for (k = 0; k < 3; k++)
						acc[k] += q[k] * a * wgt;
					acc[3] += a * wgt;
					sum += wgt;
				}
			for (k = 0; k < 3; k++)
				d[k] = (unsigned char)(acc[3] > 0 ? (acc[k] / acc[3] < 255 ? acc[k] / acc[3] + 0.5 : 255) : 0);
			d[3] = (unsigned char)(sum > 0 ? acc[3] / sum * 255 + 0.5 : 0);
		}
	/* (lines thinner than a pixel once shrunk fade out: they get their strength back) */
	want = alpha_p99(l->rgba, (size_t)l->width * l->height * 4);
	got = alpha_p99(fit, (size_t)w * h * 4);
	if (got > 0 && want > got)
	{
		double gain = (double)want / got < 4 ? (double)want / got : 4;
		for (k = 3; k < w * h * 4; k += 4)
		{
			double v = fit[k] * gain + 0.5;
			fit[k] = (unsigned char)(v < 255 ? v : 255);
		}
	}
	/* (bottom right of what shows, 4 in) */
	px = SHOWN_WIDTH - 4 - w, py = SHOWN_HEIGHT - 4 - h;
	px = px < 0 ? 0 : px > PICTURE_WIDTH - w ? PICTURE_WIDTH - w : px;
	py = py < 0 ? 0 : py > PICTURE_HEIGHT - h ? PICTURE_HEIGHT - h : py;
	memcpy(out, "HUSB", 4);
	out[4] = (unsigned char)w, out[5] = 0, out[6] = (unsigned char)h, out[7] = 0;
	out[8] = (unsigned char)px, out[9] = (unsigned char)(px >> 8), out[10] = (unsigned char)py, out[11] = 0;
	memset(out + 12, 0, 4);
	for (k = 0, n = 16; k < w * h * 4; k += 4, n += 4)
		out[n] = fit[k + 2], out[n + 1] = fit[k + 1], out[n + 2] = fit[k], out[n + 3] = fit[k + 3];
	free(fit);
	return n;
}
