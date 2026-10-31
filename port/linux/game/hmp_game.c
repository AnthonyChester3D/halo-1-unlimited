/*
HMP_GAME.C

Halo Universal's END GAME for the host (hmp.h; Halo Universal's game.c). In a network match the host's pause menu
gets a third item under RESUME GAME and LEAVE GAME: END GAME. It asks first, in Halo's own alert box ("END GAME?" /
"Are you sure?" with the pause menu's button line "B =RESUME  A =QUIT"); A ends the match for everyone as a score limit
does (the announcer, the postgame scores, then the host's A takes everyone back to the lobby), B or Back goes back to
the game. Only the host gets the item, once the match has run 3 seconds and every console has loaded it.

The item: a copy of LEAVE GAME's list item made from the same definition by its index alone, without the datum's salt
(tag lookups take either; the menus find list items by datum, so the copy needs its own), linked after it, a row lower, with
the text END GAME. LEAVE GAME's A handler (network_game_remove_local_player) asks hmp_game_handles_leave first. The
dialog opens after the menus are done with that event (hmp_game_after_dispatch), and takes its own A / B before them
(hmp_game_dispatch).
*/

#ifdef HALO_XBOX_TARGET

/* ---------- headers */

#include "cseries.h"
#include "tag_files/tag_groups.h"
#include "interface/ui_widget.h"
#include "interface/event_manager.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "hmp.h"

#include <string.h>
#include <xtl.h>

/* ---------- constants */

enum
{
	_string_item = HMP_STRING_BASE,      /* END GAME */
	_string_dialog_text,                 /* Are you sure? */
	_string_header,                      /* END GAME? */
	_string_key_back,                    /* =RESUME */
	_string_key_select,                  /* =QUIT */
	_string_options_item,                /* OPTIONS (the main menu's 4th item) */
	_string_options_header,              /* OPTIONS */
	_string_key_done,                    /* =DONE */
	_string_key_change,                  /* =CHANGE */
	NUMBER_OF_GAME_STRINGS = 9,
	_string_options_text = HMP_STRING_BASE + NUMBER_OF_GAME_STRINGS   /* the options dialog's text (options_text) */
};

#define SETTINGS_PATH "T:\\opence_settings.txt"   /* OpenCE's own settings, beside Halo's saves (Halo never reads it) */

#define LEAVE_GAME_FUNCTION 72           /* (network_game_remove_local_player in the event handler function table) */

/* ---------- structures */

/* (ui_widget.c's) */
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
		wchar_t *text;
		short string_list_index;
	} text_box;
	byte list_parameters[0x50 - 0x44];
	struct widget_animation_data animation;
};
typedef char verify_hmp_game_widget_instance_size[sizeof(struct widget_instance) == 0x58 ? 1 : -1];

/* ---------- prototypes */

void ui_widget_delete(struct widget_instance *widget);
struct widget_instance *ui_widget_get_active_widget(short local_player_index);
boolean network_game_server_hosting_all_loaded(void);
void game_engine_end_game_now(void);

/* ---------- globals */

static char const *const game_strings[NUMBER_OF_GAME_STRINGS] =
	{ "END GAME", "Are you sure?", "END GAME?", "=RESUME", "=QUIT", "OPTIONS", "OPTIONS", "=DONE", "=CHANGE" };

static struct
{
	wchar_t strings[NUMBER_OF_GAME_STRINGS][16];
	long item[4];                        /* the END GAME item's datum, each local player's (0: none) */
	boolean pending[4];                  /* A on the item: the dialog opens after this event */
	struct widget_instance *dialog[4], *dialog_text[4];
	long dialog_datum[4];
	char size[4];                        /* the pause menu's size: '1' full, '2' half, '4' quarter screen */
	/* the main menu's OPTIONS item and its dialog */
	struct widget_instance *main_menu[4];
	long main_menu_time[4];
	long options_item[4];                /* the OPTIONS item's datum on that main menu (0: none) */
	boolean options_focus[4];            /* the main menu comes back from the dialog: OPTIONS gets the focus */
	struct widget_instance *options[4], *options_text_box[4];
	long options_datum[4];
	wchar_t options_text[128];
} hmp_game_globals;

