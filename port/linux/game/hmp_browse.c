/*
HMP_BROWSE.C

Halo Universal's map browser (hmp.h; Halo Universal's browse.c). The map list can be sorted eight ways; X (or White)
changes the sort, LT / RT jump between its groups, and the sort is remembered (hmp_stats.c):

	Packs            Halo's order (order.txt, packs together): Halo's maps, then each pack, then your maps
	Favorites        your favorites first, then the rest in Halo's order
	Most played      by this console's plays (20+, 10-19, 5-9, 2-4, once, never)
	Recently played  the last 5 played, then the others played before, then never played
	Players          by the recommended number of players (info.txt "Players:", or "4-16 players" in its text)
	Genres           a group per genre (maps\genres.txt), a map in each of its genres; then Uncategorized
	Gametypes        CTF, King of the Hill, Oddball, Race (a map in each it has), Slayer only, Unknown
	Name             A to Z, a group per letter

Every sort ends with a Hidden group. Black held 1.5 s opens the map's options on its card: Delete (a folder map, A
twice: its folder goes from both maps folders; not Halo's maps, nor the map the game is on), Hide / Show again, Cancel
(up / down, A, B); while they're up they take every button, so the list underneath stays put. Y makes a map a
favorite (again: not).
Each card's description starts with its group and "Favorite" / "Hidden". The screen shows the sort at the top right and
"Y =FAVORITE  X =SORT" in its button bar. After a match, on the scores, Y favorites the map just played, and the scores
say "<gametype> on <map>" under their title with the gametype's picture.
*/

#ifdef HALO_XBOX_TARGET

/* ---------- headers */

#include "cseries.h"
#include "tag_files/tag_groups.h"
#include "bitmaps/bitmap_group.h"
#include "text/draw_string.h"
#include "interface/ui_widget.h"
#include "networking/network_game_manager.h"
#include "interface/event_manager.h"
#include "input/input.h"
#include "game/game_engine.h"
#include "interface/player_ui.h"
#include "hmp.h"

#include <string.h>
#include <xtl.h>

/* ---------- constants */

#define MAXIMUM_POSITIONS 4096
#define HIDDEN_GROUP 255
#define MAXIMUM_GENRES 32
#define MAXIMUM_GENRE_ASSIGNMENTS 128
#define HIDE_HOLD_MILLISECONDS 1500
enum { PROMPT_DELETE, PROMPT_HIDE, PROMPT_CANCEL, PROMPT_CHOICES };
#define INVALID_FILE ((HANDLE)-1)
#define ICON_TEXTURE_WIDTH 64              /* (the gametype pictures: A8R8G8B8, swizzled, read from the XBE's file) */
#define ICON_TEXTURE_HEIGHT 64
#define ICON_SIZE 22                       /* (drawn 22 x 22 before the postgame's title) */
#define POSTGAME_TITLE_Y 72
#define POSTGAME_TITLE_MIDDLE 307

enum
{
	_gametype_ctf = 1,
	_gametype_king = 2,
	_gametype_oddball = 4,
	_gametype_race = 8,
	_gametype_slayer = 16,
	_gametype_known = 128
};

/* ---------- structures */

/* (ui_widget.c's: the list widget's fields this file moves) */
struct widget_animation_data
{
	short current_frame_index;
	short first_frame_index;
	short last_frame_index;
	short number_of_sprite_frames;
};
struct widget_instance
{
	long definition_tag_index;
	char const *name;
	short local_player_index;
	short horizontal_offset;
	short vertical_offset;
	short type;
	boolean visible;
	boolean render_regardless_of_controller_index;
	boolean disabled;
	boolean pause_game_time;
	boolean delete_recursion_lock;
	boolean widget_is_error_dialog;
	boolean close_if_local_player_controller_present;
	byte pad17;
	long creation_time;
	unsigned long milliseconds_to_auto_close;
	unsigned long auto_close_fade_time;
	real alpha_modifier;
	struct widget_instance *previous;
	struct widget_instance *next;
	struct widget_instance *parent;
	struct widget_instance *child;
	struct widget_instance *focused_child;
	struct
	{
		short selected_index;
		short last_list_tab_direction;
		void *list_items;
		word number_of_items;
		struct widget_instance *extended_description;
		wchar_t *item_text;
	} list;
	struct widget_animation_data animation;
};
typedef char verify_hmp_widget_instance_size[sizeof(struct widget_instance) == 0x58 ? 1 : -1];

struct map_tags
{
	unsigned long genres;
	unsigned char gametypes, players_minimum, players_maximum, parsed;
	char sort_name[12];
};

struct position_item
{
	unsigned short base;
	unsigned char group, pad;
	unsigned long sub;
};

/* ---------- prototypes */

void *__stdcall LocalAlloc(unsigned int flags, unsigned int bytes);
void *__stdcall LocalFree(void *memory);
char const *cache_files_loaded_map_name(void);
char const *network_game_current_map_name(void);
extern short draw_string_and_hack_in_icons_end;
extern struct game_variant global_variant;

/* ---------- globals */

static char const *const stock_titles[HMP_STOCK_LEVELS] =
{
	"Battle Creek", "Sidewinder", "Damnation", "Rat Race", "Prisoner", "Hang 'Em High", "Chill Out", "Derelict",
	"Boarding Action", "Blood Gulch", "Wizard", "Chiron TL34", "Longest"
};
static unsigned char const stock_players[HMP_STOCK_LEVELS][2] =
{
	{2,8},{4,16},{4,8},{2,6},{2,8},{4,16},{2,8},{4,8},{4,16},{4,16},{2,8},{2,6},{2,8}
};
static char const *const default_genres[] =
{
	"Competitive", "Casual", "Memes", "Artistic", "Custom vehicles", "Custom weapons", "Total conversions"
};
static char const *const sort_titles[NUMBER_OF_HMP_SORTS] =
{
	"PACKS", "FAVORITES", "MOST PLAYED", "RECENTLY PLAYED", "PLAYERS", "GENRES", "GAMETYPES", "NAME"
};
static char const *const engine_titles[5] = { "Capture the Flag", "Slayer", "Oddball", "King of the Hill", "Race" };
static unsigned char const engine_frames[6] = { 5, 0, 2, 3, 1, 4 };   /* game engine -> Halo's picture frame (5: unknown) */

static struct
{
	/* the menus' (freed with the list: hmp_browse_scan(NONE)) */
	struct map_tags *tags;
	long tag_count;
	struct map_tags stock_tags[HMP_STOCK_LEVELS];
	char **base;                       /* Halo's order (the base of every sort) */
	long base_count;
	unsigned char *groups;             /* each list position's group */
	struct position_item *items;
	boolean capped;
	char genre_names[MAXIMUM_GENRES][24];
	long genre_count;
	struct { unsigned char genre; char spec[32]; } assignments[MAXIMUM_GENRE_ASSIGNMENTS];
	long assignment_count;
	/* parsing */
	struct map_tags *parsing;
	long parse_line;
	boolean parse_players;
	/* the list's buttons */
	short sort;
	char const *black_entry;
	unsigned long black_last, black_start;
	boolean black_down;
	char const *tap_entry;
	unsigned long tap_time;
	/* Black held: the map's options (Delete, Hide / Show, Cancel) on its card, every button the prompt's */
	boolean prompt, prompt_armed;           /* armed: Delete chosen once, A again deletes */
	short prompt_choice;
	char prompt_key[64];
	long prompt_position;
	struct widget_instance *prompt_list;
	unsigned long prompt_seen, prompt_stick;
	short swallow;                          /* the button that closed it + 1, ignored until pressed afresh (0: none) */
	short prompt_grey;                      /* the prompt's line drawn in grey (Delete, when it can't be), or NONE */
	wchar_t prompt_text[160];
	wchar_t label[64], mark[32], text[160];
	/* the postgame */
	unsigned char pad_y[4];
	unsigned long postgame_seen, postgame_said;
	boolean postgame_favorite;
	struct bitmap_data icons[7];
} hmp_browse_globals;

#define g hmp_browse_globals

/* ---------- private code */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ieq(char const *a, char const *b)
{
	while (*a && lower(*a) == lower(*b)) { a++; b++; }
	return lower(*a) - lower(*b);
}
static char *scpy(char *d, char const *s) { while ((*d = *s) != 0) { d++; s++; } return d; }
static boolean is_alphanumeric(int c) { return (c >= '0' && c <= '9') || (lower(c) >= 'a' && lower(c) <= 'z'); }
static char *trim(char *s)
{
	char *t;
	while (*s == ' ' || *s == '\t') s++;
	for (t = s + strlen(s); t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '.'); t--) ;
	*t = 0;
	return s;
}
static unsigned long now_milliseconds(void) { return GetTickCount(); }

