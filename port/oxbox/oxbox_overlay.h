/* retail XBE ranges no retail library code touches (game .text and globals): OpenCE .bss goes here */
/* (0x210520-0x216590 is left alone: it holds the retail loading screen picture, 0x210988-0x217174, which
source/interface/progress_bar.c reads) */
static const struct { unsigned long start, end; } oxbox_overlay_regions[] = {
	{ 0x25000, 0x180c00 },
	{ 0x1f3650, 0x1f8a70 },
	{ 0x1f9350, 0x1ffef0 },
	{ 0x2058d0, 0x20a2d0 },
	{ 0x217280, 0x23ff70 },
	{ 0x24db30, 0x25f580 },
	{ 0x270a50, 0x292770 },
	{ 0x2c00d0, 0x2f9940 },
	{ 0x2fa000, 0x2fdf00 },
	{ 0x30bf10, 0x339e70 },
	{ 0x345100, 0x3a5e80 },
};