/* OpenCE's settings (T:\\opence_settings.txt: "name=value" lines) */
enum { _aspect_dashboard, _aspect_standard, _aspect_widescreen, NUMBER_OF_ASPECTS };
static struct
{
	boolean loaded;
	long aspect;
} hmp_settings;

/* ---------- private code */

static boolean name_is(struct widget_instance *w, char const *n) { return w->name && !strcmp(w->name, n); }
static boolean name_starts(struct widget_instance *w, char const *n) { return w->name && !strncmp(w->name, n, strlen(n)); }
static byte *definition_of(struct widget_instance *w) { return (byte *)tag_get('DeLa', w->definition_tag_index); }
static short bounds_height(struct widget_instance *w)
{
	byte *d = definition_of(w);
	return (short)(*(short *)(d + 0x28) - *(short *)(d + 0x24));
}

/* does A on this item run LEAVE GAME? (its definition's event handlers: flags at +0, function at +6, 0x48 each) */
static boolean is_leave_item(struct widget_instance *w)
{
	byte *d = definition_of(w), *h;
	long i, n = *(long *)(d + 0x54);
	h = *(byte **)(d + 0x58);
	for (i = 0; h && i < n && i < 16; i++, h += 0x48) if ((*(unsigned long *)h & 0x80) && *(short *)(h + 6) == LEAVE_GAME_FUNCTION) return TRUE;
	return FALSE;
}

/* the host may end the match now: hosting, in the match for 3 s, nobody still loading */
static boolean can_end(void)
{
	return game_engine && game_in_progress() && game_time_get() >= 90 && network_game_server_hosting_all_loaded();
}

static struct widget_instance *create(char const *name, long tag_index, struct widget_instance *parent, short local)
{
	return ui_widget_load_by_name_or_tag(name, tag_index, parent, local, NONE, NONE, NONE);
}

/* the dialog's button line, as the pause menu's: "B =RESUME   A =QUIT" (Halo's button_key_sm with its texts swapped).
The alert box is 240 x 159: header bar to 28, body to 130, bottom bar to 158 */
static void add_keys(struct widget_instance *text, short local, short back_string, short select_string)
{
	struct widget_instance *panel = text->parent, *keys, *c, *last;
	if (!panel || !(keys = create("ui\\shell\\main_menu\\button_key_sm", NONE, panel, local))) return;
	/* (created as the panel's child; put last, where it draws over nothing) */
	for (last = panel->child; last && last->next; last = last->next) ;
	if (last != keys)
	{
		if (keys->previous) keys->previous->next = keys->next;
		else if (panel->child == keys) panel->child = keys->next;
		if (keys->next) keys->next->previous = keys->previous;
		for (last = panel->child; last && last->next; last = last->next) ;
		keys->next = NULL;
		keys->previous = last;
		if (last) last->next = keys;
		else panel->child = keys;
	}
	keys->horizontal_offset = (short)(text->horizontal_offset + 46);   /* (centred: B, =RESUME, A, =QUIT are 148 px) */
	keys->vertical_offset = (short)(text->vertical_offset + 134);      /* (4 px under the bar's line) */
	keys->milliseconds_to_auto_close = keys->auto_close_fade_time = 0;
	for (c = keys->child; c; c = c->next)
	{
		c->milliseconds_to_auto_close = c->auto_close_fade_time = 0;
		if (name_is(c, "=Back"))
		{
			byte *d = definition_of(c);
			c->text_box.string_list_index = back_string;
			if (*(short *)(d + 0x2a) < 72) *(short *)(d + 0x2a) = 72;   /* ("=RESUME" is 64 px; left-justified) */
		}
		else if (name_is(c, "=select")) c->text_box.string_list_index = select_string;
	}
}

