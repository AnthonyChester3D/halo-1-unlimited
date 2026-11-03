/*
OXBOX_MENUS.C

Halo 1 Unlimited's own Xbox menu screens: widget tags built when ui.map has loaded (cache_files.c calls
menu_tags_loaded) and added to its tag table, then run by the game's widgets as Bungie's are. ui.map itself is
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

Anything it can't find (a ui.map unlike the ones it knows) is logged, and nothing is added: the menus stay
Bungie's.
*/

#include "cseries.h"
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
#define ITEMS_HEADER SETUP "item_options_edit\\header_item_options"
#define ITEMS_YES_NO_ROW SETUP "item_options_edit\\op_infinite_grenades"
#define ITEMS_WIDE_ROW SETUP "item_options_edit\\op_vehicle_set"

enum
{
	/* our functions (MENU_FUNCTION_BASE + these) */
	_menu_function_vehicle_options_load = 0,
	_menu_function_vehicle_options_save,

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
	ROW_STEP = 30,
	EDIT_ROW_STEP = 33,
	/* the tags it adds: 4 string lists; the editor's row, the screen, its header, its list, and each row's
	container, label and spinner */
	ADDED_TAGS = 4 + 4 + 3 * VEHICLE_OPTION_ROWS,
	MAXIMUM_BLOCKS = 128,
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

static char const *const vehicle_option_labels[VEHICLE_OPTION_ROWS] =
	{ "SHARED VEHICLES:", "LIGHT VEHICLES:", "MEDIUM VEHICLES:", "HEAVY VEHICLES:", "FLYING VEHICLES:", "TURRETS:" };
static char const *const vehicle_option_counts[NUMBER_OF_VEHICLE_CATEGORY_COUNTS] =
	{ "ALL", "NONE", "1", "2", "3", "4", "6", "8" };
static char const vehicle_options_title[] = "VEHICLE OPTIONS";
static char const vehicle_options_description[] =
	"Choose how many vehicles of\r\neach kind appear, and whether\r\nenemies can ride together.\r\n\r\nThis gametype:";

/* ---------- private code */

static void *allocate(long size)
{
	void *block;

	if (oxbox_menus.failed || oxbox_menus.block_count >= MAXIMUM_BLOCKS || !(block = malloc(size > 0 ? size : 1)))
	{
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

static void vehicle_options_build(void)
{
	long existing, index;
	struct cache_file_tag_instance *instances = cache_files_tag_instances(&existing);
	struct cache_file_tag_instance *grown;
	long edit_list = found(WIDGET_TAG, EDIT_LIST), edit_row = found(WIDGET_TAG, EDIT_ROW);
	long description_text = found(WIDGET_TAG, EDIT_DESCRIPTION_TEXT);
	long items_screen = found(WIDGET_TAG, ITEMS_SCREEN), items_list = found(WIDGET_TAG, ITEMS_LIST);
	long items_header = found(WIDGET_TAG, ITEMS_HEADER);
	long yes_no_row = found(WIDGET_TAG, ITEMS_YES_NO_ROW), wide_row = found(WIDGET_TAG, ITEMS_WIDE_ROW);
	long yes_no_label, yes_no_spinner, wide_label, wide_spinner, row_position, save_position;
	short yes_no_x, yes_no_y, wide_x, wide_y;
	long labels_tag, counts_tag, title_tag, description_tag, editor_row_tag, screen_tag, header_tag, list_tag;
	long row_tags[VEHICLE_OPTION_ROWS], label_tags[VEHICLE_OPTION_ROWS], spinner_tags[VEHICLE_OPTION_ROWS];
	struct ui_widget_definition *editor_list, *definition, *screen, *list;
	struct ui_widget_definition const *model;
	struct ui_widget_child_reference *edit_children;
	char const *description_strings[1];

	if (oxbox_menus.failed || !instances || existing <= 0 || existing > 0xFFFF - ADDED_TAGS)
		return;
	if (!row_parts(yes_no_row, &yes_no_label, &yes_no_spinner, &yes_no_x, &yes_no_y) ||
		!row_parts(wide_row, &wide_label, &wide_spinner, &wide_x, &wide_y))
	{
		platform_log("menus: ITEM OPTIONS' rows aren't as expected: Halo 1 Unlimited's screens aren't added");
		return;
	}
	model = widget_get(edit_list);
	row_position = child_position(model, edit_row);
	if (row_position == NONE || row_position + 1 >= model->child_widgets.count)
	{
		platform_log("menus: the gametype editor's list isn't as expected: Halo 1 Unlimited's screens aren't added");
		return;
	}
	save_position = row_position + 1;          /* (SAVE CHANGES, after INDICATOR OPTIONS) */

	/* (the table, grown: every tag keeps its index, ours after them) */
	grown = allocate((existing + ADDED_TAGS) * sizeof(*grown));
	if (!grown)
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
	oxbox_menus.count = ADDED_TAGS;
	labels_tag = next_tag();
	counts_tag = next_tag();
	title_tag = next_tag();
	description_tag = next_tag();
	editor_row_tag = next_tag();
	screen_tag = next_tag();
	header_tag = next_tag();
	list_tag = next_tag();
	for (index = 0; index < VEHICLE_OPTION_ROWS; index++)
	{
		row_tags[index] = next_tag();
		label_tags[index] = next_tag();
		spinner_tags[index] = next_tag();
	}

	/* (the string lists) */
	description_strings[0] = vehicle_options_description;
	instance_set(grown, STRINGS_TAG, labels_tag, "vehicle_options_labels",
		strings_build(vehicle_option_labels, VEHICLE_OPTION_ROWS, NONE, 0));
	instance_set(grown, STRINGS_TAG, counts_tag, "vehicle_counts",
		strings_build(vehicle_option_counts, NUMBER_OF_VEHICLE_CATEGORY_COUNTS, NONE, 0));
	{
		char const *title = vehicle_options_title;

		instance_set(grown, STRINGS_TAG, title_tag, "vehicle_options_title", strings_build(&title, 1, NONE, 0));
	}
	instance_set(grown, STRINGS_TAG, description_tag, "playlist_edit_descriptions",
		strings_build(description_strings, 1,
			widget_get(description_text)->text_label_string_list.index, row_position + 1));

	/* (the screen: ITEM OPTIONS' children, its header and list ours) */
	model = widget_get(items_screen);
	screen = widget_copy(items_screen, model->child_widgets.count);
	if (screen)
	{
		struct ui_widget_child_reference *children = (struct ui_widget_child_reference *)screen->child_widgets.address;

		strncpy(screen->name, "vehicle_options_screen", sizeof(screen->name) - 1);
		for (index = 0; children && index < screen->child_widgets.count; index++)
		{
			if (children[index].widget_tag.index == items_header)
				reference_set(&children[index].widget_tag, WIDGET_TAG, header_tag);
			else if (children[index].widget_tag.index == items_list)
				reference_set(&children[index].widget_tag, WIDGET_TAG, list_tag);
		}
	}
	instance_set(grown, WIDGET_TAG, screen_tag, "vehicle_options_screen", screen);

	/* (its header: a label's look, the title's text, where ITEM OPTIONS' picture is) */
	definition = widget_copy(yes_no_label, 0);
	if (definition)
	{
		rectangle2d header_bounds = widget_get(items_header)->bounds;

		long large_font = tag_loaded('font', HEADER_FONT);

		strncpy(definition->name, "vehicle_options_header", sizeof(definition->name) - 1);
		definition->bounds.y0 = header_bounds.y0 + 14;
		definition->bounds.x0 = header_bounds.x0 + 14;
		definition->bounds.y1 = definition->bounds.y0 + 40;
		definition->bounds.x1 = header_bounds.x1 + 250;
		if (large_font != NONE)
			reference_set(&definition->text_font, 'font', large_font);
		reference_set(&definition->text_label_string_list, STRINGS_TAG, title_tag);
		definition->string_list_index = 0;
	}
	instance_set(grown, WIDGET_TAG, header_tag, "vehicle_options_header", definition);

	/* (its list: ITEM OPTIONS', with no help, our rows, and our functions to show and keep the choices) */
	list = widget_copy(items_list, VEHICLE_OPTION_ROWS);
	if (list)
	{
		static short const events[3] = { _event_created, _event_a_button, _event_start_button };
		long flags[3], widgets[3] = { NONE, NONE, NONE };
		short functions[3];

		flags[0] = 1L << _event_handler_run_function_bit;
		flags[1] = flags[2] = (1L << _event_handler_run_function_bit) | (1L << _event_handler_go_back_bit);
		functions[0] = MENU_FUNCTION_BASE + _menu_function_vehicle_options_load;
		functions[1] = functions[2] = MENU_FUNCTION_BASE + _menu_function_vehicle_options_save;
		strncpy(list->name, "vehicle_options_list", sizeof(list->name) - 1);
		list->game_data_inputs.count = 0;
		list->game_data_inputs.address = NULL;
		reference_set(&list->extended_description_widget, WIDGET_TAG, NONE);
		handlers_set(list, 3, events, flags, functions, widgets);
		for (index = 0; index < VEHICLE_OPTION_ROWS; index++)
		{
			struct ui_widget_child_reference const *first =
				(struct ui_widget_child_reference const *)widget_get(items_list)->child_widgets.address;

			child_set(list, index, row_tags[index], first->horizontal_offset,
				(short)(first->vertical_offset + index * ROW_STEP));
		}
	}
	instance_set(grown, WIDGET_TAG, list_tag, "vehicle_options_list", list);

	/* (each row: a copy of ITEM OPTIONS' INFINITE GRENADES row (YES / NO) or VEHICLE SET row (a wider spinner)) */
	for (index = 0; index < VEHICLE_OPTION_ROWS; index++)
	{
		boolean yes_no = index == 0;
		long row_model = yes_no ? yes_no_row : wide_row;
		struct ui_widget_definition *row = widget_copy(row_model, 2);
		struct ui_widget_definition *label = widget_copy(yes_no ? yes_no_label : wide_label, 0);
		struct ui_widget_definition *spinner = widget_copy(yes_no ? yes_no_spinner : wide_spinner, 0);

		if (row)
		{
			child_set(row, 0, label_tags[index], 0, 0);
			child_set(row, 1, spinner_tags[index], yes_no ? yes_no_x : wide_x, yes_no ? yes_no_y : wide_y);
		}
		if (label)
		{
			reference_set(&label->text_label_string_list, STRINGS_TAG, labels_tag);
			label->string_list_index = (short)index;
		}
		if (spinner && !yes_no)
			reference_set(&spinner->text_label_string_list, STRINGS_TAG, counts_tag);
		instance_set(grown, WIDGET_TAG, row_tags[index], "vehicle_options_row", row);
		instance_set(grown, WIDGET_TAG, label_tags[index], "vehicle_options_label", label);
		instance_set(grown, WIDGET_TAG, spinner_tags[index], "vehicle_options_spinner", spinner);
	}

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

	/* the map's own, changed in its loaded copy (as loaded again each time): the editor's list gets our row
	before SAVE CHANGES, which moves a row down, and its description text our string list (theirs, ours put in) */
	editor_list = widget_get(edit_list);
	edit_children = allocate((editor_list->child_widgets.count + 1) * sizeof(*edit_children));
	if (!edit_children)
		return;
	{
		struct ui_widget_child_reference const *old = (struct ui_widget_child_reference const *)editor_list->child_widgets.address;
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
	/* (and the editor screen's line under the list moves down with it) */
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
				char const *name = children[index].widget_tag.index == NONE ? "" :
					tag_get_name(children[index].widget_tag.index);
				long length = (long)strlen(name);

				if (length >= 8 && !strcmp(name + length - 8, "blueline"))
					children[index].vertical_offset += EDIT_ROW_STEP;
			}
			screen_definition->child_widgets.address = children;
		}
	}
	oxbox_menus.edit_list_index = edit_list;
	oxbox_menus.edit_row_position = save_position;

	oxbox_menus.original_instances = instances;
	oxbox_menus.original_count = existing;
	cache_files_set_tag_instances(grown, existing + ADDED_TAGS);
	platform_log("menus: VEHICLE OPTIONS added to the gametype editor (%ld tags)", (long)ADDED_TAGS);
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

/* ---------- public code (cache_files.c's, ui_widget*.c's) */

void menu_tags_loaded(
	char const *map_name)
{
	release(FALSE);
	if (!map_name || strcmp(map_name, "ui"))
		return;
	vehicle_options_build();
	if (!oxbox_menus.original_instances)
		release(FALSE);
}

void menu_tags_unloaded(
	void)
{
	release(TRUE);
}

boolean pc_menu_tag(
	long tag_index)
{
	long absolute = tag_index & 0xFFFF;

	return tag_index != NONE && oxbox_menus.original_instances && absolute >= oxbox_menus.first_index &&
		absolute < oxbox_menus.first_index + oxbox_menus.count;
}

boolean pc_menu_event_function_invoke(
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
	}
	return FALSE;
}

/* the extended description's picture for a list's item: the gametype editor's, past the row added, is the one
the row it moved took (VEHICLE OPTIONS shows ITEM OPTIONS') */
short oxbox_menus_description_frame(
	long list_tag_index,
	short index)
{
	if (list_tag_index != oxbox_menus.edit_list_index || oxbox_menus.edit_row_position == NONE)
		return index;
	if (index == oxbox_menus.edit_row_position)
		return (short)(oxbox_menus.edit_row_position - 2);
	return index > oxbox_menus.edit_row_position ? (short)(index - 1) : index;
}
