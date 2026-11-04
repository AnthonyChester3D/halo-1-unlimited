/*
OXBOX_MENUS.C

Halo 1 Unlimited's own Xbox menu screens: widget tags built when ui.map has loaded (cache_files.c calls
menu_tags_loaded, oxbox_sdk.c's, which loads this unit's XBE section, OXMENU, for as long as ui.map is loaded: no
game carries its code) and added to its tag table, then run by the game's widgets as Bungie's are. ui.map itself is
never changed: what's built is let go when it unloads (menu_tags_unloaded), and the next load builds it again.
The PC build has its own (port/linux/game/menu_tags.c, from XML); this one is the Xbox's, made from the player's
own ui.map's widgets: each new widget is a copy of one of Bungie's (its look: font, colour, size), with our text,
so nothing of Bungie's is in the build.

VEHICLE OPTIONS, on the gametype editor's list (Settings > Multiplayer Setup > a gametype), before SAVE CHANGES:
its screen is laid out as ITEM OPTIONS' is (a copy of its rows), with
	SHARED VEHICLES     YES / NO                          (game_variant_options' shared_vehicles)
	LIGHT VEHICLES ... TURRETS: ALL, NONE, 1, 2, 3, 4, 6, 8 (its vehicle_categories: game_engine.c)
Its A or START keeps what's chosen in the gametype being edited (player_ui's edit options), which SAVE CHANGES
writes with the rest.

OPTIONS, the main menu's (hmp_game.c opens it in place of its old dialog, and keeps the settings): SCREEN SHAPE,
ATTRACT MODE (the showcase, Halo's movies, off), ATTRACT MODE WAIT and CHECK FOR UPDATES.

Both are laid out as ITEM OPTIONS is (copies of its rows, its help box under them: a line for each row's values),
with a header picture made by the installer from Halo's own header letters (oxbox_entry.c), or a text title.

Anything it can't find (a ui.map unlike the ones it knows) is logged, and nothing is added: the menus stay
Bungie's.
*/

#include "cseries.h"
#include "bitmaps/bitmap_group.h"
#include "game/game_engine.h"
#include "interface/player_ui.h"
#include "tag_files/tag_groups.h"
#include "text/text_group.h"

#include <stdlib.h>
#include <string.h>

void platform_log(char const *format, ...);
void *cache_files_tag_instances(long *count);
void cache_files_set_tag_instances(void *instances, long count);
char *tag_get_name(long tag_index);
int oxbox_xbe_menu_header_present(int which);   /* (oxbox_entry.c: the installer's pictures: 0 VEHICLE OPTIONS, 1 OPTIONS) */
int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);
#define text_print halo_linux_snprintf
/* (hmp_game.c's: the main menu's OPTIONS, OPTIONS_ROWS values) */
void hmp_options_get(short *values);
void hmp_options_set(short const *values);

/* ---------- constants */

#define MENU_FUNCTION_BASE 256              /* (halo_menus.h's PC_MENU_FUNCTION_BASE) */
#define TAG_PREFIX "h1u\\"
#define DEFINITION_SIZE 0x3EC
#define WIDGET_TAG 'DeLa'
#define STRINGS_TAG 'ustr'

#define SETUP "ui\\shell\\main_menu\\settings_select\\multiplayer_setup\\"
#define EDIT_LIST SETUP "playlist_edit\\edit_playlist_select_list"
#define EDIT_ROW SETUP "playlist_edit\\playlist_edit_indicators_item"
#define EDIT_SCREEN SETUP "playlist_edit\\gametype_edit_screen"
#define HEADER_FONT "ui\\interstate"
#define EDIT_DESCRIPTION_TEXT SETUP "playlist_edit\\playlist_edit_ext_desc_text"
#define ITEMS_SCREEN SETUP "item_options_edit\\item_options_screen"
#define ITEMS_LIST SETUP "item_options_edit\\item_options_menu"
#define ITEMS_HELP SETUP "item_options_edit\\item_options_help"
#define ITEMS_HEADER_BITMAP SETUP "item_options_edit\\header_item_options"
#define ITEMS_HEADER SETUP "item_options_edit\\header_item_options"
#define ITEMS_YES_NO_ROW SETUP "item_options_edit\\op_infinite_grenades"
#define ITEMS_WIDE_ROW SETUP "item_options_edit\\op_vehicle_set"
#define ITEMS_WIDEST_ROW SETUP "item_options_edit\\op_weapon_set"

enum
{
	/* our functions (MENU_FUNCTION_BASE + these) */
	_menu_function_vehicle_options_load = 0,
	_menu_function_vehicle_options_save,
	_menu_function_options_load,
	_menu_function_options_save,

	/* the rows our screens copy: ITEM OPTIONS' INFINITE GRENADES (YES / NO), VEHICLE SET and WEAPON SET (wider
	spinners) */
	_row_model_yes_no = 0,
	_row_model_wide,
	_row_model_widest,
	NUMBER_OF_ROW_MODELS,

	_event_handler_open_widget_bit = 3,
	_event_handler_run_function_bit = 7,
	_event_handler_go_back_bit = 9,

	_event_a_button = 0,
	_event_start_button = 12,
	_event_created = 24,

	_widget_type_container = 0,
	_widget_type_text_box,
	_widget_type_spinner_list,

	VEHICLE_OPTION_ROWS = 1 + NUMBER_OF_VEHICLE_CATEGORIES,
	OPTIONS_ROWS = 4,
	MAXIMUM_ROWS = 8,
	ROW_STEP = 30,
	EDIT_ROW_STEP = 33,
	/* the tags it adds, at most: each screen's screen, header, picture, list, help, help's, labels' and title's
	string lists, and each row's container, label, spinner and values; the editor's row and its strings */
	MAXIMUM_ADDED_TAGS = 2 * 8 + 4 * (VEHICLE_OPTION_ROWS + OPTIONS_ROWS) + 3,
	/* the help: a line for each row's each value, in order (the game picks it: game_options_menu_update_text_desc) */
	VEHICLE_HELP_LINES = 2 + NUMBER_OF_VEHICLE_CATEGORIES * NUMBER_OF_VEHICLE_CATEGORY_COUNTS,
	MAXIMUM_HELP = 64,
	HELP_SIZE = 128,
	MAXIMUM_BLOCKS = 640,
};