static void open_dialog(short local)
{
	static char const *const names[3] =
	{
		"ui\\shell\\error\\error_nonmodal_fullscreen", "ui\\shell\\error\\error_nonmodal_halfscreen",
		"ui\\shell\\error\\error_nonmodal_qtrscreen"
	};
	struct widget_instance *dialog, *stack[24], *w, *text = NULL;
	long sp = 0, i;
	if (!can_end()) { hmp_log("game: END GAME: not now (the match is ending, or a console is still loading)"); return; }
	dialog = create(names[hmp_game_globals.size[local] == '4' ? 2 : hmp_game_globals.size[local] == '2' ? 1 : 0], NONE, NULL, local);
	if (!dialog) { hmp_log("game: END GAME: the map has no dialog to ask with"); return; }
	stack[sp++] = dialog;                            /* its text boxes, by name */
	while (sp)
	{
		w = stack[--sp];
		w->milliseconds_to_auto_close = w->auto_close_fade_time = 0;   /* (these close themselves after 5 s: not this one) */
		if (name_is(w, "error_text_box")) { w->text_box.string_list_index = _string_dialog_text; text = w; }
		else if (name_is(w, "header_error")) w->text_box.string_list_index = _string_header;
		for (w = w->child, i = 0; w && sp < 24 && i < 16; w = w->next, i++) stack[sp++] = w;
	}
	if (!text) { ui_widget_delete(dialog); hmp_log("game: END GAME: the dialog has no text box"); return; }
	add_keys(text, local, _string_key_back, _string_key_select);
	hmp_game_globals.dialog[local] = dialog;
	hmp_game_globals.dialog_text[local] = text;
	hmp_game_globals.dialog_datum[local] = dialog->definition_tag_index;
}

/* is the local player's top menu still our dialog? */
static boolean is_dialog(short local, struct widget_instance *root)
{
	struct widget_instance *dialog = hmp_game_globals.dialog[local];
	if (!dialog) return FALSE;
	if (ui_widget_get_active_widget(local) != dialog || dialog->definition_tag_index != hmp_game_globals.dialog_datum[local] ||
		hmp_game_globals.dialog_text[local]->text_box.string_list_index != _string_dialog_text)
	{
		hmp_game_globals.dialog[local] = NULL;
		return FALSE;
	}
	return root == dialog;
}

static void end_game(void)
{
	short k;
	for (k = 0; k < 4; k++) { hmp_game_globals.dialog[k] = NULL; hmp_game_globals.pending[k] = FALSE; hmp_game_globals.item[k] = 0; }
	if (!can_end()) return;
	game_engine_end_game_now();
	hmp_log("game: the host ended the match (END GAME)");
}


/* ---------- OpenCE's settings and the main menu's OPTIONS */

static void settings_load(void)
{
	char buffer[512], *line, *next;
	unsigned long got = 0;
	HANDLE file;
	if (hmp_settings.loaded) return;
	hmp_settings.loaded = TRUE;
	file = CreateFileA(SETTINGS_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE) return;
	if (!ReadFile(file, buffer, sizeof(buffer) - 1, &got, NULL)) got = 0;
	CloseHandle(file);
	buffer[got] = 0;
	for (line = buffer; line && *line; line = next)
	{
		next = strchr(line, '\n');
		if (next) *next++ = 0;
		if (!strncmp(line, "aspect=", 7))
		{
			char const *v = line + 7;
			hmp_settings.aspect = !strncmp(v, "4:3", 3) ? _aspect_standard : !strncmp(v, "16:9", 4) ? _aspect_widescreen : _aspect_dashboard;
		}
	}
}

static void settings_save(void)
{
	static char const *const aspects[NUMBER_OF_ASPECTS] = { "dashboard", "4:3", "16:9" };
	char buffer[256], *o = buffer;
	char const *a;
	unsigned long put = 0;
	HANDLE file;
	for (a = "; OpenCE's settings (Halo doesn't read this file)\r\naspect="; *a; ) *o++ = *a++;
	for (a = aspects[hmp_settings.aspect]; *a; ) *o++ = *a++;
	*o++ = '\r'; *o++ = '\n';
	file = CreateFileA(SETTINGS_PATH, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) { hmp_log("options: couldn't write " SETTINGS_PATH); return; }
	if (!WriteFile(file, buffer, (unsigned long)(o - buffer), &put, NULL) || put != (unsigned long)(o - buffer))
		hmp_log("options: couldn't write " SETTINGS_PATH " (disk full?)");
	CloseHandle(file);
}

/* (the backup of Halo's profiles and gametypes in T:\opence_backup, made the first time while the game shared Halo's
saves, is gone: the game has its own save folder now, and nothing of the player's is copied onto their drive) */

/* the screen shape: -1 as the dashboard says, 0 4:3, 1 widescreen (xbox/oxbox_sdk.c's oxbox_widescreen) */
int hmp_settings_aspect(void)
{
	settings_load();
	return hmp_settings.aspect == _aspect_standard ? 0 : hmp_settings.aspect == _aspect_widescreen ? 1 : -1;
}

