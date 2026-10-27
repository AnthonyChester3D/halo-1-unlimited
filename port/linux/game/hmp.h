/*
HMP.H

Halo Universal's map features, ported to OpenCE on the Xbox (hmp_*.c, the Xbox build only):

	hmp_maps.c    map folders and packs in d:\maps and T:\maps (the multiplayer map list, names, descriptions,
	              pictures from preview.bin with a pack's series.bin, order.txt, the map's file for a "+key")
	hmp_browse.c  the map browser: sorts, LT / RT groups, favorites (Y), hiding (Black), the sort and button line,
	              the postgame's favorite button and its "<gametype> on <map>" line
	hmp_stats.c   play stats, favorites, hidden maps and the remembered sort (T:\hmp_stats.txt)
	hmp_game.c    the host's END GAME in the pause menu
	hmp_online.c  ONLINE PLAY in the Multiplayer menu (the online test builds, OXBOX_NET_P2P)
	hmp_share.c   map sharing: a client missing the host's folder map (or holding another build of it) downloads it
	              from the host (TCP 5152), and the background thread that writes files

A folder map's level name is "+" and its folder ("+Precipice"), or "+pack/folder" for a pack's map ('/' because
Halo cuts a level name at its last backslash). Each map is found by that key on every console.

Everything a match doesn't need is allocated while the menus run and freed when a game map loads
(hmp_menu_memory_release): the list's tables, the text caches, the browser's sort tables.
*/

#ifndef __HMP_H
#define __HMP_H

/* ---------- constants */

#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ 0x00000001
#endif
#ifndef LMEM_ZEROINIT
#define LMEM_ZEROINIT 0x0040
#endif
#ifndef FILE_ATTRIBUTE_DIRECTORY
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#endif
#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#endif

/* display indices (string list / picture frame numbers past the menus' own tags') */
enum
{
	HMP_NAME_BASE = 0x4000,         /* a folder map's name on one line; also its picture frame */
	HMP_WRAPPED_NAME_BASE = 0x5000, /* its name wrapped for the map list's 2-line box */
	HMP_DESCRIPTION_BASE = 0x6000,  /* its description, wrapped, with the browser's lines on top */
	HMP_LIVE_NAME = 0x7008,         /* (past ui_widget.c's kills-to-win descriptions, 0x7000-0x7005) the lobby's map name: map sharing's status while it runs, else the map's name */
	HMP_STRING_BASE = 0x7010,       /* hmp_game.c's and hmp_browse.c's own texts (0x7010-0x70ff) */
	HMP_ONLINE_STRING_BASE = 0x7090, /* hmp_online.c's (the online test builds: 0x7090-0x709f) */
	HMP_STOCK_DESCRIPTION_BASE = 0x7100, /* a stock map's description with the browser's lines on top */
	HMP_MISSING_NAME_BASE = 0x7200, /* a map this console hasn't (a game in the browser): its folder's name */
	HMP_MISSING_NAMES = 8,
	HMP_UNKNOWN_PICTURE = 13,       /* the frame Halo itself shows for an unknown map */
	HMP_STOCK_LEVELS = 13,
};

enum
{
	_hmp_sort_packs = 0,
	_hmp_sort_favorites,
	_hmp_sort_played,
	_hmp_sort_recent,
	_hmp_sort_players,
	_hmp_sort_genres,
	_hmp_sort_gametypes,
	_hmp_sort_name,
	NUMBER_OF_HMP_SORTS
};

/* ---------- structures */

struct hmp_rating
{
	unsigned long plays;
	unsigned long order;
	unsigned char favorite;
	unsigned char hidden;
	unsigned char known;
	unsigned char pad;
};

struct bitmap_data;
struct widget_instance;
struct event_record;

/* ---------- prototypes/HMP_MAPS.C */

