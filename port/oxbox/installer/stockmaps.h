/* stockmaps.h: Halo's 13 multiplayer maps in a Halo folder swapped for small maps drawing on stock.shared, built from
the player's own maps, and put back exactly (stockmaps.c; docs/shared_maps.md) */
#ifndef OXBOX_STOCKMAPS_H
#define OXBOX_STOCKMAPS_H

#define STOCK_MAPS 13

/* what it's doing, for the installer's log */
typedef void (*stockmaps_say_t)(int warning, char const *text);

/* the drive written out (set by the installer): what's been done lasts through a power cut */
extern void (*stockmaps_flush_drive)(char const *path);

struct stockmaps_look
{
	int retail_sized;              /* of the 13, how many are there with a retail map's file size */
	int small;                     /* how many draw on stock.shared */
	int restorable;                /* maps\stock.restore and maps\stock.shared are there */
};

/* looks at the folder's maps (their sizes and headers); first finishes or undoes a swap a power cut stopped */
void stockmaps_examine(char const *halo, struct stockmaps_look *look);

/* the swap: 0 when it's done. Nothing is changed until everything is built and proved: every map's restore is tried
first, and the retail map isn't touched until the restore makes its exact bytes. Else error[256] */
int stockmaps_convert(char const *halo, stockmaps_say_t say, char *error);

/* puts the retail maps back, each checked against its original's size and CRC, then takes out what the swap added;
0 when done (or nothing to do), else error[256]: what isn't back yet still plays in Halo 1 Unlimited */
int stockmaps_restore(char const *halo, stockmaps_say_t say, char *error);

#endif