static void options_text_update(void)
{
	extern int oxbox_dashboard_widescreen(void);
	static char const *const lines[NUMBER_OF_ASPECTS] = { "As the dashboard: ", "4:3", "Widescreen (16:9)" };
	wchar_t *o = hmp_game_globals.options_text;
	char const *a;
	for (a = "Screen shape:\r\n"; *a; ) *o++ = (unsigned char)*a++;
	for (a = lines[hmp_settings.aspect]; *a; ) *o++ = (unsigned char)*a++;
	if (hmp_settings.aspect == _aspect_dashboard)
		for (a = oxbox_dashboard_widescreen() ? "16:9" : "4:3"; *a; ) *o++ = (unsigned char)*a++;
	for (a = "\r\n\r\n%a-button =CHANGE    %b-button =DONE"; *a; ) *o++ = (unsigned char)*a++;
	*o = 0;
}

/* the main menu (each local player's): OPTIONS in GAME DEMOS' place, under SETTINGS (Halo hides that item when the
disc has no demos; OpenCE never launches them) */
static void main_menu_patch(short local, struct widget_instance *root)
{
	struct widget_instance *list = NULL, *c, *item = NULL;
	hmp_game_globals.main_menu[local] = root;
	hmp_game_globals.main_menu_time[local] = root->creation_time;
	hmp_game_globals.options_item[local] = 0;
	for (c = root->child; c && !list; c = c->next) if (c->type == 3) list = c;
	for (c = list ? list->child : NULL; c; c = c->next) if (name_is(c, "game_demos_item")) item = c;
	if (!item) { hmp_log("options: the main menu has no place for OPTIONS"); return; }
	item->visible = TRUE;
	item->disabled = FALSE;
	item->text_box.string_list_index = _string_options_item;
	hmp_game_globals.options_item[local] = item->definition_tag_index;
	if (hmp_game_globals.options_focus[local]) { hmp_game_globals.options_focus[local] = FALSE; list->focused_child = item; }
}

static void options_open(short local, struct widget_instance *menu, struct widget_instance *list, struct widget_instance *item)
{
	struct widget_instance *dialog, *stack[24], *w, *text = NULL;
	long sp = 0, i;
	short owner = menu->local_player_index, index = 0;
	settings_load();
	options_text_update();
	for (w = list->child; w && w != item; w = w->next) index++;
	/* (the main menu goes on the history stack, so it comes back, on OPTIONS, when the dialog closes) */
	dialog = ui_widget_load_by_name_or_tag("ui\\shell\\error\\error_nonmodal_fullscreen", NONE, NULL, owner,
		menu->definition_tag_index, list->definition_tag_index, index);
	if (!dialog) { hmp_log("options: no dialog to show them in"); return; }
	stack[sp++] = dialog;
	while (sp)
	{
		w = stack[--sp];
		w->milliseconds_to_auto_close = w->auto_close_fade_time = 0;
		if (name_is(w, "error_text_box")) { w->text_box.string_list_index = _string_options_text; text = w; }
		else if (name_is(w, "header_error")) w->text_box.string_list_index = _string_options_header;
		for (w = w->child, i = 0; w && sp < 24 && i < 16; w = w->next, i++) stack[sp++] = w;
	}
	if (!text) { ui_widget_delete(dialog); hmp_log("options: the dialog has no text box"); return; }
	hmp_game_globals.options[local] = dialog;
	hmp_game_globals.options_text_box[local] = text;
	hmp_game_globals.options_datum[local] = dialog->definition_tag_index;
}

static boolean is_options(short local, struct widget_instance *root)
{
	struct widget_instance *dialog = hmp_game_globals.options[local];
	if (!dialog) return FALSE;
	if (ui_widget_get_active_widget(local) != dialog || dialog->definition_tag_index != hmp_game_globals.options_datum[local] ||
		hmp_game_globals.options_text_box[local]->text_box.string_list_index != _string_options_text)
	{
		hmp_game_globals.options[local] = NULL;
		return FALSE;
	}
	return root == dialog;
}

static void main_menu_check(short local)
{
	struct widget_instance *root = ui_widget_get_active_widget(local);
	if (game_engine || !root || !name_is(root, "the_main_menu")) return;
	if (root == hmp_game_globals.main_menu[local] && root->creation_time == hmp_game_globals.main_menu_time[local]) return;
	main_menu_patch(local, root);
}