/* ---------- structures (ui_widget.c's, as menu_tags.c has them) */

struct cache_file_tag_instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

struct ui_widget_event_handler_reference
{
	long flags;
	short event_type;
	short function;
	struct tag_reference widget_tag;
	struct tag_reference sound_effect;
	char script[32];
};

struct ui_widget_child_reference
{
	struct tag_reference widget_tag;
	char name[32];
	long flags;
	short custom_controller_index;
	short vertical_offset;
	short horizontal_offset;
	byte unknown03A[0x50 - 0x3A];
};

struct ui_widget_definition
{
	short type;
	short controller_index;
	char name[32];
	rectangle2d bounds;
	long flags;
	long milliseconds_to_auto_close;
	long auto_close_fade_time;
	struct tag_reference background_bitmap;
	struct tag_block game_data_inputs;
	struct tag_block event_handlers;
	struct tag_block search_and_replace_functions;
	byte unknown06C[0xEC - 0x6C];
	struct tag_reference text_label_string_list;
	struct tag_reference text_font;
	real_argb_color text_color;
	short justification;
	word text_box_flags;
	byte unknown120[0x12E - 0x120];
	short string_list_index;
	short horizontal_offset;
	short vertical_offset;
	byte unknown134[0x150 - 0x134];
	long list_flags;
	struct tag_reference list_header_bitmap;
	struct tag_reference list_footer_bitmap;
	rectangle2d list_header_bounds;
	rectangle2d list_footer_bounds;
	byte unknown184[0x1A4 - 0x184];
	struct tag_reference extended_description_widget;
	byte unknown1B4[0x2D4 - 0x1B4];
	struct tag_block conditional_widgets;
	byte unknown2E0[0x3E0 - 0x2E0];
	struct tag_block child_widgets;
};

#define widget_get(index) ((struct ui_widget_definition *)tag_get(WIDGET_TAG, (index)))

typedef char verify_oxbox_menus_definition_size[sizeof(struct ui_widget_definition) == DEFINITION_SIZE ? 1 : -1];
typedef char verify_oxbox_menus_handler_size[sizeof(struct ui_widget_event_handler_reference) == 0x48 ? 1 : -1];
typedef char verify_oxbox_menus_child_size[sizeof(struct ui_widget_child_reference) == 0x50 ? 1 : -1];
typedef char verify_oxbox_menus_instance_size[sizeof(struct cache_file_tag_instance) == 0x20 ? 1 : -1];

/* a widget's instance, as far as a spinner's choice (ui_widget_event_handler_functions.c's) */
struct menu_widget_instance
{
	long definition_tag_index;
	char const *name;
	short local_player_index;
	byte unknown0A[4];
	short type;
	byte unknown10[0x2C - 0x10];
	struct menu_widget_instance *next;
	struct menu_widget_instance *parent;
	struct menu_widget_instance *child;
	struct menu_widget_instance *focused_child;
	short selected_index;
};

/* ---------- globals */

static struct
{
	struct cache_file_tag_instance *original_instances;
	long original_count;
	long first_index, first_salt, next, count;
	long edit_list_index;                    /* (the editor's list, whose row it added: NONE when not) */
	long edit_row_position;                  /* (where in that list) */
	boolean failed;
	void *blocks[MAXIMUM_BLOCKS];
	long block_count;
} oxbox_menus = { NULL, 0, 0, 0, 0, 0, NONE, NONE };

/* ---------- private code */

static void *allocate(long size)
{
	void *block;

	if (oxbox_menus.failed || oxbox_menus.block_count >= MAXIMUM_BLOCKS || !(block = malloc(size > 0 ? size : 1)))
	{
		if (!oxbox_menus.failed)
			platform_log("menus: out of memory (or blocks: %ld): Halo 1 Unlimited's screens aren't added",
				oxbox_menus.block_count);
		oxbox_menus.failed = TRUE;
		return NULL;
	}
	memset(block, 0, size > 0 ? size : 1);
	oxbox_menus.blocks[oxbox_menus.block_count++] = block;
	return block;
}

/* (restore: the map's own table back, when it's still loaded) */
static void release(boolean restore)
{
	long index;

	if (restore && oxbox_menus.original_instances)
		cache_files_set_tag_instances(oxbox_menus.original_instances, oxbox_menus.original_count);
	/* (the game's free takes no NULL) */
	for (index = 0; index < oxbox_menus.block_count; index++)
		free(oxbox_menus.blocks[index]);
	memset(&oxbox_menus, 0, sizeof(oxbox_menus));
	oxbox_menus.edit_list_index = NONE;
	oxbox_menus.edit_row_position = NONE;
}

static long found(long group_tag, char const *name)
{
	long tag_index = tag_loaded(group_tag, name);

	if (tag_index == NONE)
	{
		platform_log("menus: ui.map has no %s: Halo 1 Unlimited's screens aren't added", name);
		oxbox_menus.failed = TRUE;
	}
	return tag_index;
}

static void reference_set(struct tag_reference *reference, long group_tag, long tag_index)
{
	reference->group_tag = group_tag;
	reference->name = tag_index == NONE ? (char *)"" : tag_get_name(tag_index);
	reference->name_length = (long)strlen(reference->name);
	reference->index = tag_index;
}

static long next_tag(void)
{
	long tag_index = ((oxbox_menus.first_salt + oxbox_menus.next) << 16) | (oxbox_menus.first_index + oxbox_menus.next);

	oxbox_menus.next++;
	return tag_index;
}

