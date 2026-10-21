/*
NETWORK_GAME_UI.C

symbols in this file:
0011AE30 0060:
	_network_game_get_random_player_name (0000)
*/

/* ---------- headers */

#include "cseries.h"
#include "math/real_math.h"
#include "tag_files/tag_groups.h"
#include "text/text_group.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes */

/* ---------- globals */

/* ---------- public code */

#if defined(HALO_XBOX_TARGET)
/* oxbox: the names kept from the menus' list: in a game the menus' tags
(ui.map) are not loaded, and a player joining a game in progress whose
name another player has (or a Default profile's, which has none) got an
empty one; the stock game added no players in game */
static wchar_t network_game_random_names[64][12];
static short network_game_random_name_count;
#endif

wchar_t const *network_game_get_random_player_name(
	void)
{
	wchar_t const *player_name = L"";
	long string_list_index = tag_loaded('ustr', "ui\\random_player_names");

#if defined(HALO_XBOX_TARGET)
	if (string_list_index != NONE && !network_game_random_name_count)
	{
		struct string_list *string_list = unicode_string_list_definition_get(string_list_index);
		short index;

		for (index = 0; string_list && index < string_list->strings.count &&
			network_game_random_name_count < (short)NUMBEROF(network_game_random_names); index++)
		{
			wchar_t const *name = unicode_string_list_get_string(string_list_index, index);

			if (name && name[0])
			{
				ustrncpy(network_game_random_names[network_game_random_name_count], name, 11);
				network_game_random_names[network_game_random_name_count][11] = 0;
				network_game_random_name_count++;
			}
		}
	}
	if (network_game_random_name_count)
	{
		return network_game_random_names[seed_random_range(
			get_global_local_random_seed_address(),
			0,
			network_game_random_name_count - 1)];
	}
	if (string_list_index == NONE)
		return L"Player";
#endif
	if (string_list_index != NONE)
	{
		struct string_list *string_list = unicode_string_list_definition_get(string_list_index);

		if (string_list)
		{
			short string_index = seed_random_range(
				get_global_local_random_seed_address(),
				0,
				string_list->strings.count - 1);
			player_name = unicode_string_list_get_string(string_list_index, string_index);
		}
	}

	return player_name;
}

/* ---------- private code */