/* ---------- the system link lobby's SELECT GAMETYPE: X edits the gametype, Y makes a new one from it (Halo's own
gametype editor, opened over this screen, which comes back when it closes: the game stays up meanwhile) */

#define GAMETYPE_EDIT_SCREEN "ui\\shell\\main_menu\\settings_select\\multiplayer_setup\\playlist_edit\\gametype_edit_screen"

boolean playlist_profile_get(long playlist_profile_index, struct game_variant *variant);
long playlist_profile_new(short local_player_index, wchar_t *name);
void playlist_profile_save(long playlist_profile_index, struct game_variant *variant);
void player_ui_begin_editing_profile(long profile_index);
void player_ui_end_editing_profile(void);
struct game_variant *player_ui_get_edit_playlist_profile(void);
void saved_game_file_get_useable_untitled_profile_name(wchar_t *display_name);
boolean saved_game_file_get_path_to_enclosing_directory(long profile_index, char *directory_path);
void saved_game_file_remember_last_used_multiplayer_variant_directory(char const *directory_path);

struct hmp_spinner_list                  /* (ui_widget.c's spinner list instance, as the gametype handlers read it) */
{
	byte header[0x3c];
	unsigned short selected_index;
	short pad3e;
	long *generated_list;
	unsigned short generated_count;
};

/* the lobby's gametype list (the wrapper's screen's first child), or NULL */
struct widget_instance *hmp_gametypes_list(struct widget_instance *widget)
{
	struct widget_instance *screen;
	if (!widget || !name_is(widget, "gametype_select_screen_wrapper") || !(screen = widget->child) || !screen->child ||
		screen->child->type != 2) return NULL;
	return screen->child;
}

/* is this spinner list the lobby's gametype list (for its button line, hmp_browse.c) */
boolean hmp_gametypes_list_is(struct widget_instance *list)
{
	return list && list->parent && list->parent->parent && hmp_gametypes_list(list->parent->parent) == list;
}

static long gametypes_selected(struct widget_instance *list)
{
	struct hmp_spinner_list *l = (struct hmp_spinner_list *)list;
	if (!l->generated_list || l->selected_index >= l->generated_count) return NONE;
	return l->generated_list[l->selected_index];
}

static void gametypes_open_editor(struct widget_instance *root, struct widget_instance *list, long profile_index)
{
	char directory[256];
	struct widget_instance *editor;
	if (saved_game_file_get_path_to_enclosing_directory(profile_index, directory))
		saved_game_file_remember_last_used_multiplayer_variant_directory(directory);   /* (the list comes back on it) */
	player_ui_begin_editing_profile(profile_index);
	editor = ui_widget_load_by_name_or_tag(GAMETYPE_EDIT_SCREEN, NONE, NULL, root->local_player_index,
		root->definition_tag_index, NONE, NONE);   /* (no focus kept: the list comes back on the remembered gametype) */
	if (!editor)
	{
		player_ui_end_editing_profile();
		ui_play_audio_feedback_sound(4);
		hmp_log("gametypes: the gametype editor isn't in this map");
		return;
	}
	ui_play_audio_feedback_sound(1);
}

/* Y: a new gametype, a copy of the selected one, saved at once as "New00x", then edited */
static void gametypes_new(struct widget_instance *root, struct widget_instance *list)
{
	struct game_variant variant;
	wchar_t name[128];
	long selected = gametypes_selected(list), profile_index;
	short k;
	if (selected == NONE || !playlist_profile_get(selected, &variant)) { ui_play_audio_feedback_sound(4); return; }
	saved_game_file_get_useable_untitled_profile_name(name);
	if (!name[0] || (profile_index = playlist_profile_new(root->local_player_index, name)) == NONE)
	{
		ui_play_audio_feedback_sound(4);
		hmp_log("gametypes: couldn't make a new gametype (the disk is full, or there are too many)");
		return;
	}
	for (k = 0; k < 11 && name[k]; k++) variant.human_readable_game_description[k] = name[k];
	for (; k < 12; k++) variant.human_readable_game_description[k] = 0;
	*(short *)((byte *)&variant + 0x64) = 0;          /* (not a built-in: as Halo's own "create new" leaves it) */
	playlist_profile_save(profile_index, &variant);
	hmp_log("gametypes: a new gametype made in the lobby");
	gametypes_open_editor(root, list, profile_index);
}