/* a tag of ours, named h1u\<name>, in the grown table */
static void instance_set(struct cache_file_tag_instance *instances, long group_tag, long tag_index, char const *name,
	void *definition)
{
	struct cache_file_tag_instance *instance = &instances[tag_index & 0xFFFF];
	char *copy = allocate((long)strlen(TAG_PREFIX) + (long)strlen(name) + 1);

	if (copy)
	{
		strcpy(copy, TAG_PREFIX);
		strcat(copy, name);
	}
	instance->group_tag = group_tag;
	instance->parent_group_tags[0] = NONE;
	instance->parent_group_tags[1] = NONE;
	instance->tag_index = tag_index;
	instance->name = copy ? copy : "";
	instance->base_address = definition;
}

/* a unicode string list of ASCII strings (and, first, count of an existing list's, copied, with ours put at
insert) */
static struct string_list *strings_build(char const *const *strings, long count, long copy_from, long insert)
{
	struct string_list *existing = copy_from == NONE ? NULL : unicode_string_list_definition_get(copy_from);
	long existing_count = existing ? existing->strings.count : 0;
	struct string_list *list = allocate(sizeof(struct string_list));
	struct string_list_entry *entries = allocate((existing_count + count) * sizeof(struct string_list_entry));
	long index, at = 0;

	if (!list || !entries)
		return list;
	list->strings.count = existing_count + count;
	list->strings.address = entries;
	for (index = 0; index < existing_count + count; index++)
	{
		struct string_list_entry *entry = &entries[index];
		long own = existing ? index - insert : index;

		if (existing && (own < 0 || own >= count))
		{
			/* (the map's own string, as it is) */
			*entry = ((struct string_list_entry *)existing->strings.address)[at++];
			continue;
		}
		{
			long length = (long)strlen(strings[own]), k;
			unsigned short *characters = allocate((length + 1) * sizeof(unsigned short));

			if (!characters)
				return list;
			for (k = 0; k <= length; k++)
				characters[k] = (unsigned char)strings[own][k];
			entry->string.size = (length + 1) * (long)sizeof(unsigned short);
			entry->string.address = characters;
		}
	}
	return list;
}

/* text joined: up to 8 pieces (NULL ends them early), into out (HELP_SIZE) */
static char *joined(char *out, char const *a, char const *b, char const *c, char const *d, char const *e, char const *f,
	char const *g, char const *h)
{
	char const *pieces[8];
	long n = 0, k;

	pieces[0] = a, pieces[1] = b, pieces[2] = c, pieces[3] = d, pieces[4] = e, pieces[5] = f, pieces[6] = g;
	pieces[7] = h;
	for (k = 0; k < 8 && pieces[k]; k++)
	{
		char const *s = pieces[k];

		while (*s && n < HELP_SIZE - 1)
			out[n++] = *s++;
	}
	out[n] = 0;
	return out;
}

/* a copy of one of the map's widgets, its blocks its own (children: child_count of them, from the model's, the
rest zeroed) */
static struct ui_widget_definition *widget_copy(long model_index, long child_count)
{
	struct ui_widget_definition const *model = widget_get(model_index);
	struct ui_widget_definition *copy = allocate(DEFINITION_SIZE);
	struct ui_widget_child_reference *children;

	if (!copy)
		return NULL;
	memcpy(copy, model, DEFINITION_SIZE);
	children = child_count ? allocate(child_count * sizeof(*children)) : NULL;
	if (child_count && !children)
		return copy;
	if (children)
		memcpy(children, model->child_widgets.address, MIN(child_count, model->child_widgets.count) * sizeof(*children));
	copy->child_widgets.count = child_count;
	copy->child_widgets.address = children;
	return copy;
}

/* its event handlers: count of them, each (when event isn't NONE) the event, flags, function and widget given */
static void handlers_set(struct ui_widget_definition *definition, long count, short const *events, long const *flags,
	short const *functions, long const *widgets)
{
	struct ui_widget_event_handler_reference *handlers = count ? allocate(count * sizeof(*handlers)) : NULL;
	long index;

	definition->event_handlers.count = handlers ? count : 0;
	definition->event_handlers.address = handlers;
	for (index = 0; handlers && index < count; index++)
	{
		handlers[index].event_type = events[index];
		handlers[index].flags = flags[index];
		handlers[index].function = functions[index];
		reference_set(&handlers[index].widget_tag, WIDGET_TAG, widgets[index]);
		reference_set(&handlers[index].sound_effect, 'snd!', NONE);
	}
}

static void child_set(struct ui_widget_definition *definition, long index, long widget_index, short x, short y)
{
	struct ui_widget_child_reference *child = (struct ui_widget_child_reference *)definition->child_widgets.address;

	if (!child || index >= definition->child_widgets.count)
		return;
	child += index;
	reference_set(&child->widget_tag, WIDGET_TAG, widget_index);
	child->horizontal_offset = x;
	child->vertical_offset = y;
}

/* the model row's label and spinner (its children's widgets) */
static boolean row_parts(long row_index, long *label_index, long *spinner_index, short *spinner_x, short *spinner_y)
{
	struct ui_widget_definition const *row = widget_get(row_index);
	struct ui_widget_child_reference const *children = (struct ui_widget_child_reference const *)row->child_widgets.address;
	long index;

	*label_index = *spinner_index = NONE;
	for (index = 0; index < row->child_widgets.count; index++)
	{
		long child_index = children[index].widget_tag.index;

		if (child_index == NONE)
			continue;
		if (widget_get(child_index)->type == _widget_type_spinner_list)
		{
			*spinner_index = child_index;
			*spinner_x = children[index].horizontal_offset;
			*spinner_y = children[index].vertical_offset;
		}
		else
			*label_index = child_index;
	}
	return *label_index != NONE && *spinner_index != NONE;
}

/* the editor's list's child that opens the model row's screen: its position, or NONE */
static long child_position(struct ui_widget_definition const *list, long widget_index)
{
	struct ui_widget_child_reference const *children = (struct ui_widget_child_reference const *)list->child_widgets.address;
	long index;

	for (index = 0; index < list->child_widgets.count; index++)
	{
		if (children[index].widget_tag.index == widget_index)
			return index;
	}
	return NONE;
}