/* scans the maps folders (the first time the list is asked for, and after a download) */
void hmp_maps_scan(void);
/* the folder maps found: how many, and map i's key ("+folder"), NULL past them */
long hmp_maps_count(void);
char const *hmp_maps_key(long map_index);
/* a level name's folder map (a "+key"), or NONE */
long hmp_maps_find(char const *level_name);
/* a list entry (a stock level's name or a key) -> 0-12 for a stock level, HMP_STOCK_LEVELS + folder map, NONE */
long hmp_maps_entry(char const *entry);
/* the stock levels' names (levels\test\bloodgulch\bloodgulch ...), the list's first 13 */
char const *hmp_maps_stock_level(long stock_index);
char const *hmp_maps_stock_name(long stock_index);    /* "bloodgulch" */
/* the first bytes of map i's info .txt; 0 = none */
long hmp_maps_read_info(long map_index, unsigned char *buffer, long size);
/* in the same pack (both stock, or both loose folder maps, count as one) */
boolean hmp_maps_same_pack(char const *entry0, char const *entry1);
/* a folder map's menu name (its info .txt's first line, or its folder), NULL for anything else */
wchar_t const *hmp_maps_title(char const *level_name);
/* the .map of a folder map (a "+key"): the path, TRUE; FALSE for any other level name */
boolean hmp_map_file_path(char const *level_name, char *path, long size);
/* map sharing: map i's files (0 the .map, 1 the .txt, 2 preview.bin, 3 its pack's series.bin; NULL = none), which
maps folder its copy is in (0 d:\maps, 1 T:\maps), whether there's a copy in the other, the .map's path in maps folder
`root` (NONE: the copy in use), and switching to the copy in `root` for the rest of the session */
char const *hmp_maps_file(long map_index, short which);
short hmp_maps_root(long map_index);
boolean hmp_maps_both(long map_index);
boolean hmp_maps_map_path(long map_index, short root, char *path, long size);
boolean hmp_maps_use_root(long map_index, short root);
/* the map list's table (the browser sorts it in place) and its length */
char **hmp_maps_table(void);
long hmp_maps_table_count(void);
void hmp_maps_table_set_count(long count);
long hmp_maps_table_capacity(void);
/* the map list widget's three cards: the list position each shows (NONE) */
long hmp_maps_visible_position(short card);
/* a texture cache read of one of our fake bitmaps (a preview, a gametype picture): TRUE when it was ours (read now) */
boolean hmp_texture_read(struct bitmap_data *bitmap, void *buffer, long size);
/* a string list index past the tags' (text_group.c): our text, or NULL */
wchar_t *hmp_string(long tag_index, short string_index);
/* the wrapped text for a box of the menus' fonts (large: ui\large_ui, else small_ui), into dst (cap characters) */
void hmp_wrap_text(wchar_t const *text, wchar_t *dst, long cap, boolean large, long width);
long hmp_text_width(wchar_t const *text, boolean large);
/* the menus' memory (the list's tables, the text caches) is freed when a game map loads */
void hmp_menu_memory_release(void);
/* a fake bitmap like the ones above, for a 32x16 picture made here (hmp_browse.c's gametype pictures) */
void hmp_bitmap_setup(struct bitmap_data *bitmap, long fake_index, short width, short height, short format,
	unsigned short flags, long size);
/* the map list's card `card` (0-2, 0 first each time) shows list position `position`: its name, frame, description */
void hmp_maps_list_item(short card, long position, short *name, short *frame, short *description);

/* ---------- prototypes/HMP_BROWSE.C */

void hmp_browse_scan(long map_count);
void hmp_browse_forget(long map_index);
void hmp_browse_apply(void);
boolean hmp_browse_same(long position0, long position1);
wchar_t const *hmp_browse_label(long position);
wchar_t const *hmp_browse_mark(char const *entry);
short hmp_browse_black(char const *entry);
/* a button on the map list (its selection and arrow flash): TRUE when it was ours */
boolean hmp_browse_event(struct widget_instance *list, struct event_record const *event);
void hmp_browse_visible(char const *selected);
/* after the map list is drawn: the sort and the button line */
void hmp_browse_render(short selected_index, float alpha);
/* the postgame scores: the button line with Y (=favorite), and the "<gametype> on <map>" line */
void hmp_browse_postgame_button_line(rectangle2d *bounds, wchar_t const *text);
void hmp_browse_postgame_title_line(void);
void hmp_browse_tick(void);
/* a fake bitmap of ours (PIC tag | 0xff00 + n): its pixels */
boolean hmp_browse_icon_read(long icon, void *buffer, long size);

/* ---------- prototypes/HMP_STATS.C */

unsigned long hmp_today(void);
void hmp_stats_seen(char const *name);
void hmp_stats_get(char const *name, struct hmp_rating *rating);
void hmp_stats_set(char const *name, short what, long value);
short hmp_stats_sort(void);
void hmp_stats_set_sort(short sort);
void hmp_stats_tick(void);
/* the background thread: write the file if there's a new one; TRUE when it did */
boolean hmp_stats_flush(void);

/* ---------- prototypes/HMP_GAME.C */

void hmp_game_pause_menu_opened(struct widget_instance *root, short local_player_index);
boolean hmp_game_handles_leave(struct widget_instance *widget);
boolean hmp_game_dispatch(struct widget_instance *root, struct event_record *event);
void hmp_game_after_dispatch(struct widget_instance *root);
wchar_t *hmp_game_string(short string_index);
void hmp_game_tick(void);

/* ---------- prototypes/HMP_ONLINE.C (the online test builds, OXBOX_NET_P2P) */

void hmp_online_tick(short local_player_index);
boolean hmp_online_dispatch(struct widget_instance *root, struct event_record *event);
void hmp_online_event(struct widget_instance *root, struct event_record *event);
void hmp_online_release(void);
void hmp_online_multiplayer_description(struct widget_instance *list, short index, short *picture, short *text);
wchar_t *hmp_online_string(short string_index);
struct bitmap_data *hmp_online_picture(long bitmap_tag_index, short *frame_index);
boolean hmp_online_picture_read(long fake_index, void *buffer, long size);

/* ---------- prototypes/HMP_SHARE.C */

/* the lobby's map name and countdown line while map sharing has something to say */
boolean hmp_share_status_active(void);
wchar_t *hmp_share_status_text(void);
/* every frame, menus and matches (main.c) */
void hmp_share_tick(void);
/* a log line (oxbox_log.txt) */
void hmp_log(char const *format, ...);
/* hands work to the background thread (TRUE = it runs) */
boolean hmp_worker_running(void);

#endif