static long gametypes_dispatch(struct widget_instance *root, struct event_record *event)
{
	struct widget_instance *list = hmp_gametypes_list(root);
	short b = event->data.button.index;
	long selected;
	if (!list || (b != 2 && b != 3)) return 0;
	if (event->data.button.value != 1) return 1;
	if (b == 3) { gametypes_new(root, list); return 1; }
	selected = gametypes_selected(list);
	if (selected == NONE) { ui_play_audio_feedback_sound(4); return 1; }
	gametypes_open_editor(root, list, selected);      /* (a built-in one asks for a new name when it's saved, as Halo does) */
	return 1;
}

/* a local player's top menu is about to get an event: 0 not ours, 1 taken, 2 taken and the root deleted */
long hmp_options_dispatch(struct widget_instance *root, struct event_record *event)
{
	short local;
	boolean press;
#if defined(OXBOX_NET_P2P)
	hmp_online_event(root, event);   /* (the A that joins a game listed online once its host is reached) */
#endif
	if (!root || event->type != 3) return 0;
	for (local = 0; local < 4 && ui_widget_get_active_widget(local) != root; local++) ;   /* (the main menu is anyone's: its slot) */
	if (local > 3) return 0;
#if defined(OXBOX_NET_P2P)
	if (hmp_online_dispatch(root, event)) return 1;   /* (SYSTEM LINK PLAY or ONLINE PLAY; the games list's scrolling) */
#endif
	if (gametypes_dispatch(root, event)) return 1;
	press = event->data.button.value == 1;
	if (is_options(local, root))
	{
		short b = event->data.button.index;
		if (b == 0 || b == 10 || b == 11)            /* A, d-pad right: the next shape; d-pad left: the one before */
		{
			if (press)
			{
				hmp_settings.aspect = (hmp_settings.aspect + (b == 10 ? NUMBER_OF_ASPECTS - 1 : 1)) % NUMBER_OF_ASPECTS;
				options_text_update();
				ui_play_audio_feedback_sound(1);
			}
			return 1;
		}
		if (b == 1 || b == 13 || b == 12)            /* B, Back, Start: done, saved */
		{
			if (!press) return 1;
			hmp_game_globals.options[local] = NULL;
			hmp_game_globals.options_focus[local] = TRUE;
			settings_save();
			ui_widget_delete(root);
			ui_play_audio_feedback_sound(2);
			return 2;
		}
		return 1;
	}
	if (press && event->data.button.index == 0 && root == hmp_game_globals.main_menu[local] && hmp_game_globals.options_item[local])
	{
		struct widget_instance *list = root->focused_child, *item = list ? list->focused_child : NULL;
		if (item && item->definition_tag_index == hmp_game_globals.options_item[local] && item->text_box.string_list_index == _string_options_item)
		{
			ui_play_audio_feedback_sound(1);
			options_open(local, root, list, item);
			return 1;
		}
	}
	return 0;
}

/* ---------- public code */

wchar_t *hmp_game_string(short index)
{
	wchar_t *d;
	char const *a;
	if (index == _string_options_text) return hmp_game_globals.options_text;
#if defined(OXBOX_NET_P2P)
	if (index >= HMP_ONLINE_STRING_BASE && index < HMP_ONLINE_STRING_BASE + 0x10) return hmp_online_string(index);
#endif
	if (index < _string_item || index >= _string_item + NUMBER_OF_GAME_STRINGS) return NULL;
	d = hmp_game_globals.strings[index - _string_item];
	if (!d[0]) for (a = game_strings[index - _string_item]; (*d++ = (unsigned char)*a++) != 0; ) ;
	return hmp_game_globals.strings[index - _string_item];
}