/* a genre's index by name, added when new (NONE: the list is full) */
static long genre_of(char const *name, boolean add)
{
	long k;
	char *o;
	if (!*name) return NONE;
	for (k = 0; k < g.genre_count; k++) if (!ieq(g.genre_names[k], name)) return k;
	if (!add || g.genre_count >= MAXIMUM_GENRES) return NONE;
	for (o = g.genre_names[g.genre_count], k = 0; name[k] && k < 23; k++) *o++ = name[k];
	*o = 0;
	return g.genre_count++;
}
static void genres_line(char *s)
{
	char *c;
	long genre;
	s = trim(s);
	if (!*s || *s == '#' || *s == ';') return;
	for (c = s; *c && *c != ':'; c++) ;
	if (*c == ':') *c++ = 0;
	if ((genre = genre_of(trim(s), TRUE)) == NONE) return;
	while (*c)                                       /* "Name: map, map": those maps are in it */
	{
		char *m = c;
		while (*c && *c != ',') c++;
		if (*c) *c++ = 0;
		m = trim(m);
		if (*m == '+') m++;
		if (*m && g.assignment_count < MAXIMUM_GENRE_ASSIGNMENTS && strlen(m) < 32)
		{
			g.assignments[g.assignment_count].genre = (unsigned char)genre;
			scpy(g.assignments[g.assignment_count].spec, m);
			g.assignment_count++;
		}
	}
}
static boolean read_lines(char const *path, void (*line_proc)(char *))
{
	unsigned char raw[512];
	char line[160];
	long n = 0, k;
	boolean first = TRUE, utf16 = FALSE, half = FALSE;
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_FILE) return FALSE;
	for (;;)
	{
		unsigned long got = 0;
		if (!ReadFile(file, raw, sizeof(raw), &got, NULL) || !got) break;
		k = 0;
		if (first)
		{
			first = FALSE;
			if (got >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) k = 3;
			else if (got >= 2 && raw[0] == 0xff && raw[1] == 0xfe) { k = 2; utf16 = TRUE; }
		}
		for (; k < (long)got; k++)
		{
			char c = (char)raw[k];
			if (utf16 && (half = !half) == FALSE) continue;
			if (c == '\n' || c == '\r') { line[n] = 0; line_proc(line); n = 0; }
			else if (n < (long)sizeof(line) - 1) line[n++] = c;
		}
	}
	line[n] = 0;
	line_proc(line);
	CloseHandle(file);
	return TRUE;
}
static void load_genres(void)
{
	long k;
	g.genre_count = g.assignment_count = 0;
	read_lines("d:\\maps\\genres.txt", genres_line);
	read_lines("T:\\maps\\genres.txt", genres_line);
	if (!g.genre_count) for (k = 0; k < (long)NUMBEROF(default_genres); k++) genre_of(default_genres[k], TRUE);
}
/* does a genres.txt name mean this map (a stock map's name or title, a folder's key, a pack's map by its folder)? */
static boolean spec_is(char const *spec, long entry)
{
	char const *k, *q, *key;
	if (entry < HMP_STOCK_LEVELS) return !ieq(spec, hmp_maps_stock_name(entry)) || !ieq(spec, stock_titles[entry]);
	key = hmp_maps_key(entry - HMP_STOCK_LEVELS);
	if (!key) return FALSE;
	k = key + 1;
	for (q = spec; *k; k++, q++) if (lower(*k == '/' ? '\\' : *k) != lower(*q == '/' ? '\\' : *q)) break;
	if (!*k && !*q) return TRUE;
	for (k = q = key + 1; *k; k++) if (*k == '/') q = k + 1;
	return q != key + 1 && !ieq(spec, q);
}

/* "4-16 players" / "Players: 4-16" / "8 players" */
static boolean players_in(char const *s, unsigned char *low, unsigned char *high)
{
	long a = 0, b = 0, k;
	while (*s == ' ') s++;
	for (k = 0; s[k] >= '0' && s[k] <= '9' && k < 3; k++) a = a * 10 + s[k] - '0';
	if (!k) return FALSE;
	s += k;
	while (*s == ' ') s++;
	if (*s == '-' || *s == 'x' || (s[0] == 't' && s[1] == 'o'))
	{
		s += *s == 't' ? 2 : 1;
		while (*s == ' ') s++;
		for (k = 0; s[k] >= '0' && s[k] <= '9' && k < 3; k++) b = b * 10 + s[k] - '0';
		if (!k) return FALSE;
	}
	else b = a;
	if (a < 1 || b < a || b > 16) return FALSE;
	*low = (unsigned char)a;
	*high = (unsigned char)b;
	return TRUE;
}
static boolean word_at(char const *s, char const *w) { while (*w && lower(*s) == *w) { s++; w++; } return !*w; }
static unsigned char gametypes_in(char const *s)
{
	unsigned char types = _gametype_known;
	char const *s0 = s;
	for (; *s; s++)
	{
		if (s > s0 && is_alphanumeric(s[-1])) continue;
		if (word_at(s, "ctf") || word_at(s, "capture")) types |= _gametype_ctf;
		else if (word_at(s, "king") || word_at(s, "hill") || word_at(s, "koth")) types |= _gametype_king;
		else if (word_at(s, "oddball") || word_at(s, "ball")) types |= _gametype_oddball;
		else if (word_at(s, "race")) types |= _gametype_race;
		else if (word_at(s, "slayer")) types |= _gametype_slayer;
	}
	return types;
}
static void sort_name(char *o, char const *s)
{
	long n = 0;
	for (; *s && n < 11; s++) if (is_alphanumeric(*s)) o[n++] = (char)lower(*s);
	o[n] = 0;
}
static void tag_line(char *s)
{
	char *v;
	long k;
	g.parse_line++;
	s = trim(s);
	if (g.parse_line == 1) { if (*s) sort_name(g.parsing->sort_name, s); return; }
	for (v = s; *v && *v != ':'; v++) ;
	if (*v == ':')
	{
		*v++ = 0;
		if (!ieq(trim(s), "genres"))
		{
			while (*v)
			{
				char *m = v;
				while (*v && *v != ',') v++;
				if (*v) *v++ = 0;
				if ((k = genre_of(trim(m), TRUE)) != NONE) g.parsing->genres |= 1u << k;
			}
			return;
		}
		if (!ieq(trim(s), "gametypes")) { g.parsing->gametypes = gametypes_in(v); return; }
		if (!ieq(trim(s), "players"))
		{
			if (players_in(v, &g.parsing->players_minimum, &g.parsing->players_maximum)) g.parse_players = TRUE;
			return;
		}
		v[-1] = ':';
	}
	if (!g.parse_players && !g.parsing->players_maximum)    /* "... 4-16 players" in the description */
	{
		for (v = s; *v; v++)
		{
			if (word_at(v, "player") && v > s)
			{
				char *n = v;
				while (n > s && n[-1] == ' ') n--;
				while (n > s && ((n[-1] >= '0' && n[-1] <= '9') || n[-1] == '-' || n[-1] == ' ')) n--;
				if (players_in(n, &g.parsing->players_minimum, &g.parsing->players_maximum)) break;
			}
		}
	}
}
static void tag_lines(unsigned char const *b, long got)
{
	char line[160];
	long n = 0, k = 0;
	boolean utf16 = FALSE;
	if (got >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) k = 3;
	else if (got >= 2 && b[0] == 0xff && b[1] == 0xfe) { k = 2; utf16 = TRUE; }
	for (; k < got; k += utf16 ? 2 : 1)
	{
		char c = (char)b[k];
		if (utf16 && k + 1 < got && b[k + 1]) c = '?';
		if (c == '\n' || c == '\r') { line[n] = 0; tag_line(line); n = 0; }
		else if (n < (long)sizeof(line) - 1) line[n++] = c;
	}
	line[n] = 0;
	if (n) tag_line(line);
}
static struct map_tags *tags_of(long entry)
{
	struct map_tags *t;
	long k;
	if (entry == NONE) return NULL;
	if (entry < HMP_STOCK_LEVELS)
	{
		t = &g.stock_tags[entry];
		if (!t->parsed)
		{
			t->parsed = 1;
			t->gametypes = _gametype_known | _gametype_ctf | _gametype_king | _gametype_oddball | _gametype_race | _gametype_slayer;
			t->players_minimum = stock_players[entry][0];
			t->players_maximum = stock_players[entry][1];
			sort_name(t->sort_name, stock_titles[entry]);
			for (k = 0; k < g.assignment_count; k++) if (spec_is(g.assignments[k].spec, entry)) t->genres |= 1u << g.assignments[k].genre;
		}
		return t;
	}
	if (!g.tags || entry - HMP_STOCK_LEVELS >= g.tag_count) return NULL;
	t = &g.tags[entry - HMP_STOCK_LEVELS];
	if (!t->parsed)
	{
		unsigned char text[1024];
		char const *s = hmp_maps_key(entry - HMP_STOCK_LEVELS) + 1, *q;
		long got;
		t->parsed = 1;
		for (q = s; *q; q++) if (*q == '/') s = q + 1;
		sort_name(t->sort_name, s);                  /* (the folder's name until info.txt gives one) */
		got = hmp_maps_read_info(entry - HMP_STOCK_LEVELS, text, sizeof(text));
		g.parsing = t;
		g.parse_line = 0;
		g.parse_players = FALSE;
		if (got > 0) tag_lines(text, got);
		for (k = 0; k < g.assignment_count; k++) if (spec_is(g.assignments[k].spec, entry)) t->genres |= 1u << g.assignments[k].genre;
	}
	return t;
}

