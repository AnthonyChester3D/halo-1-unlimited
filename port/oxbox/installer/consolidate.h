/* consolidate.h: a pack's maps moved onto shared files (consolidate.c, docs/shared_maps.md): what it would save (it only
reads), and doing it. */
#ifndef OXBOX_CONSOLIDATE_H
#define OXBOX_CONSOLIDATE_H

#include <stdint.h>

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

struct consolidate_result
{
	int converted;                  /* now small, drawing on stock.shared and the pack's file */
	int already, unreadable;        /* left as they are */
	int too_big;                    /* would unpack to more than a multiplayer map may: left as they are */
	int failed;                     /* didn't read back right: left as they are */
	unsigned long long before;      /* the converted maps' files as they were */
	unsigned long long after;       /* their small maps and the pack's file */
	char shared[32];                /* the pack's file's name, without .shared ("" for none) */
};

/* the log file only (set by the installer): "time:" lines, how long each step took and where the time went, as the stock
maps' swap logs them */
extern void (*consolidate_log)(char const *text);

/* the maps at paths (count of them) measured, against stock_index (maps\stock.index). 0 when done, else error[256] */
int consolidate_measure(char const *stock_index, char const *const *paths, int count, consolidate_say_t say,
	struct consolidate_measure *result, char *error);

/* the maps at paths (count of them; multiplayer maps imported as links) converted against stock_folder's stock.index and
stock.shared: each one's small map written in folders[i] (its map.link's folder) and checked, then put beside the link (the
game plays a folder's .map before its link);
the blocks two or more of them use in pack_folder\<pack_prefix>-<its checksum>.shared (pack_prefix: at most 21 characters).
The maps at paths are left as they are. A map that can't be converted, or doesn't read back right, keeps its link. 0 when
done (result says how it went), else error[256], and nothing is changed */
int consolidate_convert(char const *stock_folder, char const *pack_folder, char const *pack_prefix, char const *const *paths,
	char const *const *folders, int count, consolidate_say_t say, struct consolidate_result *result, char *error);

/* a map's identity: its file's size and the CRC32s of its first and last 64 KiB (all of it when it's smaller). 0 when read */
int consolidate_identity(char const *path, uint32_t *size, uint32_t *head, uint32_t *tail);

/* a map a pack's file was made from, as its list names it: the original's identity, its small map's length and CRC32
(unpacked) */
struct consolidate_member
{
	uint32_t size, head, tail, small_length, small_crc;
};

/* a pack's file's list of the maps it was made from: up to most of them in members, how many in *count, the file's name
(its header's, 32 bytes) in name. 0 when read (a file with no list isn't) */
int consolidate_members(char const *pack_path, struct consolidate_member *members, int most, int *count, char *name);

/* the full-size maps at paths (count of them; each one its list names) made small against stock_folder's stock.index and
stock.shared and the pack's file at pack_path, which must be where the game finds it for them: each one is made, its CRC
checked against the list's, read back, then takes its full-size one's place. One that doesn't come out as the list says
stays as it is. 0 when done (result says how it went), else error[256], and nothing is changed */
int consolidate_shrink(char const *stock_folder, char const *pack_path, char const *const *paths, int count,
	consolidate_say_t say, struct consolidate_result *result, char *error);

#endif