/* the network pause menu was made for a local player (size: '1', '2' or '4', from its name) */
void hmp_game_pause_menu_opened(struct widget_instance *root, short local)
{
	struct widget_instance *list = NULL, *c, *leave = NULL, *first, *item, *bar = NULL, *box = NULL;
	char const *n;
	short step, h;
	if (local < 0 || local > 3 || !root) return;
	hmp_game_globals.item[local] = 0;
	for (n = root->name ? root->name : ""; *n && !(n[0] >= '1' && n[0] <= '4' && n[1] == 'p' && n[2] == '_'); n++) ;
	hmp_game_globals.size[local] = *n ? *n : '1';
	if (!can_end()) return;
	for (c = root->child; c; c = c->next)
	{
		if (c->type == 3 && !list) list = c;          /* the column list */
		if (name_starts(c, "button_key")) bar = c;   /* the "B Back  A select" line */
		if (name_is(c, "pause_dialog_bkd")) box = c;  /* the box's frame */
	}
	if (!list || !(first = list->child)) return;
	for (c = first; c; c = c->next) if (c->type == 1 && is_leave_item(c)) leave = c;
	if (!leave || leave == first) return;
	item = create(NULL, leave->definition_tag_index & 0xffff, list, local);
	if (!item) return;
	/* (made as the list's child; moved to just after LEAVE GAME) */
	if (item->previous) item->previous->next = item->next;
	else if (list->child == item) list->child = item->next;
	if (item->next) item->next->previous = item->previous;
	item->previous = leave;
	item->next = leave->next;
	if (leave->next) leave->next->previous = item;
	leave->next = item;
	step = (short)(leave->vertical_offset - (leave->previous ? leave->previous : first)->vertical_offset);
	if (step <= 0 || step > 80) step = 35;
	item->horizontal_offset = leave->horizontal_offset;
	item->vertical_offset = (short)(leave->vertical_offset + step);
	item->text_box.string_list_index = _string_item;
	h = bounds_height(leave);
	if (box && bar)                                  /* the rows centred between the box's top and the line over the buttons */
	{
		short top = box->vertical_offset, line = (short)(bar->vertical_offset - 4);
		short block = (short)(item->vertical_offset - first->vertical_offset + h);
		short y = (short)(top + (line - top - block) / 2 - first->vertical_offset);
		if (y > top && y < list->vertical_offset) list->vertical_offset = y;
	}
	else                                             /* (another pause menu: half a row up, never over the button line) */
	{
		list->vertical_offset -= step / 2;
		if (bar)
		{
			short bottom = (short)(list->vertical_offset + item->vertical_offset + h + 2);
			if (bottom > bar->vertical_offset && bottom - bar->vertical_offset < 40) list->vertical_offset -= bottom - bar->vertical_offset;
		}
	}
	hmp_game_globals.item[local] = item->definition_tag_index;
	hmp_log("game: END GAME added to the host's pause menu");
}

/* LEAVE GAME's A: TRUE when it was our END GAME item (the dialog opens after this event) */
boolean hmp_game_handles_leave(struct widget_instance *w)
{
	short local = w ? w->local_player_index : NONE;
	if (local < 0 || local > 3 || !hmp_game_globals.item[local] || w->definition_tag_index != hmp_game_globals.item[local]) return FALSE;
	hmp_game_globals.pending[local] = TRUE;
	return TRUE;
}

/* a local player's top menu is about to get an event: TRUE when the dialog took it (the root is deleted) */
boolean hmp_game_dispatch(struct widget_instance *root, struct event_record *event)
{
	short local = root ? root->local_player_index : NONE;
	if (local < 0 || local > 3 || !is_dialog(local, root)) return FALSE;
	if (event->type == 3 && event->data.button.value == 1 &&
		(event->data.button.index == 0 || event->data.button.index == 1 || event->data.button.index == 13))   /* A, B, Back */
	{
		hmp_game_globals.dialog[local] = NULL;
		ui_widget_delete(root);
		if (event->data.button.index == 0) end_game();
		return TRUE;
	}
	return FALSE;
}

void hmp_game_after_dispatch(struct widget_instance *root)
{
	short local;
	for (local = 0; local < 4; local++)
	{
		if (hmp_game_globals.pending[local]) { hmp_game_globals.pending[local] = FALSE; open_dialog(local); }
	}
	(void)root;
}

void hmp_game_tick(void)
{
	short k;
	for (k = 0; k < 4; k++) { is_dialog(k, NULL); is_options(k, NULL); main_menu_check(k); }
#if defined(OXBOX_NET_P2P)
	for (k = 0; k < 4; k++) if (!game_engine) hmp_online_tick(k);   /* (ONLINE PLAY in the Multiplayer menu) */
#endif
}

#endif /* HALO_XBOX_TARGET */