static void tables_free(void)
{
	if (g.tags) { LocalFree(g.tags); g.tags = NULL; }
	if (g.base) { LocalFree(g.base); g.base = NULL; }
	if (g.groups) { LocalFree(g.groups); g.groups = NULL; }
	if (g.items) { LocalFree(g.items); g.items = NULL; }
	g.tag_count = g.base_count = 0;
}

static long item_compare(struct position_item const *a, struct position_item const *b)
{
	if (a->group != b->group) return a->group - b->group;
	if (g.sort == _hmp_sort_name)
	{
		struct map_tags const *x = tags_of(hmp_maps_entry(g.base[a->base])), *y = tags_of(hmp_maps_entry(g.base[b->base]));
		long c = x && y ? ieq(x->sort_name, y->sort_name) : 0;
		if (c) return c;
	}
	if (a->sub != b->sub) return a->sub < b->sub ? -1 : 1;
	return a->base - b->base;
}
static void put(long *n, long capacity, long b, long group, unsigned long sub)
{
	if (*n >= capacity) { g.capped = TRUE; return; }
	g.items[*n].base = (unsigned short)b;
	g.items[*n].group = (unsigned char)group;
	g.items[*n].sub = sub;
	(*n)++;
}
/* the list in the current sort, from the base order (the table holds it now) */
static void apply(void)
{
	char **table = hmp_maps_table();
	long count = hmp_maps_table_count(), capacity = hmp_maps_table_capacity();
	long b, n = 0, k, gap, recent = 0;
	if (!table || capacity <= 0) return;
	if (!g.items) g.items = (struct position_item *)LocalAlloc(0, (unsigned int)capacity * sizeof(struct position_item));
	if (!g.groups) g.groups = (unsigned char *)LocalAlloc(LMEM_ZEROINIT, (unsigned int)capacity);
	if (g.base) LocalFree(g.base);
	g.base = (char **)LocalAlloc(0, (unsigned int)(count > 0 ? count : 1) * sizeof(char *));
	if (!g.base || !g.items || !g.groups) { g.base_count = 0; return; }
	for (b = 0; b < count; b++) g.base[b] = table[b];
	g.base_count = count;
	g.capped = FALSE;
	for (b = 0; b < g.base_count; b++)
	{
		struct hmp_rating rating;
		struct map_tags *t;
		long entry = hmp_maps_entry(g.base[b]);
		hmp_stats_get(g.base[b], &rating);
		if (rating.hidden) { put(&n, capacity, b, HIDDEN_GROUP, 0); continue; }
		switch (g.sort)
		{
		default: put(&n, capacity, b, 0, 0); break;
		case _hmp_sort_favorites: put(&n, capacity, b, rating.favorite ? 0 : 1, 0); break;
		case _hmp_sort_played:
			put(&n, capacity, b, rating.plays >= 20 ? 0 : rating.plays >= 10 ? 1 : rating.plays >= 5 ? 2 : rating.plays >= 2 ? 3 :
				rating.plays ? 4 : 5, ~rating.plays);
			break;
		case _hmp_sort_recent: put(&n, capacity, b, rating.order && rating.plays ? 0 : 2, ~rating.order); break;
		case _hmp_sort_players:
			t = tags_of(entry);
			if (!t || !t->players_maximum) put(&n, capacity, b, 4, 0);
			else put(&n, capacity, b, t->players_maximum <= 4 ? 0 : t->players_maximum <= 8 ? 1 : t->players_maximum <= 12 ? 2 : 3,
				(unsigned long)t->players_maximum << 8 | t->players_minimum);
			break;
		case _hmp_sort_genres:
			t = tags_of(entry);
			if (!t || !t->genres) put(&n, capacity, b, MAXIMUM_GENRES, 0);
			else for (k = 0; k < g.genre_count; k++) if (t->genres >> k & 1) put(&n, capacity, b, k, 0);
			break;
		case _hmp_sort_gametypes:
			t = tags_of(entry);
			if (!t || !(t->gametypes & _gametype_known)) put(&n, capacity, b, 5, 0);
			else if (!(t->gametypes & (_gametype_ctf | _gametype_king | _gametype_oddball | _gametype_race))) put(&n, capacity, b, 4, 0);
			else for (k = 0; k < 4; k++) if (t->gametypes >> k & 1) put(&n, capacity, b, k, 0);
			break;
		case _hmp_sort_name:
			t = tags_of(entry);
			k = t && t->sort_name[0] >= 'a' && t->sort_name[0] <= 'z' ? t->sort_name[0] - 'a' + 1 : 0;
			put(&n, capacity, b, k, 0);
			break;
		}
	}
	for (gap = n / 2; gap > 0; gap /= 2)             /* Shell sort (the base order breaks ties) */
	{
		for (b = gap; b < n; b++)
		{
			struct position_item x = g.items[b];
			for (k = b; k >= gap && item_compare(&g.items[k - gap], &x) > 0; k -= gap) g.items[k] = g.items[k - gap];
			g.items[k] = x;
		}
	}
	for (k = 0; k < n; k++)
	{
		table[k] = g.base[g.items[k].base];
		g.groups[k] = g.items[k].group;
		if (g.sort == _hmp_sort_recent && g.groups[k] == 0 && ++recent > 5) g.groups[k] = 1;   /* (the last 5, then the rest) */
	}
	hmp_maps_table_set_count(n);
	if (g.capped) hmp_log("browse: the list is full (%d places); some maps are left out of some of their groups", capacity);
}

/* the list rebuilt in sort s from the same base order: where `keep` is now, or `position` (clamped) */
static long resort(short s, char const *keep, long position)
{
	char **table = hmp_maps_table();
	long k, count;
	if (!g.base || !table) return position;
	for (k = 0; k < g.base_count && k < hmp_maps_table_capacity(); k++) table[k] = g.base[k];
	hmp_maps_table_set_count(k);
	g.sort = s;
	apply();
	count = hmp_maps_table_count();
	if (keep) for (k = 0; k < count; k++) if (table[k] == keep) return k;
	return position >= count ? count - 1 : position < 0 ? 0 : position;
}

static void label_add(wchar_t **o, char const *s) { while (*s && *o < g.label + 63) *(*o)++ = (unsigned char)*s++; }
static void label_number(wchar_t **o, long v)
{
	char d[8], *p = d + 7;
	*p = 0;
	do *--p = (char)('0' + v % 10), v /= 10; while (v);
	label_add(o, p);
}
static wchar_t *put_text(wchar_t *o, char const *s) { while (*s) *o++ = (unsigned char)*s++; *o = 0; return o; }
static wchar_t *put_wide(wchar_t *o, wchar_t const *s, long maximum)
{
	while (s && *s && maximum--) *o++ = *s++;
	*o = 0;
	return o;
}

static boolean toggle_favorite(char const *entry)
{
	struct hmp_rating rating;
	hmp_stats_get(entry, &rating);
	hmp_stats_set(entry, 0, !rating.favorite);
	return !rating.favorite;
}

/* the list widget points at position `to` (its arrow flash: direction) with the new count */
static void select_position(struct widget_instance *list, long to, short direction)
{
	list->list.number_of_items = (word)hmp_maps_table_count();
	list->list.selected_index = (short)to;
	list->focused_child = list->child;
	if (direction) list->list.last_list_tab_direction = direction > 0 ? 15 : -15;
}

