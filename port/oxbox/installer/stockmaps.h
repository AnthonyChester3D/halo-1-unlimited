/* stockmaps.h: Halo's 23 maps in a Halo folder (13 multiplayer, 10 campaign) swapped for small maps drawing on stock.shared, built from
the player's own maps, and put back exactly (stockmaps.c; docs/shared_maps.md) */
#ifndef OXBOX_STOCKMAPS_H
#define OXBOX_STOCKMAPS_H

#define STOCK_MAPS 23

/* what it's doing, for the installer's log */
typedef void (*stockmaps_say_t)(int warning, char const *text);

/* the drive written out (set by the installer): what's been done lasts through a power cut */
extern void (*stockmaps_flush_drive)(char const *path);

/* the log file only (set by the installer): "time:" lines, how long each step took and where the time went */
extern void (*stockmaps_log)(char const *text);

/* the cache drives (set by the installer, as "X:" and the like; NULL: none): while the swap reads the retail maps into
stock.shared it makes each small map in the maps folder (<map>.map.oxsmall) when its drive has the room, else there, so it
reads each map once; then each is renamed or copied into place. On a cache drive its files are named h1u_<map>.oxsmall;
they go when the swap's done with them, when it stops, and after a power cut (a map whose copy isn't there is made from its
retail map, as before) */
#define STOCKMAPS_CACHES 3
extern char const *stockmaps_cache[STOCKMAPS_CACHES];

/* how much room the cache drives need in all for the small maps (they're spread over them): none when the Halo folder's
drive has the room for them, beside its retail maps (then they're made in its maps folder and needn't be copied) */
unsigned long long stockmaps_cache_need(char const *halo);

/* the swap's files on the cache drives deleted */
void stockmaps_cache_clear(void);

struct stockmaps_look
{
	int retail_sized;              /* of the 23, how many are there with a retail map's file size */
	int small;                     /* how many draw on stock.shared */
	int restorable;                /* maps\stock.restore and maps\stock.shared are there */
};

/* looks at the folder's maps (their sizes and headers); first finishes or undoes a swap a power cut stopped */
void stockmaps_examine(char const *halo, struct stockmaps_look *look);

/* the swap: 0 when it's done. Nothing is changed until stock.shared and its tables are built and read back; then a retail
map isn't touched until its small map is the one it must be (the table's CRC) and reads back from the disk. Else error[256] */
int stockmaps_convert(char const *halo, stockmaps_say_t say, char *error);

/* puts the retail maps back, each checked against its original's size and CRC, then takes out what the swap added;
0 when done (or nothing to do), else error[256]: what isn't back yet still plays in Halo 1 Unlimited */
int stockmaps_restore(char const *halo, stockmaps_say_t say, char *error);

#endif