/* ITEM OPTIONS' widgets, which our screens are copies of */
struct menu_models
{
	long screen, list, header, help, header_bitmap;
	long rows[NUMBER_OF_ROW_MODELS], labels[NUMBER_OF_ROW_MODELS], spinners[NUMBER_OF_ROW_MODELS];
	short spinner_x[NUMBER_OF_ROW_MODELS], spinner_y[NUMBER_OF_ROW_MODELS];
};

/* a row of a screen: its label, the model it's a copy of (its spinner's width), its values, and a line of help for
each value */
struct menu_row
{
	char const *label;
	short model, value_count;
	char const *const *values;
	char const **help;
};

/* a screen: its name (h1u\<name>_screen), its title, its header picture (oxbox_entry.c's: NONE for none), its rows,
and our functions that show the values (on its list's creation) and keep them (A or START, then back) */
struct menu_screen
{
	char const *name, *title;
	int picture;
	short row_count;
	struct menu_row const *rows;
	short load_function, save_function;
};

static long screen_build(struct cache_file_tag_instance *grown, struct menu_models const *models,
	struct menu_screen const *spec)
{
	char name[64];
	long screen_tag = next_tag(), header_tag = next_tag(), picture_tag = next_tag(), list_tag = next_tag();
	long help_tag = next_tag(), help_strings_tag = next_tag(), labels_tag = next_tag(), title_tag = next_tag();
	long row_tags[MAXIMUM_ROWS], label_tags[MAXIMUM_ROWS], spinner_tags[MAXIMUM_ROWS], values_tags[MAXIMUM_ROWS];
	char const *labels[MAXIMUM_ROWS], *help[MAXIMUM_HELP];
	struct ui_widget_definition *definition, *screen, *list;
	struct ui_widget_definition const *model;
	boolean picture = spec->picture != NONE && oxbox_xbe_menu_header_present(spec->picture);
	long index, value, help_count = 0;

	for (index = 0; index < spec->row_count && index < MAXIMUM_ROWS; index++)
	{
		row_tags[index] = next_tag();
		label_tags[index] = next_tag();
		spinner_tags[index] = next_tag();
		values_tags[index] = next_tag();
		labels[index] = spec->rows[index].label;
		for (value = 0; value < spec->rows[index].value_count && help_count < MAXIMUM_HELP; value++)
			help[help_count++] = spec->rows[index].help[value];
	}

	/* (its string lists: the labels, the title, the help, each row's values) */
	text_print(name, sizeof(name), "%s_labels", spec->name);
	instance_set(grown, STRINGS_TAG, labels_tag, name, strings_build(labels, spec->row_count, NONE, 0));
	text_print(name, sizeof(name), "%s_title", spec->name);
	instance_set(grown, STRINGS_TAG, title_tag, name, strings_build(&spec->title, 1, NONE, 0));
	text_print(name, sizeof(name), "%s_help", spec->name);
	instance_set(grown, STRINGS_TAG, help_strings_tag, name, strings_build(help, help_count, NONE, 0));
	for (index = 0; index < spec->row_count; index++)
	{
		text_print(name, sizeof(name), "%s_values_%ld", spec->name, index);
		instance_set(grown, STRINGS_TAG, values_tags[index], name,
			strings_build(spec->rows[index].values, spec->rows[index].value_count, NONE, 0));
	}

	/* (the screen: ITEM OPTIONS' children, its header and list ours) */
	model = widget_get(models->screen);
	screen = widget_copy(models->screen, model->child_widgets.count);
	if (screen)
	{
		struct ui_widget_child_reference *children = (struct ui_widget_child_reference *)screen->child_widgets.address;

		text_print(screen->name, sizeof(screen->name), "%s_screen", spec->name);
		for (index = 0; children && index < screen->child_widgets.count; index++)
		{
			if (children[index].widget_tag.index == models->header)
				reference_set(&children[index].widget_tag, WIDGET_TAG, header_tag);
			else if (children[index].widget_tag.index == models->list)
				reference_set(&children[index].widget_tag, WIDGET_TAG, list_tag);
		}
	}
	text_print(name, sizeof(name), "%s_screen", spec->name);
	instance_set(grown, WIDGET_TAG, screen_tag, name, screen);

	/* (its header: ITEM OPTIONS' with our picture, the installer's (made of Halo's letters, as ONLINE GAMES is), a copy
	of ITEM OPTIONS' bitmap that reads it (hmp_maps.c's menu_word_read: by its name, h1u\<name>_header); with none in
	the XBE, a label's look and the title's text) */
	text_print(name, sizeof(name), "%s_header", spec->name);
	if (picture)
	{
		struct bitmap_group *group = allocate(sizeof(struct bitmap_group));
		struct bitmap_data *bitmap = allocate(sizeof(struct bitmap_data));
		struct bitmap_group const *model_group = (struct bitmap_group const *)tag_get('bitm', models->header_bitmap);

		if (group && bitmap && model_group->bitmaps.count >= 1)
		{
			memcpy(group, model_group, sizeof(*group));
			memcpy(bitmap, model_group->bitmaps.address, sizeof(*bitmap));
			bitmap->tag_index = picture_tag;
			bitmap->cache_block_index = NONE;
			bitmap->hardware_format = NULL;
			bitmap->base_address = NULL;
			group->bitmaps.count = 1;
			group->bitmaps.address = bitmap;
		}
		instance_set(grown, 'bitm', picture_tag, name, group);
		definition = widget_copy(models->header, 0);
		if (definition)
		{
			text_print(definition->name, sizeof(definition->name), "%s_title", spec->name);
			reference_set(&definition->background_bitmap, 'bitm', picture_tag);
			definition->bounds.x1 += 100;      /* (a title may be longer than ITEM OPTIONS) */
		}
	}
	else
	{
		/* (a placeholder in the table: a bitmap group of no bitmaps) */
		instance_set(grown, 'bitm', picture_tag, name, allocate(sizeof(struct bitmap_group)));
		definition = widget_copy(models->labels[_row_model_yes_no], 0);
		if (definition)
		{
			rectangle2d header_bounds = widget_get(models->header)->bounds;

			text_print(definition->name, sizeof(definition->name), "%s_title", spec->name);
			definition->bounds.y0 = header_bounds.y0 + 14;
			definition->bounds.x0 = header_bounds.x0 + 14;
			definition->bounds.y1 = definition->bounds.y0 + 40;
			definition->bounds.x1 = header_bounds.x1 + 250;
			reference_set(&definition->text_label_string_list, STRINGS_TAG, title_tag);
			definition->string_list_index = 0;
		}
	}
	text_print(name, sizeof(name), "%s_title", spec->name);
	instance_set(grown, WIDGET_TAG, header_tag, name, definition);