/* the tag of a name, looked up again only once the tags change */
static long tag_named(long group, char const *name)
{
	return tag_loaded(group, name);
}

static void text_mode(long font, short justification, real alpha)
{
	real_argb_color color = { 1.0f, 0.156863f, 0.588235f, 1.0f };   /* (the menus' light blue, as =BACK) */
	color.alpha = alpha;
	draw_string_set_draw_mode(font, NONE, justification, 0, &color);
	draw_string_set_tab_stops(NULL, 0);
}
static void draw_line(short y0, short x0, short y1, short x1, wchar_t const *text)
{
	rectangle2d bounds;
	bounds.y0 = y0; bounds.x0 = x0; bounds.y1 = y1; bounds.x1 = x1;
	draw_string_and_hack_in_icons(&bounds, NULL, NULL, 0, text, FALSE);
}
/* a line with button pictures that ends at x_end: measured (drawn see-through), then drawn left-aligned */
static short line_width(short y0, short y1, wchar_t const *text)
{
	real_argb_color color;
	real_argb_color clear;
	draw_string_get_color(&color);
	clear = color;
	clear.alpha = 0.0f;
	draw_string_set_color(&clear);
	draw_string_and_hack_in_icons_end = 0;
	draw_line(y0, 0, y1, 640, text);
	draw_string_set_color(&color);
	return draw_string_and_hack_in_icons_end > 0 ? draw_string_and_hack_in_icons_end : 0;
}
static void draw_button(char const *name, short x, short y, unsigned long alpha)
{
	long tag = tag_named('bitm', name);
	struct bitmap_data *bitmap;
	rectangle2d rect;
	if (tag == NONE || !(bitmap = bitmap_group_get_bitmap_from_sequence(tag, 0, 0))) return;
	rect.y0 = y; rect.x0 = x; rect.y1 = (short)(y + 25); rect.x1 = (short)(x + 20);   /* (y_butn, x_butn: 20 x 25) */
	draw_bitmap_in_rect(bitmap, &rect, &rect, NULL, alpha << 24 | 0xffffff, NULL, FALSE);
}

/* ---------- public code */

/* the maps were scanned (map_count of them), or the menus' memory is freed (NONE) */
void hmp_browse_scan(long map_count)
{
	long k;
	tables_free();
	for (k = 0; k < HMP_STOCK_LEVELS; k++) { g.stock_tags[k].parsed = 0; g.stock_tags[k].genres = 0; }
	if (map_count == NONE) return;
	g.tags = map_count > 0 ? (struct map_tags *)LocalAlloc(LMEM_ZEROINIT, (unsigned int)map_count * sizeof(struct map_tags)) : NULL;
	g.tag_count = g.tags ? map_count : 0;
	load_genres();
}

void hmp_browse_forget(long i)
{
	if (g.tags && i >= 0 && i < g.tag_count) { g.tags[i].parsed = 0; g.tags[i].genres = 0; }
}

void hmp_browse_apply(void)
{
	short s = hmp_stats_sort();
	if (!g.tags && hmp_maps_count() > 0)             /* (the list built again after a game freed the menus' memory) */
	{
		g.tags = (struct map_tags *)LocalAlloc(LMEM_ZEROINIT, (unsigned int)hmp_maps_count() * sizeof(struct map_tags));
		g.tag_count = g.tags ? hmp_maps_count() : 0;
		load_genres();
	}
	if (s >= 0 && s < NUMBER_OF_HMP_SORTS) g.sort = s;
	apply();
}

boolean hmp_browse_same(long a, long b)
{
	char **table = hmp_maps_table();
	long count = hmp_maps_table_count();
	if (!g.groups || !table || a < 0 || b < 0 || a >= count || b >= count || g.groups[a] != g.groups[b]) return FALSE;
	if (g.groups[a] == HIDDEN_GROUP) return TRUE;
	if (g.sort == _hmp_sort_packs || (g.sort == _hmp_sort_favorites && g.groups[a] == 1)) return hmp_maps_same_pack(table[a], table[b]);
	return TRUE;
}

wchar_t const *hmp_browse_label(long p)
{
	static char const *const played[6] = { "Played 20+ times", "Played 10-19 times", "Played 5-9 times", "Played 2-4 times",
		"Played once", "Never played" };
	static char const *const gametypes[6] = { "CTF", "King of the Hill", "Oddball", "Race", "Slayer only", "Gametypes unknown" };
	char **table = hmp_maps_table();
	wchar_t *o = g.label;
	long group, entry;
	g.label[0] = 0;
	if (!g.groups || !table || p < 0 || p >= hmp_maps_table_count()) return g.label;
	group = g.groups[p];
	entry = hmp_maps_entry(table[p]);
	if (group == HIDDEN_GROUP) label_add(&o, "Hidden");
	else switch (g.sort)
	{
	case _hmp_sort_favorites:
		if (group == 0) { label_add(&o, "Favorites"); break; }
		/* (the rest by pack) */
	case _hmp_sort_packs:
		if (entry < HMP_STOCK_LEVELS) label_add(&o, "Halo's maps");
		else
		{
			char const *k = hmp_maps_key(entry - HMP_STOCK_LEVELS) + 1;
			if (!strchr(k, '/')) label_add(&o, "Your maps");
			else { label_add(&o, "Pack: "); while (*k != '/' && o < g.label + 63) *o++ = (unsigned char)*k++; }
		}
		break;
	case _hmp_sort_played: label_add(&o, played[group > 5 ? 5 : group]); break;
	case _hmp_sort_recent: label_add(&o, group == 0 ? "Recently played" : group == 1 ? "Played before" : "Never played"); break;
	case _hmp_sort_players:
		if (group >= 4) label_add(&o, "Players: not known");
		else { label_add(&o, "Up to "); label_number(&o, group == 0 ? 4 : group == 1 ? 8 : group == 2 ? 12 : 16); label_add(&o, " players"); }
		break;
	case _hmp_sort_genres:
		if (group >= g.genre_count) label_add(&o, "Uncategorized");
		else { label_add(&o, "Genre: "); label_add(&o, g.genre_names[group]); }
		break;
	case _hmp_sort_gametypes: label_add(&o, gametypes[group > 5 ? 5 : group]); break;
	case _hmp_sort_name:
		if (group) { char c[2]; c[0] = (char)('A' + group - 1); c[1] = 0; label_add(&o, "Names: "); label_add(&o, c); }
		else label_add(&o, "Names: 0-9 and others");
		break;
	}
	*o = 0;
	return g.label;
}

/* the chosen gametype's title when entry's card says the map hasn't that gametype (its info.txt "Gametypes:" line,
which Halo Map Porter writes from the map's own netgame flags); NULL when it has it, when the card doesn't say, for
Slayer (any map), and for Halo's own maps (all of them). From the menus' card cache: nothing is read during a game */
char const *hmp_browse_unsupported(char const *entry)
{
	static unsigned char const needs[6] = { 0, _gametype_ctf, 0, _gametype_oddball, _gametype_king, _gametype_race };
	long engine = player_ui_get_game_variant_engine();
	struct map_tags const *t;
	if (!entry || engine < game_engine_ctf || engine > game_engine_race || !needs[engine]) return NULL;
	t = tags_of(hmp_maps_entry(entry));
	if (!t || !(t->gametypes & _gametype_known) || (t->gametypes & needs[engine])) return NULL;
	return engine_titles[engine - 1];
}

/* why entry's map can't be loaded (hmp_maps.c's map checks: its header, or opence.ini map_check's full check), NULL
when it can as far as is known; Halo's own maps, NULL */
char const *hmp_browse_problem(char const *entry)
{
	long e = hmp_maps_entry(entry);
	return e >= HMP_STOCK_LEVELS ? hmp_maps_problem(e - HMP_STOCK_LEVELS) : NULL;
}

/* the card's text says so (compose, hmp_maps.c): ui_widget.c draws it in yellow */
static boolean starts_with(wchar_t const *text, char const *prefix)
{
	long k;
	for (k = 0; prefix[k]; k++) if (text[k] != (unsigned char)prefix[k]) return FALSE;
	return TRUE;
}
/* the card's text in a colour of its own (ui_widget.c): 1 yellow ("Does not support ...", compose in hmp_maps.c),
   2 red (the prompt's last word before a delete); 0 as the tag has it */
