/* consolidate.h: what moving a pack's maps onto shared files would save (consolidate.c, docs/shared_maps.md). This part only
reads: nothing is written or changed. */
#ifndef OXBOX_CONSOLIDATE_H
#define OXBOX_CONSOLIDATE_H

struct consolidate_measure
{
	int maps;                       /* measured */
	int unreadable;                 /* its tags don't read: such a map would be left as it is */
	int already;                    /* already draws on shared files: left as it is */
	unsigned long long before;      /* the measured maps' files */
	unsigned long long in_stock;    /* textures and sounds stock.shared has, counted in each map using them */
	unsigned long long in_pack;     /* those two or more of the pack's maps use, counted once: the pack's shared file */
	unsigned long long own;         /* those one map uses: they stay in it */
	unsigned long long after;       /* the maps and the pack's file, packed: an estimate */
};

typedef void (*consolidate_say_t)(int warning, char const *text);

/* the maps at paths (count of them) measured, against stock_index (maps\stock.index). 0 when done, else error[256] */
int consolidate_measure(char const *stock_index, char const *const *paths, int count, consolidate_say_t say,
	struct consolidate_measure *result, char *error);

#endif