	/* (its list: ITEM OPTIONS', our rows, its help ours, and our functions to show and keep the values) */
	list = widget_copy(models->list, spec->row_count);
	if (list)
	{
		static short const events[3] = { _event_created, _event_a_button, _event_start_button };
		struct ui_widget_child_reference const *first =
			(struct ui_widget_child_reference const *)widget_get(models->list)->child_widgets.address;
		long flags[3], widgets[3] = { NONE, NONE, NONE };
		short functions[3];

		flags[0] = 1L << _event_handler_run_function_bit;
		flags[1] = flags[2] = (1L << _event_handler_run_function_bit) | (1L << _event_handler_go_back_bit);
		functions[0] = MENU_FUNCTION_BASE + spec->load_function;
		functions[1] = functions[2] = MENU_FUNCTION_BASE + spec->save_function;
		text_print(list->name, sizeof(list->name), "%s_list", spec->name);
		/* (its help: ITEM OPTIONS' text box, our lines; the list's game data input picks the line, as ITEM OPTIONS'
		does: the rows' values in order) */
		reference_set(&list->extended_description_widget, WIDGET_TAG, help_tag);
		handlers_set(list, 3, events, flags, functions, widgets);
		for (index = 0; index < spec->row_count; index++)
			child_set(list, index, row_tags[index], first->horizontal_offset,
				(short)(first->vertical_offset + index * ROW_STEP));
	}
	text_print(name, sizeof(name), "%s_list", spec->name);
	instance_set(grown, WIDGET_TAG, list_tag, name, list);

	definition = widget_copy(models->help, 0);
	if (definition)
	{
		text_print(definition->name, sizeof(definition->name), "%s_help", spec->name);
		reference_set(&definition->text_label_string_list, STRINGS_TAG, help_strings_tag);
		definition->string_list_index = 0;
	}
	text_print(name, sizeof(name), "%s_help", spec->name);
	instance_set(grown, WIDGET_TAG, help_tag, name, definition);

	/* (each row: a copy of one of ITEM OPTIONS' rows, for its spinner's width) */
	for (index = 0; index < spec->row_count; index++)
	{
		short kind = spec->rows[index].model;
		struct ui_widget_definition *row = widget_copy(models->rows[kind], 2);
		struct ui_widget_definition *label = widget_copy(models->labels[kind], 0);
		struct ui_widget_definition *spinner = widget_copy(models->spinners[kind], 0);

		if (row)
		{
			child_set(row, 0, label_tags[index], 0, 0);
			child_set(row, 1, spinner_tags[index], models->spinner_x[kind], models->spinner_y[kind]);
		}
		if (label)
		{
			reference_set(&label->text_label_string_list, STRINGS_TAG, labels_tag);
			label->string_list_index = (short)index;
		}
		if (spinner)
			reference_set(&spinner->text_label_string_list, STRINGS_TAG, values_tags[index]);
		text_print(name, sizeof(name), "%s_row", spec->name);
		instance_set(grown, WIDGET_TAG, row_tags[index], name, row);
		text_print(name, sizeof(name), "%s_label", spec->name);
		instance_set(grown, WIDGET_TAG, label_tags[index], name, label);
		text_print(name, sizeof(name), "%s_spinner", spec->name);
		instance_set(grown, WIDGET_TAG, spinner_tags[index], name, spinner);
	}
	return screen_tag;
}

/* ---- the gametype editor's VEHICLE OPTIONS */

static char const *const yes_no_values[2] = { "YES", "NO" };
static char const *const vehicle_option_labels[VEHICLE_OPTION_ROWS] =
	{ "SHARED VEHICLES:", "LIGHT VEHICLES:", "MEDIUM VEHICLES:", "HEAVY VEHICLES:", "FLYING VEHICLES:", "TURRETS:" };
static char const *const vehicle_option_counts[NUMBER_OF_VEHICLE_CATEGORY_COUNTS] =
	{ "ALL", "NONE", "1", "2", "3", "4", "6", "8" };
/* each category, for the help: a vehicle, vehicles, and its kind */
static char const *const vehicle_help_names[NUMBER_OF_VEHICLE_CATEGORIES][3] = {
	{ "light vehicle", "light vehicles", "Ghosts" },
	{ "medium vehicle", "medium vehicles", "Warthogs" },
	{ "heavy vehicle", "heavy vehicles", "Scorpions" },
	{ "flying vehicle", "flying vehicles", "Banshees" },
	{ "turret", "turrets", "gun turrets" },
};
static char const vehicle_options_description[] =
	"Choose how many vehicles of\r\neach kind appear, and whether\r\nenemies can ride together.\r\n\r\nThis gametype:";