long hmp_browse_text_colour(wchar_t const *text)
{
	return starts_with(text, "Does not support ") || starts_with(text, "Problem: ") ? 1 :
		starts_with(text, "Press A again to") ? 2 : 0;
}

wchar_t const *hmp_browse_mark(char const *entry)
{
	struct hmp_rating rating;
	wchar_t *o = g.mark;
	hmp_stats_get(entry, &rating);
	if (rating.favorite) o = put_text(o, "Favorite");
	if (rating.hidden) o = put_text(o, o > g.mark ? " Hidden" : "Hidden");
	*o = 0;
	return g.mark;
}

/* what entry's card says about Black: 0 nothing, 1 "Keep holding Black..." (held), 2 "Hold Black..." (just tapped) */
short hmp_browse_black(char const *entry)
{
	unsigned long now = now_milliseconds();
	if (!entry) return 0;
	if (g.black_down && g.black_entry == entry && now - g.black_start >= 150) return 1;
	if (g.tap_entry == entry && now - g.tap_time < 2000) return 2;
	return 0;
}

/* a button on the map list: X / White the next sort, Y favorite, Black held hide, LT / RT the previous / next group */
boolean hmp_browse_event(struct widget_instance *list, struct event_record const *event)
{
	char **table = hmp_maps_table();
	long count = hmp_maps_table_count();
	long selected = list->list.selected_index;
	short button = event->data.button.index;
	boolean press = event->data.button.value == 1;
	char const *entry;
	unsigned long now = now_milliseconds();
	if (!table || list->list.list_items != (void *)table || event->type != 3 /* _event_type_button */) return FALSE;
	entry = selected >= 0 && selected < count ? table[selected] : NULL;
	if (!entry) return FALSE;
	if ((button == 0 || button == 12) && (hmp_browse_unsupported(entry) || hmp_browse_problem(entry)))   /* A, Start: not a map
	                                                                                    without the gametype, or one that can't load */
	{
		if (press) ui_play_audio_feedback_sound(4);
		return TRUE;
	}
#if defined(OXBOX_SHOWCASE)
	/* (White, with opence.ini showcase_preview = 1: the map's showcase, if it has one; else White sorts, as X does) */
	if (button == 5)
	{
		extern int oxbox_setting_showcase_preview;
		boolean showcase_preview_request(char const *key);

		if (oxbox_setting_showcase_preview)
		{
			if (press) ui_play_audio_feedback_sound(showcase_preview_request(entry) ? 1 : 4);
			return TRUE;
		}
	}
#endif
	if (button == 2 || button == 5)                  /* X, White */
	{
		if (press)
		{
			short s = (short)((g.sort + 1) % NUMBER_OF_HMP_SORTS);
			select_position(list, resort(s, entry, selected), 1);
			hmp_stats_set_sort(s);
			ui_play_audio_feedback_sound(1);
		}
		return TRUE;
	}
	if (button == 3)                                 /* Y */
	{
		if (press) ui_play_audio_feedback_sound(toggle_favorite(entry) ? 2 : 3);
		return TRUE;
	}
	if (button == 4)                                 /* Black, held */
	{
		if (press || (!g.black_down && event->data.button.value <= 2))
		{
			g.black_down = TRUE;
			g.black_entry = entry;
			g.black_start = now;
		}
		g.black_last = now;
		if (g.black_entry && g.black_entry == entry && now - g.black_start >= HIDE_HOLD_MILLISECONDS)
		{
			long k;
			g.black_entry = NULL;                    /* (done until Black is let go) */
			g.prompt = TRUE;                         /* the map's options: hmp_browse_prompt_event has the buttons now */
			g.prompt_armed = FALSE;
			g.prompt_choice = PROMPT_CANCEL;
			for (k = 0; entry[k] && k < (long)sizeof(g.prompt_key) - 1; k++) g.prompt_key[k] = entry[k];
			g.prompt_key[k] = 0;
			g.prompt_position = selected;
			g.prompt_list = list;
			g.prompt_seen = g.prompt_stick = now;
			ui_play_audio_feedback_sound(1);
		}
		return TRUE;
	}
	if (button == 6 || button == 7)                  /* LT, RT: the first map of the previous / next group */
	{
		static unsigned long repeat_due[4];
		short pad = event->controller_index & 3;
		long k, p, to = selected, n = count;
		if (press) repeat_due[pad] = now + 450;
		else if ((long)(now - repeat_due[pad]) >= 0) repeat_due[pad] = now + 250;
		else return TRUE;
		for (k = 0; k < n && hmp_browse_same((k + n - 1) % n, k); k++) ;
		if (n >= 2 && k < n)                         /* (more than one group) */
		{
			if (button == 7)
			{
				for (k = 1; k < n; k++) { p = (selected + k) % n; if (!hmp_browse_same((p + n - 1) % n, p)) { to = p; break; } }
			}
			else
			{
				p = selected;
				for (k = 0; k < n && hmp_browse_same((p + n - 1) % n, p); k++) p = (p + n - 1) % n;
				p = (p + n - 1) % n;
				for (k = 0; k < n && hmp_browse_same((p + n - 1) % n, p); k++) p = (p + n - 1) % n;
				to = p;
			}
		}
		if (to == selected) { if (press) ui_play_audio_feedback_sound(4); }
		else
		{
			select_position(list, to, button == 7 ? 1 : -1);
			ui_play_audio_feedback_sound(1);
		}
		return TRUE;
	}
	return FALSE;
}

void hmp_browse_visible(char const *selected)
{
	if (g.black_entry && selected != g.black_entry) g.black_entry = NULL;
	if (g.tap_entry && selected != g.tap_entry) g.tap_entry = NULL;
}

/* after the map list is drawn: "SORT: GENRES" at the top right; "Y =FAVORITE  X =SORT" in the button bar, as Edit
Gametypes has its Y and X (the pictures at 78 and 250, the words at 100 and 272) */
void hmp_browse_render(short selected, float alpha)
{
	g.prompt_seen = now_milliseconds();          /* (the prompt's list is still there) */
	char **table = hmp_maps_table();
	long font = tag_named('font', "ui\\large_ui");
	char const *entry;
	struct hmp_rating rating;
	unsigned long a8;
	wchar_t *o;
	boolean y_picture, x_picture;
	if (!table || font == NONE || !(alpha > 0.0f)) return;
	if (alpha > 1.0f) alpha = 1.0f;
	a8 = (unsigned long)(long)(alpha * 255.0f + 0.5f);
	entry = selected >= 0 && selected < hmp_maps_table_count() ? table[selected] : NULL;
	hmp_browse_visible(entry);
	o = put_text(g.text, "SORT: ");
	put_text(o, sort_titles[g.sort]);
	text_mode(font, 1, alpha);
	draw_line(48, 330, 72, 605, g.text);
	hmp_stats_get(entry ? entry : "", &rating);
	text_mode(font, 0, alpha);
	y_picture = tag_named('bitm', "ui\\shell\\bitmaps\\y_butn") != NONE;
	x_picture = tag_named('bitm', "ui\\shell\\bitmaps\\x_butn") != NONE;
	draw_button("ui\\shell\\bitmaps\\y_butn", 78, 414, a8);
	put_text(g.text, y_picture ? (rating.favorite ? "=UNFAVORITE" : "=FAVORITE") :
		(rating.favorite ? "%y-button =UNFAVORITE" : "%y-button =FAVORITE"));
	draw_line(414, y_picture ? 100 : 78, 436, 250, g.text);
	draw_button("ui\\shell\\bitmaps\\x_butn", 250, 414, a8);
	put_text(g.text, x_picture ? "=SORT" : "%x-button =SORT");
	draw_line(414, x_picture ? 272 : 250, 436, 370, g.text);
}

/* the lobby's SELECT GAMETYPE: "X =EDIT  Y =NEW" in its button bar, left of Halo's B and A (hmp_game.c does them) */
void hmp_gametypes_render(float alpha)
{
	long font = tag_named('font', "ui\\large_ui");
	unsigned long a8;
	boolean x_picture, y_picture;
	if (font == NONE || !(alpha > 0.0f)) return;
	if (alpha > 1.0f) alpha = 1.0f;
	a8 = (unsigned long)(long)(alpha * 255.0f + 0.5f);
	text_mode(font, 0, alpha);
	x_picture = tag_named('bitm', "ui\\shell\\bitmaps\\x_butn") != NONE;
	y_picture = tag_named('bitm', "ui\\shell\\bitmaps\\y_butn") != NONE;
	draw_button("ui\\shell\\bitmaps\\x_butn", 78, 414, a8);
	put_text(g.text, x_picture ? "=EDIT" : "%x-button =EDIT");
	draw_line(414, x_picture ? 100 : 78, 436, 200, g.text);
	draw_button("ui\\shell\\bitmaps\\y_butn", 180, 414, a8);
	put_text(g.text, y_picture ? "=NEW" : "%y-button =NEW");
	draw_line(414, y_picture ? 202 : 180, 436, 300, g.text);
}

