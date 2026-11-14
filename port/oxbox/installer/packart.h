/* packart.h: a map pack's art as the game shows it (packart.c): a ui.map bitmap of any format and size made a map
picture, and the pack's logo made its maps' stamp (series.bin). As Halo Map Porter makes them (its mappack.cpp). */
#ifndef OXBOX_PACKART_H
#define OXBOX_PACKART_H

#define ART_PICTURE_SIZE 16384                 /* a map picture: 256x128 DXT1, its top-left 140x114 shown */
#define ART_SERIES_MOST (16 + 64 * 64 * 4)     /* the biggest series.bin */

/* a bitmap's picture as ui.map has it (a bitmap_data: 0x30 bytes, its pixels in the map) */
struct art_frame
{
	int width, height, format, flags;
	unsigned long offset, size;
};

/* its pixels decoded (width * height RGBA, malloc'd: free it); NULL when its format isn't one Halo's menus use */
unsigned char *art_decode(struct art_frame const *frame, unsigned char const *pixels);

/* a map picture (ART_PICTURE_SIZE bytes) from it: the same share of it Halo shows of its own (the top-left 140/256 x
114/128), made 140x114 at the top left. 0 when done */
int art_picture(struct art_frame const *frame, unsigned char const *pixels, unsigned char *out);

/* FNV-1a of its pixels (Halo's own HALO logo's is ART_STOCK_LOGO: the menu didn't change it) */
#define ART_STOCK_LOGO 0xa445b303f9e75e59ull
unsigned long long art_hash(unsigned char const *pixels, unsigned long size);

/* a logo on the pack's main menu: cut out of its picture (rgba, width x height; taken over: freed here), how likely
it's the logo (named: its tag's or widget's name says logo; screen: its widget covers the screen; named_only: a
logo-named bitmap not on the menu itself). 0 when it's no logo (empty, a backdrop, an overlay) */
struct art_logo
{
	int score, width, height;
	unsigned char *rgba;                       /* (malloc'd; NULL: none) */
};
int art_logo_cut(unsigned char *rgba, int width, int height, int named, int screen, int named_only, struct art_logo *logo);

/* series.bin for it: fitted in box x box (strokes kept strong), at the picture's bottom right, 4 in from its edges;
its size, 0 when none */
unsigned long art_series(struct art_logo const *logo, int box, unsigned char *out);

#endif