/* its rows, their help lines made (lines of 48 characters at most: the help box's width) */
static void vehicle_rows(struct menu_row *rows, char (*lines)[HELP_SIZE], char const **help)
{
	long category, count, at = 0;

	rows[0].label = vehicle_option_labels[0];
	rows[0].model = _row_model_yes_no;
	rows[0].value_count = 2;
	rows[0].values = yes_no_values;
	rows[0].help = help;
	joined(lines[at], "Players on different teams can ride in the same\r\n",
		"vehicle (in free for all, anyone with anyone).", NULL, NULL, NULL, NULL, NULL, NULL);
	help[at] = lines[at], at++;
	joined(lines[at], "Only teammates can ride in the same vehicle.", NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	help[at] = lines[at], at++;
	for (category = 0; category < NUMBER_OF_VEHICLE_CATEGORIES; category++)
	{
		char const *const *names = vehicle_help_names[category];
		struct menu_row *row = &rows[category + 1];

		row->label = vehicle_option_labels[category + 1];
		row->model = _row_model_wide;
		row->value_count = NUMBER_OF_VEHICLE_CATEGORY_COUNTS;
		row->values = vehicle_option_counts;
		row->help = help + at;
		for (count = 0; count < NUMBER_OF_VEHICLE_CATEGORY_COUNTS; count++)
		{
			if (count == _vehicle_category_count_all)
				joined(lines[at], "All of the map's ", names[1], " appear\r\n(", names[2],
					" and the like), as the map places them.", NULL, NULL, NULL);
			else if (count == _vehicle_category_count_none)
				joined(lines[at], "No ", names[1], " (", names[2], " and the like)\r\nappear on this map.", NULL, NULL,
					NULL);
			else
				joined(lines[at], "Each team gets ", vehicle_option_counts[count], " ",
					names[count == _vehicle_category_count_1 ? 0 : 1], " (", names[2],
					" and\r\nthe like), shuffled each game; both teams match.", NULL);
			help[at] = lines[at], at++;
		}
	}
}

/* the editor's row, opening the screen, before SAVE CHANGES; the editor's description gets its line, and its line
under the list moves down with it */
static void vehicle_editor_patch(struct cache_file_tag_instance *grown, long screen_tag)
{
	long edit_list = found(WIDGET_TAG, EDIT_LIST), edit_row = found(WIDGET_TAG, EDIT_ROW);
	long description_text = found(WIDGET_TAG, EDIT_DESCRIPTION_TEXT);
	long row_position, save_position, editor_row_tag, description_tag, title_tag, index;
	struct ui_widget_definition *editor_list, *definition;
	struct ui_widget_definition const *model;
	struct ui_widget_child_reference *edit_children;
	char const *strings[1];

	if (oxbox_menus.failed)
		return;
	model = widget_get(edit_list);
	row_position = child_position(model, edit_row);
	if (row_position == NONE || row_position + 1 >= model->child_widgets.count)
	{
		platform_log("menus: the gametype editor's list isn't as expected: VEHICLE OPTIONS isn't added");
		return;
	}
	save_position = row_position + 1;          /* (SAVE CHANGES, after INDICATOR OPTIONS) */
	editor_row_tag = next_tag();
	description_tag = next_tag();
	title_tag = next_tag();
	strings[0] = vehicle_options_description;
	instance_set(grown, STRINGS_TAG, description_tag, "playlist_edit_descriptions",
		strings_build(strings, 1, widget_get(description_text)->text_label_string_list.index, row_position + 1));
	strings[0] = "VEHICLE OPTIONS";
	instance_set(grown, STRINGS_TAG, title_tag, "playlist_edit_vehicles", strings_build(strings, 1, NONE, 0));

	/* (the editor's row: a copy of INDICATOR OPTIONS', a row lower, opening our screen) */
	definition = widget_copy(edit_row, 0);
	if (definition)
	{
		struct ui_widget_event_handler_reference *handlers;
		long count = definition->event_handlers.count;

		strncpy(definition->name, "playlist_edit_vehicles_item", sizeof(definition->name) - 1);
		definition->bounds.y0 += EDIT_ROW_STEP;
		definition->bounds.y1 += EDIT_ROW_STEP;
		reference_set(&definition->text_label_string_list, STRINGS_TAG, title_tag);
		definition->string_list_index = 0;
		handlers = count ? allocate(count * sizeof(*handlers)) : NULL;
		if (handlers)
		{
			memcpy(handlers, definition->event_handlers.address, count * sizeof(*handlers));
			for (index = 0; index < count; index++)
			{
				if (TEST_FLAG(handlers[index].flags, _event_handler_open_widget_bit))
					reference_set(&handlers[index].widget_tag, WIDGET_TAG, screen_tag);
			}
		}
		definition->event_handlers.address = handlers;
	}
	instance_set(grown, WIDGET_TAG, editor_row_tag, "playlist_edit_vehicles_item", definition);
	if (oxbox_menus.failed)
		return;

	/* the map's own, changed in its loaded copy (as loaded again each time) */
	editor_list = widget_get(edit_list);
	edit_children = allocate((editor_list->child_widgets.count + 1) * sizeof(*edit_children));
	if (!edit_children)
		return;
	{
		struct ui_widget_child_reference const *old =
			(struct ui_widget_child_reference const *)editor_list->child_widgets.address;
		long count = editor_list->child_widgets.count;

		memcpy(edit_children, old, save_position * sizeof(*edit_children));
		edit_children[save_position] = old[row_position];
		reference_set(&edit_children[save_position].widget_tag, WIDGET_TAG, editor_row_tag);
		memcpy(edit_children + save_position + 1, old + save_position, (count - save_position) * sizeof(*edit_children));
		for (index = save_position + 1; index <= count; index++)
			edit_children[index].vertical_offset += EDIT_ROW_STEP;
		editor_list->child_widgets.count = count + 1;
		editor_list->child_widgets.address = edit_children;
	}
	reference_set(&widget_get(description_text)->text_label_string_list, STRINGS_TAG, description_tag);
	{
		long edit_screen = tag_loaded(WIDGET_TAG, EDIT_SCREEN);
		struct ui_widget_definition *screen_definition = edit_screen == NONE ? NULL : widget_get(edit_screen);
		struct ui_widget_child_reference *children = screen_definition ?
			allocate(screen_definition->child_widgets.count * sizeof(*children)) : NULL;

		if (children)
		{
			memcpy(children, screen_definition->child_widgets.address,
				screen_definition->child_widgets.count * sizeof(*children));
			for (index = 0; index < screen_definition->child_widgets.count; index++)
			{
				char const *child_name = children[index].widget_tag.index == NONE ? "" :
					tag_get_name(children[index].widget_tag.index);
				long length = (long)strlen(child_name);

				if (length >= 8 && !strcmp(child_name + length - 8, "blueline"))
					children[index].vertical_offset += EDIT_ROW_STEP;
			}
			screen_definition->child_widgets.address = children;
		}
	}
	oxbox_menus.edit_list_index = edit_list;
	oxbox_menus.edit_row_position = save_position;
}

/* ---- the main menu's OPTIONS (hmp_game.c opens it, and keeps the settings: hmp_options_get, hmp_options_set) */

static char const *const options_labels[OPTIONS_ROWS] =
	{ "SCREEN SHAPE:", "ATTRACT MODE:", "ATTRACT MODE WAIT:", "CHECK FOR UPDATES:" };
static char const *const options_shape_values[3] = { "AS THE DASHBOARD", "4:3", "WIDESCREEN" };
static char const *const options_shape_help[3] = {
	"The screen shape the Xbox dashboard's settings\r\nsay (the dashboard's Settings, Video).",
	"A standard 4:3 picture, whatever the dashboard\r\nsays.",
	"A 16:9 widescreen picture, whatever the dashboard\r\nsays." };
static char const *const options_attract_values[3] = { "SHOWCASE", "HALO'S MOVIES", "OFF" };
static char const *const options_attract_help[3] = {
	"Left on the main menu, the showcase starts: a\r\nmatch between bots on one of your maps.",
	"Left on the main menu, Halo's own movies play.",
	"Nothing plays, however long the main menu is\r\nleft alone." };
static char const *const options_wait_values[4] = { "30 SECONDS", "1 MINUTE", "2 MINUTES", "5 MINUTES" };
static char const *const options_wait_help[4] = {
	"The attract mode starts after 30 seconds on the\r\nmain menu with nothing pressed.",
	"The attract mode starts after a minute on the\r\nmain menu with nothing pressed.",
	"The attract mode starts after 2 minutes on the\r\nmain menu with nothing pressed.",
	"The attract mode starts after 5 minutes on the\r\nmain menu with nothing pressed." };
static char const *const options_updates_help[2] = {
	"At startup, Halo 1 Unlimited looks online for a\r\nnewer release, and offers it on the main menu.",
	"Halo 1 Unlimited never looks online for a newer\r\nrelease (the installer's Y still can)." };

static void options_rows(struct menu_row *rows)
{
	rows[0].label = options_labels[0], rows[0].model = _row_model_widest, rows[0].value_count = 3;
	rows[0].values = options_shape_values, rows[0].help = (char const **)options_shape_help;
	rows[1].label = options_labels[1], rows[1].model = _row_model_widest, rows[1].value_count = 3;
	rows[1].values = options_attract_values, rows[1].help = (char const **)options_attract_help;
	rows[2].label = options_labels[2], rows[2].model = _row_model_widest, rows[2].value_count = 4;
	rows[2].values = options_wait_values, rows[2].help = (char const **)options_wait_help;
	rows[3].label = options_labels[3], rows[3].model = _row_model_yes_no, rows[3].value_count = 2;
	rows[3].values = yes_no_values, rows[3].help = (char const **)options_updates_help;
}

static void menus_build(void)
{
	long existing, index;
	struct cache_file_tag_instance *instances = cache_files_tag_instances(&existing);
	struct cache_file_tag_instance *grown;
	struct menu_models models;
	struct menu_row vehicle[VEHICLE_OPTION_ROWS], options[OPTIONS_ROWS];
	struct menu_screen spec;
	char (*lines)[HELP_SIZE];
	char const **help;
	long vehicle_screen;
	static char const *const row_names[NUMBER_OF_ROW_MODELS] = { ITEMS_YES_NO_ROW, ITEMS_WIDE_ROW, ITEMS_WIDEST_ROW };

	if (oxbox_menus.failed || !instances || existing <= 0 || existing > 0xFFFF - MAXIMUM_ADDED_TAGS)
		return;
	models.screen = found(WIDGET_TAG, ITEMS_SCREEN);
	models.list = found(WIDGET_TAG, ITEMS_LIST);
	models.header = found(WIDGET_TAG, ITEMS_HEADER);
	models.help = found(WIDGET_TAG, ITEMS_HELP);
	models.header_bitmap = found('bitm', ITEMS_HEADER_BITMAP);
	for (index = 0; index < NUMBER_OF_ROW_MODELS; index++)
	{
		models.rows[index] = found(WIDGET_TAG, row_names[index]);
		if (oxbox_menus.failed || !row_parts(models.rows[index], &models.labels[index], &models.spinners[index],
			&models.spinner_x[index], &models.spinner_y[index]))
		{
			platform_log("menus: ITEM OPTIONS' rows aren't as expected: Halo 1 Unlimited's screens aren't added");
			oxbox_menus.failed = TRUE;
			return;
		}
	}

	/* (the table, grown: every tag keeps its index, ours after them) */
	grown = allocate((existing + MAXIMUM_ADDED_TAGS) * sizeof(*grown));
	lines = allocate(VEHICLE_HELP_LINES * HELP_SIZE);
	help = allocate(VEHICLE_HELP_LINES * sizeof(*help));
	if (!grown || !lines || !help)
		return;
	memcpy(grown, instances, existing * sizeof(*grown));
	for (index = 0; index < existing; index++)
	{
		long salt = (unsigned long)instances[index].tag_index >> 16;

		if (salt > oxbox_menus.first_salt)
			oxbox_menus.first_salt = salt;
	}
	oxbox_menus.first_salt++;
	oxbox_menus.first_index = existing;

	vehicle_rows(vehicle, lines, help);
	spec.name = "vehicle_options", spec.title = "VEHICLE OPTIONS", spec.picture = 0;
	spec.row_count = VEHICLE_OPTION_ROWS, spec.rows = vehicle;
	spec.load_function = _menu_function_vehicle_options_load, spec.save_function = _menu_function_vehicle_options_save;
	vehicle_screen = screen_build(grown, &models, &spec);
	options_rows(options);
	spec.name = "options", spec.title = "OPTIONS", spec.picture = 1;
	spec.row_count = OPTIONS_ROWS, spec.rows = options;
	spec.load_function = _menu_function_options_load, spec.save_function = _menu_function_options_save;
	screen_build(grown, &models, &spec);
	if (oxbox_menus.failed || oxbox_menus.next > MAXIMUM_ADDED_TAGS)
		return;
	vehicle_editor_patch(grown, vehicle_screen);
	if (oxbox_menus.failed || oxbox_menus.next > MAXIMUM_ADDED_TAGS)
		return;

	oxbox_menus.count = oxbox_menus.next;
	oxbox_menus.original_instances = instances;
	oxbox_menus.original_count = existing;
	cache_files_set_tag_instances(grown, existing + oxbox_menus.count);
	platform_log("menus: VEHICLE OPTIONS and OPTIONS added (%ld tags)", oxbox_menus.count);
}

/* a list's rows' spinners, in order (count at most) */
static long spinners_of(struct menu_widget_instance *list, struct menu_widget_instance **spinners, long count)
{
	struct menu_widget_instance *row;
	long found_count = 0;

	for (row = list ? list->child : NULL; row && found_count < count; row = row->next)
	{
		struct menu_widget_instance *child = row->child;

		while (child && child->type != _widget_type_spinner_list)
			child = child->next;
		if (child)
			spinners[found_count++] = child;
	}
	return found_count;
}

static boolean vehicle_options_function(struct menu_widget_instance *list, boolean save)
{
	struct game_variant_options *options = player_ui_get_edit_playlist_options();
	struct menu_widget_instance *spinners[VEHICLE_OPTION_ROWS];
	long index, bits;

	if (!options || spinners_of(list, spinners, VEHICLE_OPTION_ROWS) != VEHICLE_OPTION_ROWS)
	{
		platform_log("menus: VEHICLE OPTIONS has no gametype being edited, or not its rows");
		return FALSE;
	}
	bits = options->vehicle_categories[0] | (options->vehicle_categories[1] << 8);
	if (!save)
	{
		spinners[0]->selected_index = options->shared_vehicles ? 0 : 1;
		for (index = 0; index < NUMBER_OF_VEHICLE_CATEGORIES; index++)
			spinners[index + 1]->selected_index = (short)VEHICLE_CATEGORY_COUNT(options, index);
		return TRUE;
	}
	options->shared_vehicles = spinners[0]->selected_index == 0;
	for (index = 0; index < NUMBER_OF_VEHICLE_CATEGORIES; index++)
	{
		long count = spinners[index + 1]->selected_index;

		if (count < 0 || count >= NUMBER_OF_VEHICLE_CATEGORY_COUNTS)
			count = _vehicle_category_count_all;
		bits = (bits & ~(7L << (3 * index))) | (count << (3 * index));
	}
	options->vehicle_categories[0] = (byte)bits;
	options->vehicle_categories[1] = (byte)(bits >> 8);
	return TRUE;
}

static boolean options_function(struct menu_widget_instance *list, boolean save)
{
	struct menu_widget_instance *spinners[OPTIONS_ROWS];
	short values[OPTIONS_ROWS];
	long index;

	if (spinners_of(list, spinners, OPTIONS_ROWS) != OPTIONS_ROWS)
	{
		platform_log("menus: OPTIONS hasn't its rows");
		return FALSE;
	}
	if (!save)
	{
		hmp_options_get(values);
		for (index = 0; index < OPTIONS_ROWS; index++)
			spinners[index]->selected_index = values[index];
		return TRUE;
	}
	for (index = 0; index < OPTIONS_ROWS; index++)
		values[index] = spinners[index]->selected_index;
	hmp_options_set(values);
	return TRUE;
}

/* ---------- public code (oxbox_sdk.c's, while OXMENU is loaded: its menu_tags_loaded and the rest call these) */

/* ui.map has loaded: TRUE when screens were added (else nothing is, and the section can go) */
boolean oxbox_menus_loaded(
	void)
{
	release(FALSE);
	menus_build();
	if (!oxbox_menus.original_instances)
		release(FALSE);
	return oxbox_menus.original_instances != NULL;
}

void oxbox_menus_unloaded(
	void)
{
	release(TRUE);
}

boolean oxbox_menus_tag(
	long tag_index)
{
	long absolute = tag_index & 0xFFFF;

	return tag_index != NONE && oxbox_menus.original_instances && absolute >= oxbox_menus.first_index &&
		absolute < oxbox_menus.first_index + oxbox_menus.count;
}

boolean oxbox_menus_function(
	void *widget,
	void *event,
	long function_index,
	boolean *widget_deleted)
{
	(void)event;
	(void)widget_deleted;
	switch (function_index)
	{
	case _menu_function_vehicle_options_load:
		return vehicle_options_function((struct menu_widget_instance *)widget, FALSE);
	case _menu_function_vehicle_options_save:
		return vehicle_options_function((struct menu_widget_instance *)widget, TRUE);
	case _menu_function_options_load:
		return options_function((struct menu_widget_instance *)widget, FALSE);
	case _menu_function_options_save:
		return options_function((struct menu_widget_instance *)widget, TRUE);
	}
	return FALSE;
}

/* the extended description's picture for a list's item: the gametype editor's, past the row added, is the one
the row it moved took (VEHICLE OPTIONS shows ITEM OPTIONS') */
short oxbox_menus_frame(
	long list_tag_index,
	short index)
{
	if (list_tag_index != oxbox_menus.edit_list_index || oxbox_menus.edit_row_position == NONE)
		return index;
	if (index == oxbox_menus.edit_row_position)
		return (short)(oxbox_menus.edit_row_position - 2);
	return index > oxbox_menus.edit_row_position ? (short)(index - 1) : index;
}