/* the postgame's button line: "Y =favorite" (=unfavorite) before Halo's, Y read here (the menus' frame isn't running
in a game); after a press "Added to favorites" / "Removed from favorites" for 2.5 s. Halo drew its line left-aligned
from x 380 (host) or 520 (client): the longer line ends where Halo's would have */
void hmp_browse_postgame_button_line(rectangle2d *bounds, wchar_t const *text)
{
	char const *map = cache_files_loaded_map_name();
	struct hmp_rating rating;
	wchar_t *o = g.text;
	unsigned long now = now_milliseconds();
	boolean fresh;
	short k, end, width;
	rectangle2d line;
	if (!game_engine || !map || !map[0] || !ieq(map, "ui"))
	{
		draw_string_and_hack_in_icons(bounds, NULL, NULL, 0, text, FALSE);
		return;
	}
	fresh = now - g.postgame_seen > 500;             /* (the scores just came up: a button already held doesn't count) */
	g.postgame_seen = now;
	for (k = 0; k < 4; k++)
	{
		struct gamepad_state const *pad = input_has_gamepad(k) ? input_get_gamepad_state(k) : NULL;
		unsigned char y = pad ? pad->buttons[3] : 0;
		if (y && !g.pad_y[k] && !fresh)
		{
			g.postgame_favorite = toggle_favorite(map);
			g.postgame_said = now;
			ui_play_audio_feedback_sound(g.postgame_favorite ? 2 : 3);
		}
		g.pad_y[k] = y != 0;
	}
	hmp_stats_get(map, &rating);
	if (now - g.postgame_said < 2500) o = put_text(o, g.postgame_favorite ? "Added to favorites" : "Removed from favorites");
	else o = put_text(o, rating.favorite ? "%y-button =unfavorite" : "%y-button =favorite");
	o = put_text(o, "    ");
	{
		/* (Halo's own words, lower case as on the Xbox's postgame: "=quit", "=pick game", whatever the map's text has) */
		wchar_t *words = o, *w;
		o = put_wide(o, text + (text[0] == '\t'), 150 - (long)(o - g.text));
		for (w = words; w < o; w++) if (*w >= 'A' && *w <= 'Z') *w = (wchar_t)(*w + 32);
	}
	width = line_width(bounds->y0, bounds->y1, text + (text[0] == '\t'));
	end = width ? (short)(bounds->x0 + width) : 600;
	width = line_width(bounds->y0, bounds->y1, g.text);
	line = *bounds;
	line.x0 = (short)(end - width);
	line.x1 = (short)(end + 40);
	draw_string_and_hack_in_icons(&line, NULL, NULL, 0, g.text, FALSE);
}

/* a gametype's picture (Halo's frame: 0 CTF, 1 King, 2 Slayer, 3 Oddball, 4 Race): a 64 x 64 A8R8G8B8 texture
(swizzled, in the texture cache), read from the XBE's file when it is drawn (hmp_browse_icon_read) */
static struct bitmap_data *icon_bitmap(long frame)
{
	struct bitmap_data *bitmap = &g.icons[frame >= 0 && frame < 7 ? frame : 5];
	if (!bitmap->signature)
	{
		hmp_bitmap_setup(bitmap, 0xff00 + (frame >= 0 && frame < 7 ? frame : 5), ICON_TEXTURE_WIDTH,
			ICON_TEXTURE_HEIGHT, 11, 0x89, ICON_TEXTURE_WIDTH * ICON_TEXTURE_HEIGHT * 4);
		bitmap->registration_point.x = 0;
		bitmap->registration_point.y = 0;
	}
	return bitmap;
}

/* "<gametype> on <map>" under POSTGAME CARNAGE REPORT, centred, with the gametype's picture */
void hmp_browse_postgame_title_line(void)
{
	char const *map = cache_files_loaded_map_name();
	long engine = global_variant.game_engine_index, k;
	wchar_t const *title;
	wchar_t *o = g.text;
	short width, x0;
	struct bitmap_data *bitmap;
	real_argb_color color;
	if (!game_engine || !map) return;
	for (k = 0; k < 11 && global_variant.human_readable_game_description[k]; k++) *o++ = global_variant.human_readable_game_description[k];
	*o = 0;
	if (o == g.text) o = put_text(o, engine >= 1 && engine <= 5 ? engine_titles[engine - 1] : "Multiplayer");
	o = put_text(o, " on ");
	if ((title = hmp_maps_title(map)) != NULL) o = put_wide(o, title, 48);
	else
	{
		long stock = NONE;
		for (k = 0; k < HMP_STOCK_LEVELS; k++) if (!ieq(map, hmp_maps_stock_name(k))) stock = k;
		if (stock != NONE) o = put_text(o, stock_titles[stock]);
		else for (k = map[0] == '+'; map[k] && k < 48; k++) *o++ = (unsigned char)map[k];
		*o = 0;
	}
	draw_string_set_format(NONE, 0, 0);
	width = line_width(POSTGAME_TITLE_Y, POSTGAME_TITLE_Y + 26, g.text);
	bitmap = icon_bitmap(engine_frames[engine >= 1 && engine <= 5 ? engine : 0]);
	if (engine < 1 || engine > 5)                     /* (an unknown game: the title alone) */
	{
		draw_line(POSTGAME_TITLE_Y, (short)(POSTGAME_TITLE_MIDDLE - width / 2), POSTGAME_TITLE_Y + 26, 640, g.text);
		return;
	}
	x0 = (short)(POSTGAME_TITLE_MIDDLE - (ICON_SIZE + 6 + width) / 2);
	draw_string_get_color(&color);
	{
		rectangle2d rect;
		unsigned long a8 = (unsigned long)(long)(color.alpha * 255.0f + 0.5f);
		rect.y0 = POSTGAME_TITLE_Y + 1; rect.x0 = x0; rect.y1 = (short)(rect.y0 + ICON_SIZE); rect.x1 = (short)(x0 + ICON_SIZE);
		draw_bitmap_in_rect(bitmap, &rect, NULL /* (all of the picture) */, NULL, (a8 > 255 ? 255 : a8) << 24 | 0xffffff, NULL, TRUE);
	}
	x0 += ICON_SIZE + 6;
	draw_line(POSTGAME_TITLE_Y, x0, POSTGAME_TITLE_Y + 26, 640, g.text);
}

/* the Xbox's texture layout: the low bits of x and y interleaved (x first) while both have bits left */
static unsigned long swizzle(long x, long y, long w, long h)
{
	unsigned long r = 0, m;
	long o = 0;
	for (m = 1; m < (unsigned long)w || m < (unsigned long)h; m <<= 1)
	{
		if (m < (unsigned long)w) { if (x & m) r |= 1u << o; o++; }
		if (m < (unsigned long)h) { if (y & m) r |= 1u << o; o++; }
	}
	return r;
}

boolean hmp_browse_icon_read(long icon, void *buffer, long size)
{
	extern int oxbox_picture_read(long index, void *buffer, unsigned long size);
	unsigned char *texel = (unsigned char *)buffer;
	long k;
	/* (Halo's frames CTF, King, Slayer, Oddball, Race are pictures 4-8 of the XBE's; "unknown" (5) has none; 6 is King's
	   picture made white, its shape solid and its glow faint, which the lobby draws the crown of in the text's colour) */
	if (icon < 0 || icon > 6 || icon == 5 || size != ICON_TEXTURE_WIDTH * ICON_TEXTURE_HEIGHT * 4) return FALSE;
	if (!oxbox_picture_read(4 + (icon == 6 ? 1 : icon), buffer, (unsigned long)size)) return FALSE;
	if (icon == 6)
	{
		/* (the crown: texels 16-48 by 4-29 of King's picture, scaled to the texture's width, 64 x 50, in its middle) */
		enum { CROWN_X = 16, CROWN_Y = 4, CROWN_W = 33, CROWN_H = 26, OUT_H = 50, OUT_Y = (64 - OUT_H) / 2 };
		unsigned char crown[CROWN_W * CROWN_H];
		long x, y;
		for (y = 0; y < CROWN_H; y++)
			for (x = 0; x < CROWN_W; x++)
				crown[y * CROWN_W + x] = texel[swizzle(CROWN_X + x, CROWN_Y + y, ICON_TEXTURE_WIDTH, ICON_TEXTURE_HEIGHT) * 4 + 3];
		for (y = 0; y < ICON_TEXTURE_HEIGHT; y++)
		{
			for (x = 0; x < ICON_TEXTURE_WIDTH; x++)
			{
				unsigned char *o = texel + swizzle(x, y, ICON_TEXTURE_WIDTH, ICON_TEXTURE_HEIGHT) * 4;
				unsigned char a = 0;
				if (y >= OUT_Y && y < OUT_Y + OUT_H)
					a = crown[((y - OUT_Y) * CROWN_H / OUT_H) * CROWN_W + x * CROWN_W / ICON_TEXTURE_WIDTH];
				o[0] = o[1] = o[2] = 255;
				o[3] = (unsigned char)(a > 200 ? 255 : a > 80 ? a / 3 : 0);
			}
		}
	}
	(void)k;
	return TRUE;
}

/* ---------- the map's options (Black held) */

static void prompt_close(short button)
{
	g.prompt = FALSE;
	g.prompt_armed = FALSE;
	g.prompt_list = NULL;
	g.swallow = (short)(button + 1);            /* (NONE + 1: none) */
}

/* the prompt's map deleted, hidden or shown again: the list rebuilt, at the same place (the next map) */
static void prompt_relist(void)
{
	struct widget_instance *list = g.prompt_list;
	if (!list) return;
	list->list.list_items = (void *)hmp_maps_table();
	select_position(list, resort(g.sort, NULL, g.prompt_position), 0);
}

/* whether the prompt's map can be deleted: a folder map, and not the map the lobby or game is on (why not: said in
the log once) */
static boolean prompt_can_delete(boolean say)
{
	char const *map = network_game_current_map_name();
	/* (by its name: hmp_maps_entry knows the map list's own strings only, and the prompt keeps a copy, which it took
	for one of Halo's maps: every Delete was refused, v1.43) */
	if (g.prompt_key[0] != '+' || hmp_maps_find(g.prompt_key) == NONE) return FALSE;
	if (map && !ieq(map, g.prompt_key))
	{
		if (say) hmp_log("browse: %s is the map the lobby or game is on: not deleted", g.prompt_key);
		return FALSE;
	}
	return TRUE;
}

static void prompt_act(void)
{
	struct hmp_rating rating;
	if (g.prompt_choice == PROMPT_CANCEL) { prompt_close(_gamepad_analog_button_a); ui_play_audio_feedback_sound(1); return; }
	if (g.prompt_choice == PROMPT_HIDE)
	{
		hmp_stats_get(g.prompt_key, &rating);
		hmp_stats_set(g.prompt_key, 2, !rating.hidden);
		prompt_relist();
		prompt_close(_gamepad_analog_button_a);
		ui_play_audio_feedback_sound(rating.hidden ? 2 : 3);
		return;
	}
	/* Delete: not Halo's maps, nor the map the game is on (hosted, or loaded); A twice */
	if (!prompt_can_delete(TRUE))
	{
		ui_play_audio_feedback_sound(4);
		return;
	}
	if (!g.prompt_armed) { g.prompt_armed = TRUE; ui_play_audio_feedback_sound(1); return; }
	if (!hmp_maps_delete(g.prompt_key))
	{
		hmp_log("browse: couldn't delete %s", g.prompt_key);
		prompt_close(_gamepad_analog_button_a);
		ui_play_audio_feedback_sound(4);
		return;
	}
	hmp_stats_set(g.prompt_key, 0, 0);
	hmp_stats_set(g.prompt_key, 2, 0);
	hmp_maps_scan();
	prompt_relist();
	prompt_close(_gamepad_analog_button_a);
	ui_play_audio_feedback_sound(2);
}

/* every event of the menus, before Halo's (ui_widget.c): while the prompt is up it takes them all, every
   controller's, so the list underneath doesn't move, choose or go back. TRUE when the event was the prompt's */
boolean hmp_browse_prompt_event(struct event_record const *event)
{
	unsigned long now = now_milliseconds();
	short move = 0;
	if (g.swallow && event->type == 3 /* _event_type_button */ && event->data.button.index == g.swallow - 1)
	{
		if (event->data.button.value > 1) return TRUE;   /* (still held from closing the prompt) */
		g.swallow = 0;
	}
	if (!g.prompt) return FALSE;
	if (now - g.prompt_seen > 500) { prompt_close(NONE); return FALSE; }   /* (the list isn't drawn: it's gone) */
	if (event->type == 1 /* _event_type_left_stick */)
	{
		if (now - g.prompt_stick >= 250 && (event->data.stick.y > 29490 || event->data.stick.y < -29490))
		{
			move = event->data.stick.y > 0 ? -1 : 1;
			g.prompt_stick = now;
		}
	}
	else if (event->type == 3 && event->data.button.value == 1)
	{
		switch (event->data.button.index)
		{
		case _gamepad_binary_button_dpad_up: move = -1; break;
		case _gamepad_binary_button_dpad_down: move = 1; break;
		case _gamepad_analog_button_a: prompt_act(); return TRUE;
		case _gamepad_analog_button_b: prompt_close(_gamepad_analog_button_b); ui_play_audio_feedback_sound(1); return TRUE;
		case _gamepad_analog_button_black: break;
		default: if (g.prompt_armed) { g.prompt_armed = FALSE; ui_play_audio_feedback_sound(1); } break;
		}
	}
	if (move)
	{
		short to = (short)(g.prompt_choice + move);
		g.prompt_armed = FALSE;
		if (to == PROMPT_DELETE && !prompt_can_delete(FALSE)) to = g.prompt_choice;   /* (greyed out: not a choice) */
		if (to >= 0 && to < PROMPT_CHOICES && to != g.prompt_choice) { g.prompt_choice = to; ui_play_audio_feedback_sound(1); }
	}
	return TRUE;
}

/* whether the map list is on the screen (drawn in the last half second): the maps aren't scanned again under it
(oxbox_sdk.c's FTP changes wait for it to close) */
int hmp_browse_list_showing(void)
{
	return g.prompt_seen && now_milliseconds() - g.prompt_seen < 500;
}

/* the prompt on entry's card (compose, hmp_maps.c), NULL when it isn't up for it */
wchar_t const *hmp_browse_prompt_text(char const *entry)
{
	struct hmp_rating rating;
	wchar_t *o = g.prompt_text;
	short k;
	if (!g.prompt || !entry || ieq(entry, g.prompt_key)) return NULL;
	if (g.prompt_armed) { put_text(o, "Press A again to\rDELETE this map\r(it can't be undone)"); return g.prompt_text; }
	hmp_stats_get(entry, &rating);
	g.prompt_grey = prompt_can_delete(FALSE) ? NONE : PROMPT_DELETE + 1;   /* (its line: after "This map:") */
	o = put_text(o, "This map:\r");
	for (k = 0; k < PROMPT_CHOICES; k++)
	{
		o = put_text(o, k == g.prompt_choice ? "> " : "   ");
		o = put_text(o, k == PROMPT_DELETE ? "Delete" : k == PROMPT_HIDE ? (rating.hidden ? "Show again" : "Hide") : "Cancel");
		if (k + 1 < PROMPT_CHOICES) o = put_text(o, "\r");
	}
	return g.prompt_text;
}

/* the prompt's text (the card being drawn: ui_widget.c) with a line greyed out: split in two of the same lines,
the usable ones (the grey one a space) and the grey one alone (the others spaces). FALSE when it isn't that */
boolean hmp_browse_prompt_split(wchar_t const *text, wchar_t *usable, wchar_t *unusable, long size)
{
	long k, line = 0;
	boolean blank_u = FALSE, blank_g;
	if (!g.prompt || g.prompt_armed || g.prompt_grey == NONE) return FALSE;
	for (k = 0; g.prompt_text[k] && text[k] == g.prompt_text[k]; k++) ;
	if (g.prompt_text[k] || text[k]) return FALSE;   /* (another card's text) */
	if (k >= size) return FALSE;
	for (k = 0; text[k]; k++)
	{
		wchar_t c = text[k];
		if (c == '\r' || c == '\n') { usable[k] = unusable[k] = c; line++; continue; }
		blank_u = line == g.prompt_grey;
		blank_g = !blank_u;
		usable[k] = blank_u ? ' ' : c;
		unusable[k] = blank_g ? ' ' : c;
	}
	usable[k] = unusable[k] = 0;
	return TRUE;
}

/* every frame: Black let go on the list */
void hmp_browse_tick(void)
{
	unsigned long now = now_milliseconds();
	if (g.black_down && now - g.black_last > 200)
	{
		if (g.black_entry) { g.tap_entry = g.black_entry; g.tap_time = now; }   /* (let go too soon: say how) */
		g.black_down = FALSE;
		g.black_entry = NULL;
	}
}

/* a level's card read into t (a folder map's info.txt); FALSE when it has none */
static boolean level_tags(char const *level_name, struct map_tags *t)
{
	struct map_tags *saved_parsing = g.parsing;
	long saved_line = g.parse_line;
	boolean saved_players = g.parse_players;
	unsigned char text[1024];
	long m, got;
	csmemset(t, 0, sizeof(*t));
	m = hmp_maps_find(level_name);
	if (m == NONE || (got = hmp_maps_read_info(m, text, sizeof(text))) <= 0) return FALSE;
	g.parsing = t;
	g.parse_line = 0;
	g.parse_players = FALSE;
	tag_lines(text, got);
	g.parsing = saved_parsing;
	g.parse_line = saved_line;
	g.parse_players = saved_players;
	return TRUE;
}

/* oxbox: a level's suggested most players, for the bots' Suggested (bots.c): the stock table's, or the folder
   map's card's ("Players: 4-16", or "4-16 players" in its description); 0 when it says none */
short hmp_browse_suggested_players(char const *level_name)
{
	struct map_tags t;
	long k;
	if (!level_name || !level_name[0]) return 0;
	for (k = 0; k < HMP_STOCK_LEVELS; k++)
	{
		char const *stock = hmp_maps_stock_level(k);
		if (stock && !ieq(stock, level_name)) return stock_players[k][1];
	}
	return level_tags(level_name, &t) ? t.players_maximum : 0;
}

/* oxbox: a stock level's name as the menus have it ("Blood Gulch"), NULL for a folder map */
char const *hmp_browse_stock_title(char const *level_name)
{
	long k;
	if (!level_name || !level_name[0]) return NULL;
	for (k = 0; k < HMP_STOCK_LEVELS; k++)
	{
		char const *stock = hmp_maps_stock_level(k);
		if (stock && !ieq(stock, level_name)) return stock_titles[k];
	}
	return NULL;
}

/* oxbox: whether a level is in a genre whose name has `word` in it ("artistic"), for the showcase's picks
(showcase.c): its card's Genres line, or maps\genres.txt. From the menus' card cache; FALSE without it */
boolean hmp_browse_in_genre(char const *level_name, char const *word)
{
	struct map_tags *t = level_name ? tags_of(hmp_maps_entry(level_name)) : NULL;
	long k, n, j;
	if (!t || !t->genres || !word || !word[0]) return FALSE;
	for (n = 0; word[n]; n++) ;
	for (k = 0; k < g.genre_count; k++)
	{
		char const *name = g.genre_names[k];
		if (!(t->genres >> k & 1)) continue;
		for (; *name; name++)
		{
			for (j = 0; j < n && name[j] && lower(name[j]) == lower(word[j]); j++) ;
			if (j == n) return TRUE;
		}
	}
	return FALSE;
}

/* oxbox: whether a level has CTF, for the showcase (showcase.c): every stock map does; a folder map when its card's
   "Gametypes:" line names it (Halo Map Porter writes it from the map's own flags). No line: not known, so no */
boolean hmp_browse_has_ctf(char const *level_name)
{
	struct map_tags t;
	long k;
	if (!level_name || !level_name[0]) return FALSE;
	for (k = 0; k < HMP_STOCK_LEVELS; k++)
	{
		char const *stock = hmp_maps_stock_level(k);
		if (stock && !ieq(stock, level_name)) return TRUE;
	}
	return level_tags(level_name, &t) && (t.gametypes & _gametype_known) && (t.gametypes & _gametype_ctf);
}

#endif /* HALO_XBOX_TARGET */


/* ---------- the lobby's machine names (ui_widget_game_data_input_functions.c) */

int map_share_machine_percent(long machine_index);
int map_share_download_percent(void);

#define LOBBY_HOST_NAMES 8
#define LOBBY_HOST_ICON_SIZE 22
static struct
{
	struct widget_instance *text[LOBBY_HOST_NAMES];   /* a machine's name text the lobby showed the host's name in */
	unsigned long at[LOBBY_HOST_NAMES];               /* when */
} lobby_hosts;

/* a machine's name as the lobby shows it: with its download's percentage after it ("Dopey 37%"), this console's own
   as it counts it, another's as the host said (its mark in the settings) */
wchar_t const *hmp_lobby_machine_name(wchar_t const *name, struct network_machine const *machine, boolean local,
	wchar_t *buffer, long buffer_count)
{
	int percent = local ? map_share_download_percent() : -1;
	long n = 0;
	char digits[4];
	int d = 0;
	if (percent < 0 && machine && NETWORK_MACHINE_DOWNLOAD_MARK(machine)) percent = NETWORK_MACHINE_DOWNLOAD_MARK(machine) - 1;
	if (percent < 0 || !name || buffer_count < 8) return name;
	if (percent > 100) percent = 100;
	while (name[n] && n < buffer_count - 6) { buffer[n] = name[n]; n++; }
	buffer[n++] = L' ';
	do { digits[d++] = (char)('0' + percent % 10); percent /= 10; } while (percent && d < 3);
	while (d) buffer[n++] = (wchar_t)digits[--d];
	buffer[n++] = L'%';
	buffer[n] = 0;
	return buffer;
}

/* the lobby: whether the machine whose name a text shows is the host's */
void hmp_lobby_note_name(struct widget_instance *text, boolean host)
{
	long i, free_slot = NONE;
	unsigned long now = GetTickCount();
	for (i = 0; i < LOBBY_HOST_NAMES; i++)
	{
		if (lobby_hosts.text[i] == text)
		{
			if (host) lobby_hosts.at[i] = now;
			else lobby_hosts.text[i] = NULL;
			return;
		}
		if (free_slot == NONE && (!lobby_hosts.text[i] || now - lobby_hosts.at[i] > 1000)) free_slot = i;
	}
	if (host && free_slot != NONE) { lobby_hosts.text[free_slot] = text; lobby_hosts.at[free_slot] = now; }
}

/* ui_widget.c's: a text box drawn, at the bounds its text was drawn in (its font and justification still set): the
   host's machine name has King of the Hill's picture just left of it */
void hmp_lobby_text_drawn(struct widget_instance *widget, rectangle2d const *bounds)
{
	long i;
	rectangle2d text, cursor, icon;
	real_argb_color color;
	unsigned long a8, argb;
	short middle;
	wchar_t const *string;
	for (i = 0; i < LOBBY_HOST_NAMES; i++) if (lobby_hosts.text[i] == widget) break;
	if (i == LOBBY_HOST_NAMES || GetTickCount() - lobby_hosts.at[i] > 250) return;
	/* (a text box's text: where this file's widget_instance has a list's first fields, ui_widget.c's union) */
	string = *(wchar_t const *const *)&widget->list.selected_index;
	if (!string || !string[0]) return;
	draw_unicode_string_compute_bounds(bounds, string, &text, &cursor);
	middle = draw_unicode_string_capital_middle(bounds, string);
	/* (King of the Hill's crown, in the name's colour: its texture is the crown alone, 64 x 50 in a 64 x 64 square) */
	icon.x1 = (short)(text.x0 - 5);
	icon.x0 = (short)(icon.x1 - LOBBY_HOST_ICON_SIZE);
	icon.y0 = (short)(middle - LOBBY_HOST_ICON_SIZE / 2 - 1);
	icon.y1 = (short)(icon.y0 + LOBBY_HOST_ICON_SIZE);
	draw_string_get_color(&color);
	a8 = (unsigned long)(long)(color.alpha * 255.0f + 0.5f);
	argb = (a8 > 255 ? 255 : a8) << 24 |
		(unsigned long)(long)(color.red * 255.0f + 0.5f) << 16 |
		(unsigned long)(long)(color.green * 255.0f + 0.5f) << 8 |
		(unsigned long)(long)(color.blue * 255.0f + 0.5f);
	draw_bitmap_in_rect(icon_bitmap(6), &icon, NULL /* (all of it) */, NULL, argb, NULL, TRUE);
}
