/*
SHOWCASE.C (oxbox, the builds with OXBOX_SHOWCASE)

The showcase screensaver. When the main menu has waited its attract time (75 seconds, or opence.ini's
showcase_seconds) with nobody pressing anything, instead of an attract movie the Xbox hosts a closed system link
game on a folder map that has a showcase.txt, with bots, and flies that file's camera shots over the map while the
bots play: no HUD, the host's own player only watching (never spawned), the game not advertised, listed or
joinable, and not counted in the play stats. Any button fades it out and brings the main menu back; nothing else
sees that press.

Halo Map Porter writes showcase.txt in a ported map's folder, beside info.txt and preview.bin (a map maker can
ship their own). Map sharing (hmp_share.c, version 6) sends it with the map. The file, as the porter's showcase.hpp
has it:

	# a comment (anything after # on a line)
	fade 0.5                  seconds of the fade to black between shots (0 to 5; 0.5 when left out)
	shot 12 orbit: red flag   a shot: its seconds (up to 600), then a label for people
	look 12.3 -4.2 1.1        (in a shot, optional) the camera keeps this point in the middle of the view
	key 0 x y z yaw pitch     (in a shot) a key: seconds into the shot, the camera's position (world units), its
	                          direction in degrees (yaw 0 = +x, counterclockwise; pitch up positive), then the
	                          horizontal field of view, optional (70 when left out)

Shots play in the file's order, then the list starts again. Between a shot's keys the camera moves on a curve
through them in time: Hermite between two keys with Catmull-Rom tangents from their neighbours (one-sided at the
ends), each of x, y, z, yaw, pitch and field of view on its own, yaw the short way round; before the first key and
after the last it holds. With look, it turns to keep the point in the middle. This is the porter's at() (its Level
window's preview plays the same), so a shot looks here as it looked there. The fade is ours (the porter's preview
cuts): out over a shot's last `fade` seconds, in over the next one's first.

Which map: the folder maps with a showcase.txt, and the stock maps with a maps\showcase\<name>.txt (bloodgulch.txt,
the level's own name: the same format; the installer adds the package's on a first install), not ones hidden in the
map browser, in a random order (seeded from the cycle counter: a fresh boot gave the same order each time), a
favorite three times as likely to come next and a map in an "Artistic" genre twice (both: four times), a map at a
time. A map plays at most 90 seconds (opence.ini showcase_map_seconds = N; 0: no limit): its shots in order when they
fit, else the first, as many of the others as fit in a random order, and the last (up to 128 shots), so a map with
many shots differs each time. It plays its shots through (and again, until it has played a minute or its limit),
then the next map loads (through the main menu, as every system link game does, with the title music kept off);
with one map, its list just plays on. The game is CTF where the map has it (its info.txt's "Gametypes:" line
names CTF; every stock map), else Team Slayer, to 999 with no time limit and every vehicle the map places (the bots
travel to the other team's flag or find the other team, which plays better than Slayer's fights where they spawn,
and drive and fly what's there), and bots: the map's suggested players
(info.txt, else 8), 4 to 10, less the watching player, made even for the two teams, at Normal (opence.ini
showcase_bots = N: N bots). The bots drop out one at a time while the game runs under 15 frames a second
(opence.ini showcase_min_fps = N: under N; 0: never), down to two; each map starts with the full count again. No
announcer ("Team Slayer") at a map's start: game_engine_multiplayer_sounds.c is quiet while it runs.

A map's first ten seconds have a title card, as the campaign's chapter titles have: black bars top and bottom (an
eighth of the screen each, sliding in and out over a second) and the map's name (its info.txt's first line, as the
map browser shows it) bottom right in the chapter titles' font and place (the HUD's single player font and default
title bounds), fading in and out.

The hook sites are oxbox_showcase.h's list. Hosting is network_test.c's (the PC's automated system link tests): the
lobby's fast setup (on the showcase's map from the start: player_ui.c leaves it, so the lobby doesn't ask for the
default map, Carousel, whose copy into the cache partition held up the map's own), the map and the variant, the
player, then the start once the bots are in (they join faster than in a person's lobby: bots.c). It plays
without the network cable too: while it runs, the host's network checks treat its game as a split screen game,
which needs none (network_client_manager.c, network_server_manager.c). If hosting fails twice in a row, the movies
play for the next ten minutes.

The log (oxbox_log.txt): a line when it starts, each map, and when it ends, and any showcase.txt it couldn't read
(with the line and why). It reads only the maps' showcase.txt files and writes nothing.
*/

#if defined(OXBOX_SHOWCASE)

#include "cseries.h"
#include "cseries_windows.h"
#include "main/main.h"
#include "interface/attract_mode.h"
#include "interface/event_manager.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#if defined(OXBOX_TEST_SHOWCASE_SWITCH_TEAM)
#include "networking/network_game_manager.h"
#endif
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "scenario/scenario.h"
#include "camera/observer.h"
#include "cutscene/cinematics.h"
#include "interface/hud.h"
#include "interface/hud_definitions.h"
#include "rasterizer/rasterizer.h"
#include "text/draw_string.h"
#include "hmp.h"
#include "oxbox_showcase.h"
#if defined(OXBOX_NET_P2P)
#include "../src/p2p.h" /* port: port/linux/src/p2p_lobby.c */
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

void platform_log(char const *format, ...);
short hmp_browse_suggested_players(char const *level_name);
boolean transport_network_available(void); /* (the network cable: transport_endpoint_set_winsock.c) */
long halo_screen_width(void);
short network_game_server_bot_count(short *people);   /* (network_server_manager.c) */

/* opence.ini (oxbox_entry.c): showcase = 0 turns it off (the movies, as Halo has them); showcase_seconds = N: the
main menu waits N seconds (5 and up) instead of 75 */
int oxbox_setting_showcase = 1;
int oxbox_setting_showcase_seconds = 0;
int oxbox_setting_showcase_bots = -1;   /* showcase_bots = N: that many bots (0 to 15), else the map's suggestion */
int oxbox_setting_showcase_min_fps = 15;   /* showcase_min_fps = N: bots drop out under N frames a second (0: never) */
int oxbox_setting_showcase_map_seconds = 90;   /* showcase_map_seconds = N: each map at most N seconds (0: no limit) */

/* ---------- constants */

#define SHOWCASE_MAXIMUM_SHOTS 128
#define SHOWCASE_MAXIMUM_KEYS 1024
#define SHOWCASE_MAXIMUM_MAPS 128
#define SHOWCASE_KEY_LENGTH 48
#define SHOWCASE_LINE_LENGTH 256
#define SHOWCASE_DEFAULT_FADE 0.5f
#define SHOWCASE_DEFAULT_FOV 70.0f
#define SHOWCASE_MENU_FADE_SECONDS 0.5f       /* the main menu to black, and back */
#define SHOWCASE_LEAVE_FADE_SECONDS 0.35f     /* the game to black after a press */
#define SHOWCASE_SETTLE_SECONDS 1.5f          /* black once the map has loaded (the bots spawning), then shot 1 */
#define SHOWCASE_MAP_MINIMUM_SECONDS 60.0f    /* a map's list plays again until it has played this long (or its limit) */
#define SHOWCASE_FAVORITE_WEIGHT 2             /* a map's chances of being next: 1, these more for a favorite... */
#define SHOWCASE_ARTISTIC_WEIGHT 1             /* ...and for a map in a genre named "Artistic" (the map browser's) */
#define SHOWCASE_RETRY_MILLISECONDS (10 * 60 * 1000)
#define SHOWCASE_SCORE_TO_WIN 999
#define SHOWCASE_MENU_SETTLE_SECONDS 0.5f      /* the main menu back between maps, before the next lobby */
#define SHOWCASE_LOBBY_MAP_SECONDS 0.3f        /* the lobby's map and variant set this long after the fast setup */
#define SHOWCASE_LOBBY_PLAYER_SECONDS 0.5f     /* ... and the watching player added */
#define SHOWCASE_LOBBY_START_SECONDS 1.0f      /* the start: once the bots are in, and no sooner than this */
#define SHOWCASE_LOBBY_LONGEST_SECONDS 5.0f    /* ... or this, bots or not */
#define SHOWCASE_TITLE_SECONDS 10.0f           /* the title card: the bars and the map's name, from the first shot */
#define SHOWCASE_TITLE_SLIDE_SECONDS 1.0f      /* the bars sliding in and out (the campaign's letterbox: a second) */
#define SHOWCASE_TITLE_FADE_SECONDS 1.0f       /* the name fading in and out */
#define SHOWCASE_TITLE_BAR 0.125f              /* each bar's height, of the screen's (the campaign's) */
#define SHOWCASE_FPS_WINDOW_MILLISECONDS 3000  /* the frame rate, over this long */
#define SHOWCASE_FPS_SETTLE_SECONDS 5.0f       /* none measured this long into a map (its textures loading) */
#define SHOWCASE_FPS_DROP_SECONDS 6.0f         /* after a bot drops out, this long before the next */
#define SHOWCASE_FEWEST_BOTS 2                 /* a bot a team */

enum
{
	_showcase_off,
	_showcase_menu_fade,        /* the main menu fading to black */
	_showcase_hosting,          /* the lobby, under the black: the map, the variant, the player, the bots */
	_showcase_loading,          /* the game starting and its map loading */
	_showcase_playing,          /* the shots */
	_showcase_leaving,          /* to black, then the game let go */
	_showcase_returning,        /* waiting for the main menu: then the next map, or the menu back */
	_showcase_menu_return       /* the main menu fading back in */
};

/* ---------- structures */

/* a key: its time (seconds into the shot), x, y, z, yaw (unwrapped: each within half a turn of the one before),
pitch, field of view (degrees) */
enum { _key_t, _key_x, _key_y, _key_z, _key_yaw, _key_pitch, _key_fov, NUMBER_OF_KEY_VALUES };

struct showcase_key
{
	float v[NUMBER_OF_KEY_VALUES];
};

struct showcase_shot
{
	float seconds;
	boolean look;
	float look_point[3];
	short first_key;
	short key_count;
};

/* the map's showcase.txt, read */
static struct
{
	float fade;
	short shot_count;
	short key_count;
	boolean full;                   /* more shots or keys than we keep: the first ones */
	struct showcase_shot shots[SHOWCASE_MAXIMUM_SHOTS];
	struct showcase_key keys[SHOWCASE_MAXIMUM_KEYS];
} showcase_file;

static struct
{
	short state;
	boolean user_ended;             /* a press ended it: the main menu back, not the next map */
	boolean chain;                  /* the next map once the main menu is back */
	unsigned long input_time;       /* the last press's time (event_manager.c) */
	unsigned long last_time;        /* the last frame's (system_milliseconds) */
	real state_seconds;             /* in this state */
	real alpha;                     /* the fade to black over everything: 1 is black */
	unsigned long retry_after;      /* hosting failed: the movies until then */
	short failures;                 /* games in a row that didn't start */
	/* the lobby */
	boolean map_set;
	boolean player_added;
	real menu_seconds;              /* back at the main menu this long */
	/* the map, and where its shots are */
	char key[SHOWCASE_KEY_LENGTH];
	short plan[SHOWCASE_MAXIMUM_SHOTS];   /* the shots to play, in order (showcase_plan) */
	short plan_count;
	short shot_index;                     /* in the plan */
	real shot_time;                 /* seconds into the shot (below 0: the settle before the first) */
	real map_seconds;
	boolean camera_valid;
	struct observer_result camera;
	/* the maps to show, in order, and the next */
	short map_count;
	short map_next;
	char maps[SHOWCASE_MAXIMUM_MAPS][SHOWCASE_KEY_LENGTH];
	unsigned long random;
	/* the map's name for its title card (read in the menus: the map browser's names are let go in a game) */
	wchar_t title[64];
	/* the bots: how many now (fewer when the frame rate is low), the frames counted since window_start, and when
	one last dropped out (map_seconds) */
	short bots;
	unsigned long fps_window_start;
	long fps_frames;
	real drop_seconds;
} showcase;

/* ---------- private code: the file */

/* a number from the line (as C++'s >> takes one): TRUE with it, FALSE (and the line where it was) when there's none */
static boolean showcase_number(
	char **text,
	double *value)
{
	char *end;
	double number;

	while (**text == ' ')
		(*text)++;
	if (!**text)
		return FALSE;
	number = strtod(*text, &end);
	if (end == *text || number != number || number > 1.0e7 || number < -1.0e7)
		return FALSE;
	*text = end;
	*value = number;
	return TRUE;
}

/* one line of the file: TRUE, or FALSE and why */
static boolean showcase_line(
	char *line,
	char const **why)
{
	char *p;
	char word[8];
	long length = 0;
	double value;

	/* (the comment off; commas, semicolons, tabs and returns are spaces) */
	for (p = line; *p; p++)
	{
		if (*p == '#')
		{
			*p = 0;
			break;
		}
		if (*p == ',' || *p == ';' || *p == '\t' || *p == '\r')
			*p = ' ';
	}
	p = line;
	while (*p == ' ')
		p++;
	if (!*p)
		return TRUE;
	while (*p && *p != ' ' && length < (long)sizeof(word) - 1)
		word[length++] = *p++;
	word[length] = 0;
	if (*p && *p != ' ')
	{
		*why = "it isn't fade, shot, look or key";
		return FALSE;
	}
	if (!strcmp(word, "fade"))
	{
		if (!showcase_number(&p, &value) || value < 0.0 || value > 5.0)
		{
			*why = "fade needs seconds (0 to 5)";
			return FALSE;
		}
		showcase_file.fade = (float)value;
	}
	else if (!strcmp(word, "shot"))
	{
		struct showcase_shot *shot;

		if (!showcase_number(&p, &value) || value <= 0.0 || value > 600.0)
		{
			*why = "shot needs its seconds (up to 600)";
			return FALSE;
		}
		if (showcase_file.shot_count >= SHOWCASE_MAXIMUM_SHOTS)
		{
			showcase_file.full = TRUE;
			return TRUE;
		}
		shot = &showcase_file.shots[showcase_file.shot_count++];
		memset(shot, 0, sizeof(*shot));
		shot->seconds = (float)value;
		shot->first_key = showcase_file.key_count;
	}
	else if (!strcmp(word, "look"))
	{
		struct showcase_shot *shot;
		double x, y, z;

		if (!showcase_file.shot_count)
		{
			*why = "look before any shot";
			return FALSE;
		}
		if (!showcase_number(&p, &x) || !showcase_number(&p, &y) || !showcase_number(&p, &z))
		{
			*why = "look needs x y z";
			return FALSE;
		}
		shot = &showcase_file.shots[showcase_file.shot_count - 1];
		shot->look = TRUE;
		shot->look_point[0] = (float)x;
		shot->look_point[1] = (float)y;
		shot->look_point[2] = (float)z;
	}
	else if (!strcmp(word, "key"))
	{
		struct showcase_shot *shot;
		struct showcase_key *key;
		double t, x, y, z, yaw, pitch, fov;

		if (!showcase_file.shot_count)
		{
			*why = "key before any shot";
			return FALSE;
		}
		if (!showcase_number(&p, &t) || !showcase_number(&p, &x) || !showcase_number(&p, &y) ||
			!showcase_number(&p, &z))
		{
			*why = "key needs its time and x y z";
			return FALSE;
		}
		if (!showcase_number(&p, &yaw))
			yaw = 0.0;
		if (!showcase_number(&p, &pitch))
			pitch = 0.0;
		if (!showcase_number(&p, &fov) || fov < 10.0 || fov > 150.0)
			fov = SHOWCASE_DEFAULT_FOV;
		shot = &showcase_file.shots[showcase_file.shot_count - 1];
		if (shot->key_count && t < showcase_file.keys[showcase_file.key_count - 1].v[_key_t])
		{
			*why = "keys go in time order";
			return FALSE;
		}
		if (showcase_file.key_count >= SHOWCASE_MAXIMUM_KEYS)
		{
			/* (the shot it's in is cut short: left out) */
			showcase_file.full = TRUE;
			showcase_file.key_count -= shot->key_count;
			showcase_file.shot_count--;
			return TRUE;
		}
		key = &showcase_file.keys[showcase_file.key_count++];
		key->v[_key_t] = (float)t;
		key->v[_key_x] = (float)x;
		key->v[_key_y] = (float)y;
		key->v[_key_z] = (float)z;
		/* (the yaw unwrapped: within half a turn of the key before) */
		if (shot->key_count)
		{
			double before = showcase_file.keys[showcase_file.key_count - 2].v[_key_yaw];

			while (yaw - before > 180.0)
				yaw -= 360.0;
			while (yaw - before < -180.0)
				yaw += 360.0;
		}
		key->v[_key_yaw] = (float)yaw;
		key->v[_key_pitch] = (float)pitch;
		key->v[_key_fov] = (float)fov;
		shot->key_count++;
	}
	else
	{
		*why = "it isn't fade, shot, look or key";
		return FALSE;
	}
	return TRUE;
}

/* a map's shots: the showcase.txt beside a folder map's .map, or a stock map's maps\showcase\<its name>.txt
(maps\showcase\bloodgulch.txt); FALSE for none */
static boolean showcase_path(
	char const *key,
	char *path,
	long size)
{
	char *slash;

	if (key && key[0] != '+' && hmp_browse_stock_title(key))
	{
		char const *name = strrchr(key, '\\');

		name = name ? name + 1 : key;
		if ((long)strlen(name) + 24 > size)
			return FALSE;
		strcpy(path, "d:\\maps\\showcase\\");
		strcat(path, name);
		strcat(path, ".txt");
		return TRUE;
	}
	if (size < 16 || !hmp_map_file_path(key, path, size - 16))
		return FALSE;
	slash = strrchr(path, '\\');
	if (!slash)
		return FALSE;
	strcpy(slash + 1, "showcase.txt");
	return TRUE;
}

static HANDLE showcase_open(
	char const *key)
{
	char path[300];

	if (!showcase_path(key, path, sizeof(path)))
		return INVALID_HANDLE_VALUE;
	return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
}

/* reads the map's showcase.txt: TRUE when it has a shot to play */
static boolean showcase_load(
	char const *key)
{
	HANDLE file = showcase_open(key);
	char buffer[1024];
	char line[SHOWCASE_LINE_LENGTH];
	long line_length = 0, line_number = 1;
	unsigned long got = 0;
	char const *why = NULL;
	boolean long_line = FALSE;
	short index;

	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	memset(&showcase_file, 0, sizeof(showcase_file));
	showcase_file.fade = SHOWCASE_DEFAULT_FADE;
	while (!why && !showcase_file.full && ReadFile(file, buffer, sizeof(buffer), &got, NULL) && got)
	{
		unsigned long at;

		for (at = 0; at < got && !why && !showcase_file.full; at++)
		{
			char c = buffer[at];

			if (c != '\n')
			{
				/* (a line too long for us: past its first 255 characters is all comment, or it's an error) */
				if (line_length < SHOWCASE_LINE_LENGTH - 1)
					line[line_length++] = c;
				else
					long_line = TRUE;
				continue;
			}
			line[line_length] = 0;
			if (long_line && !strchr(line, '#'))
				why = "a line longer than 255 characters";
			else if (!showcase_line(line, &why))
				break;
			line_length = 0;
			long_line = FALSE;
			line_number++;
		}
	}
	/* (the last line, without its end) */
	if (!why && !showcase_file.full && line_length)
	{
		line[line_length] = 0;
		showcase_line(line, &why);
	}
	CloseHandle(file);
	if (!why && showcase_file.full)
		platform_log("showcase: %s's showcase.txt has more than %d shots or %d keys: its first %d shots", key,
			SHOWCASE_MAXIMUM_SHOTS, SHOWCASE_MAXIMUM_KEYS, (int)showcase_file.shot_count);
	for (index = 0; !why && index < showcase_file.shot_count; index++)
	{
		if (!showcase_file.shots[index].key_count)
		{
			why = "a shot with no keys";
			line_number = 0;
		}
	}
	if (!why && !showcase_file.shot_count)
	{
		why = "no shots";
		line_number = 0;
	}
	if (why)
	{
		if (line_number)
			platform_log("showcase: %s's showcase.txt, line %ld: %s", key, line_number, why);
		else
			platform_log("showcase: %s's showcase.txt: %s", key, why);
		showcase_file.shot_count = 0;
		return FALSE;
	}
	return TRUE;
}

/* ---------- private code: playback */

/* where the camera is t seconds into the shot (the porter's at()): x, y, z, yaw, pitch, field of view (degrees) */
static void showcase_view(
	struct showcase_shot const *shot,
	double t,
	double *view)
{
	struct showcase_key const *keys = &showcase_file.keys[shot->first_key];
	long n = shot->key_count;
	long c;

	if (n == 1 || t <= keys[0].v[_key_t])
	{
		for (c = 0; c < 6; c++)
			view[c] = keys[0].v[c + 1];
	}
	else if (t >= keys[n - 1].v[_key_t])
	{
		for (c = 0; c < 6; c++)
			view[c] = keys[n - 1].v[c + 1];
	}
	else
	{
		long i = 0;
		double t0, t1, d, u, h00, h10, h01, h11;

		while (i + 2 < n && t > keys[i + 1].v[_key_t])
			i++;
		t0 = keys[i].v[_key_t];
		t1 = keys[i + 1].v[_key_t];
		d = t1 - t0 > 1.0e-6 ? t1 - t0 : 1.0e-6;
		u = (t - t0) / d;
		u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u;
		h00 = 2.0 * u * u * u - 3.0 * u * u + 1.0;
		h10 = u * u * u - 2.0 * u * u + u;
		h01 = -2.0 * u * u * u + 3.0 * u * u;
		h11 = u * u * u - u * u;
		for (c = 0; c < 6; c++)
		{
			double tangent[2];
			long side;

			/* (Catmull-Rom: from the keys either side; one-sided at the shot's first and last) */
			for (side = 0; side < 2; side++)
			{
				long j = i + side, a = j - 1, b = j + 1;
				double span;

				if (j == 0)
					a = 0, b = 1;
				else if (j == n - 1)
					a = n - 2, b = n - 1;
				span = keys[b].v[_key_t] - keys[a].v[_key_t];
				tangent[side] = (keys[b].v[c + 1] - keys[a].v[c + 1]) / (span > 1.0e-6 ? span : 1.0e-6);
			}
			view[c] = h00 * keys[i].v[c + 1] + h10 * d * tangent[0] + h01 * keys[i + 1].v[c + 1] +
				h11 * d * tangent[1];
		}
	}
	if (shot->look)
	{
		double dx = shot->look_point[0] - view[0], dy = shot->look_point[1] - view[1],
			dz = shot->look_point[2] - view[2];

		if (sqrt(dx * dx + dy * dy + dz * dz) > 1.0e-6)
		{
			view[3] = atan2(dy, dx) * (180.0 / M_PI);
			view[4] = atan2(dz, sqrt(dx * dx + dy * dy)) * (180.0 / M_PI);
		}
	}
}

/* the camera, and the fade, for where the shots are */
static void showcase_camera_update(
	boolean fade)
{
	struct showcase_shot const *shot = &showcase_file.shots[showcase.plan[showcase.shot_index]];
	double view[6], yaw, pitch, fov;
	real t = showcase.shot_time;

	showcase_view(shot, t < 0.0f ? 0.0 : (double)t, view);
	yaw = view[3] * (M_PI / 180.0);
	pitch = view[4] * (M_PI / 180.0);
	/* (a curve can overshoot its keys a little) */
	fov = view[5] < 5.0 ? 5.0 : view[5] > 170.0 ? 170.0 : view[5];
	memset(&showcase.camera, 0, sizeof(showcase.camera));
	showcase.camera.position.x = (real)view[0];
	showcase.camera.position.y = (real)view[1];
	showcase.camera.position.z = (real)view[2];
	showcase.camera.forward.i = (real)(cos(pitch) * cos(yaw));
	showcase.camera.forward.j = (real)(cos(pitch) * sin(yaw));
	showcase.camera.forward.k = (real)sin(pitch);
	showcase.camera.up.i = (real)(-sin(pitch) * cos(yaw));
	showcase.camera.up.j = (real)(-sin(pitch) * sin(yaw));
	showcase.camera.up.k = (real)cos(pitch);
	showcase.camera.field_of_view = DEGREES_TO_RADIANS((real)fov);
	scenario_location_from_point(&showcase.camera.location, &showcase.camera.position);
	showcase.camera_valid = TRUE;
	/* (the watching player, who has no unit, where the camera is: the effects and particles made are the ones the
	local players could see, players.c's combined_pvs_local) */
	{
		long player_index = local_player_get_player_index(0);

		if (player_index != NONE && showcase.camera.location.cluster_index != NONE)
		{
			struct player_datum *player = player_get(player_index);

			if (player->unit_index == NONE)
				player->cluster_index = showcase.camera.location.cluster_index;
		}
	}
	if (fade)
	{
		/* black while it settles; then in over a shot's first `fade` seconds, out over its last (at most half the
		shot each) */
		real length = showcase_file.fade;
		real alpha = 0.0f;

		if (length > shot->seconds * 0.5f)
			length = shot->seconds * 0.5f;
		if (t < 0.0f)
			alpha = 1.0f;
		else if (length > 0.0f)
		{
			if (t < length)
				alpha = 1.0f - t / length;
			if (t > shot->seconds - length && (t - (shot->seconds - length)) / length > alpha)
				alpha = (t - (shot->seconds - length)) / length;
		}
		showcase.alpha = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
	}
}

/* ---------- private code: which map */

static unsigned long showcase_random(
	void)
{
	showcase.random = showcase.random * 1103515245ul + 12345ul;
	return showcase.random >> 8;
}

static void showcase_swap_maps(
	long a,
	long b)
{
	char swap[SHOWCASE_KEY_LENGTH];

	if (a == b)
		return;
	strcpy(swap, showcase.maps[a]);
	strcpy(showcase.maps[a], showcase.maps[b]);
	strcpy(showcase.maps[b], swap);
}

/* the cycle counter: the random numbers' seed (a fresh boot reaches the showcase at the same millisecond nearly
every time, which gave the same order every time; the cycles between differ) */
static unsigned long showcase_cycles(
	void)
{
	unsigned long low, high;

	__asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
	return low ^ (high << 16) ^ (high >> 16);
}

/* whether the map goes in the showcase's list: not hidden in the map browser, and its shots there; weight: its
chances of being next (more for a favorite, and for an "Artistic" one) */
static boolean showcase_listed(
	char const *key,
	unsigned long *weight)
{
	struct hmp_rating rating;
	HANDLE file;

	if (!key || strlen(key) >= SHOWCASE_KEY_LENGTH)
		return FALSE;
	hmp_stats_get(key, &rating);
	if (rating.hidden)
		return FALSE;
	*weight = 1 + (rating.favorite ? SHOWCASE_FAVORITE_WEIGHT : 0) +
		(hmp_browse_in_genre(key, "artistic") ? SHOWCASE_ARTISTIC_WEIGHT : 0);
	file = showcase_open(key);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	CloseHandle(file);
	return TRUE;
}

/* the maps with shots (folder maps with a showcase.txt, stock maps with a maps\showcase\<name>.txt), in the order
to show them: a random order, a favorite or an "Artistic" map more likely to come early (each place drawn from the
maps left, by their weights) */
static void showcase_find_maps(
	void)
{
	static unsigned long weights[SHOWCASE_MAXIMUM_MAPS];
	long count, index, place;
	char last[SHOWCASE_KEY_LENGTH];

	strcpy(last, showcase.key);
	showcase.random ^= showcase_cycles() ^ system_milliseconds();
	showcase.random |= 1;
	if (!hmp_maps_count())
		hmp_maps_scan();
	count = hmp_maps_count();
	showcase.map_count = 0;
	showcase.map_next = 0;
	for (index = 0; index < count + HMP_STOCK_LEVELS && showcase.map_count < SHOWCASE_MAXIMUM_MAPS; index++)
	{
		char const *key = index < count ? hmp_maps_key(index) : hmp_maps_stock_level(index - count);

		if (showcase_listed(key, &weights[showcase.map_count]))
			strcpy(showcase.maps[showcase.map_count++], key);
	}
	for (place = 0; place + 1 < showcase.map_count; place++)
	{
		unsigned long total = 0, pick;

		for (index = place; index < showcase.map_count; index++)
			total += weights[index];
		pick = showcase_random() % total;
		for (index = place; index + 1 < showcase.map_count && pick >= weights[index]; index++)
			pick -= weights[index];
		showcase_swap_maps(place, index);
		pick = weights[place];
		weights[place] = weights[index];
		weights[index] = pick;
	}
	/* (not the map just shown first again) */
	if (showcase.map_count > 1 && !strcmp(showcase.maps[0], last))
		showcase_swap_maps(0, 1);
}

/* the next map to show, its showcase.txt read: FALSE when there's none */
static boolean showcase_next_map(
	void)
{
	long tries;

	for (tries = 0; tries <= SHOWCASE_MAXIMUM_MAPS; tries++)
	{
		if (showcase.map_next >= showcase.map_count)
		{
			showcase_find_maps();
			if (!showcase.map_count)
				return FALSE;
		}
		if (showcase_load(showcase.maps[showcase.map_next]))
		{
			strcpy(showcase.key, showcase.maps[showcase.map_next++]);
			return TRUE;
		}
		/* (a file that can't be read is left out until the maps are looked at again) */
		showcase.map_next++;
		if (tries >= showcase.map_count)
			break;
	}
	return FALSE;
}

/* ---------- private code: the shots */

/* the least a map plays (its list again until then): a minute, or its limit when that's less */
static real showcase_minimum_seconds(
	void)
{
	int limit = oxbox_setting_showcase_map_seconds;

	return limit > 0 && limit < SHOWCASE_MAP_MINIMUM_SECONDS ? (real)limit : SHOWCASE_MAP_MINIMUM_SECONDS;
}

/* the shots to play: all of them in order when they fit in the map's limit (showcase_map_seconds); else the first,
as many of the others as fit in a random order, and the last, so a map with many shots differs each time */
static void showcase_plan(
	void)
{
	short count = showcase_file.shot_count;
	short middle[SHOWCASE_MAXIMUM_SHOTS];
	short middle_count = 0;
	short index;
	real total = 0.0f, room;

	showcase.plan_count = 0;
	for (index = 0; index < count; index++)
		total += showcase_file.shots[index].seconds;
	if (oxbox_setting_showcase_map_seconds <= 0 || count <= 2 || total <= (real)oxbox_setting_showcase_map_seconds)
	{
		for (index = 0; index < count; index++)
			showcase.plan[showcase.plan_count++] = index;
		return;
	}
	room = (real)oxbox_setting_showcase_map_seconds - showcase_file.shots[0].seconds -
		showcase_file.shots[count - 1].seconds;
	for (index = 1; index < count - 1; index++)
		middle[middle_count++] = index;
	for (index = middle_count - 1; index > 0; index--)   /* (Fisher-Yates) */
	{
		short other = (short)(showcase_random() % (unsigned long)(index + 1));
		short swap = middle[index];

		middle[index] = middle[other];
		middle[other] = swap;
	}
	showcase.plan[showcase.plan_count++] = 0;
	for (index = 0; index < middle_count; index++)
	{
		if (showcase_file.shots[middle[index]].seconds <= room)
		{
			showcase.plan[showcase.plan_count++] = middle[index];
			room -= showcase_file.shots[middle[index]].seconds;
		}
	}
	showcase.plan[showcase.plan_count++] = count - 1;
	platform_log("showcase: %d of %d shots fit in %d seconds: the first, %d in a random order, the last",
		(int)showcase.plan_count, (int)count, oxbox_setting_showcase_map_seconds, (int)showcase.plan_count - 2);
}

/* ---------- private code: the game */

static void showcase_set_state(
	short state)
{
	showcase.state = state;
	showcase.state_seconds = 0.0f;
}

/* a game that didn't start: the next map, but after two in a row the movies for ten minutes */
static void showcase_failed(
	void)
{
	if (++showcase.failures >= 2)
	{
		showcase.failures = 0;
		showcase.retry_after = system_milliseconds() + SHOWCASE_RETRY_MILLISECONDS;
		if (!showcase.retry_after)
			showcase.retry_after = 1;
		showcase.chain = FALSE;
		platform_log("showcase: two games in a row didn't start: the movies for the next ten minutes");
	}
}

/* the map's name for its title card: its menu name (info.txt's first line, or a stock map's), else its folder's */
static void showcase_title_read(
	void)
{
	wchar_t const *name = hmp_maps_title(showcase.key);
	char const *stock = hmp_browse_stock_title(showcase.key);
	long length = 0;

	if (stock)
	{
		while (stock[length] && length < (long)NUMBEROF(showcase.title) - 1)
		{
			showcase.title[length] = (wchar_t)(unsigned char)stock[length];
			length++;
		}
	}
	else if (name)
	{
		while (name[length] && length < (long)NUMBEROF(showcase.title) - 1)
		{
			showcase.title[length] = name[length];
			length++;
		}
	}
	else
	{
		char const *folder = strrchr(showcase.key, '/');

		folder = folder ? folder + 1 : showcase.key[0] == '+' ? showcase.key + 1 : showcase.key;
		while (folder[length] && length < (long)NUMBEROF(showcase.title) - 1)
		{
			showcase.title[length] = (wchar_t)(unsigned char)folder[length];
			length++;
		}
	}
	showcase.title[length] = 0;
}

/* the frame rate while the map plays: under the lowest wanted (opence.ini showcase_min_fps), a bot drops out, then
another after a while if it's still low, down to a bot a team (each map starts with the full count again) */
static void showcase_frame_rate(
	unsigned long now)
{
	unsigned long elapsed;
	long fps10;

	if (oxbox_setting_showcase_min_fps <= 0 || showcase.map_seconds < SHOWCASE_SETTLE_SECONDS + SHOWCASE_FPS_SETTLE_SECONDS)
	{
		showcase.fps_window_start = now;
		showcase.fps_frames = 0;
		return;
	}
	showcase.fps_frames++;
	elapsed = now - showcase.fps_window_start;
	if (elapsed < SHOWCASE_FPS_WINDOW_MILLISECONDS)
		return;
	fps10 = (long)((unsigned long)showcase.fps_frames * 10000ul / elapsed);
	showcase.fps_window_start = now;
	showcase.fps_frames = 0;
	if (fps10 >= oxbox_setting_showcase_min_fps * 10 || showcase.bots <= SHOWCASE_FEWEST_BOTS ||
		showcase.map_seconds - showcase.drop_seconds < SHOWCASE_FPS_DROP_SECONDS)
	{
		return;
	}
	showcase.bots--;
	showcase.drop_seconds = showcase.map_seconds;
	bots_showcase(showcase.bots);
	platform_log("showcase: %ld.%ld frames a second over %lu seconds (under %d): a bot drops out, %d left", fps10 / 10,
		fps10 % 10, elapsed / 1000, oxbox_setting_showcase_min_fps, (int)showcase.bots);
}

/* the lobby, as the menus' fast setup makes it (network_test.c's host) */
static void showcase_host(
	void)
{
	short players = hmp_browse_suggested_players(showcase.key);

	main_set_multiplayer_map_name(showcase.key);
	player_ui_fast_setup_network_server();
	if (!global_network_game_server_get() || !global_network_game_client_get())
	{
		platform_log("showcase: couldn't host a game: the movies for now");
		showcase.failures = 1;
		showcase_failed();
		showcase_set_state(_showcase_returning);
		return;
	}
	if (players <= 0)
		players = 8;
	players = players < 4 ? 4 : players > 10 ? 10 : players;
	/* (Team Slayer: an even number of bots, so the two teams' bots are as many, with the watching player, who never
	spawns, on one of them) */
	if ((players - 1) % 2)
		players++;
	if (oxbox_setting_showcase_bots >= 0)
		players = (short)((oxbox_setting_showcase_bots > 15 ? 15 : oxbox_setting_showcase_bots) + 1);
	showcase.bots = (short)(players - 1);
	bots_showcase(showcase.bots);
	showcase_title_read();
#if defined(OXBOX_NET_P2P)
	/* (not in internet play's server browser) */
	p2p_set_hosting_public(FALSE);
#endif
	showcase.map_set = FALSE;
	showcase.player_added = FALSE;
	showcase.camera_valid = FALSE;
	platform_log("showcase: %s, %d shots, %d bots", showcase.key, (int)showcase_file.shot_count, (int)(players - 1));
	showcase_set_state(_showcase_hosting);
}

/* to black, then the game let go */
static void showcase_leave(
	boolean next_map)
{
	showcase.chain = next_map && !showcase.user_ended;
	switch (showcase.state)
	{
	case _showcase_menu_fade:
		/* (nothing hosted yet: the main menu back) */
		showcase_set_state(_showcase_menu_return);
		break;
	case _showcase_hosting:
	case _showcase_loading:
	case _showcase_playing:
		showcase_set_state(_showcase_leaving);
		break;
	default:
		break;
	}
}

/* back at the main menu with no game: the next map, or the main menu itself */
static void showcase_back_at_menu(
	void)
{
	showcase.camera_valid = FALSE;
	if (showcase.chain && !showcase.user_ended && showcase_next_map())
	{
		showcase.alpha = 1.0f;
		showcase_host();
		return;
	}
	bots_showcase(-1);
	attract_mode_reset_timer();
	if (showcase.user_ended)
		platform_log("showcase: ended by a press");
	showcase_set_state(_showcase_menu_return);
}

/* ---------- public code */

unsigned long showcase_idle_milliseconds(
	void)
{
	if (oxbox_setting_showcase && oxbox_setting_showcase_seconds > 0)
		return (unsigned long)(oxbox_setting_showcase_seconds < 5 ? 5 : oxbox_setting_showcase_seconds) * 1000ul;
	return 75000ul;
}

boolean showcase_active(
	void)
{
	return showcase.state != _showcase_off;
}

boolean showcase_quiet(
	void)
{
	return showcase.state != _showcase_off && showcase.state != _showcase_menu_return && !showcase.user_ended;
}

boolean showcase_start(
	void)
{
	if (showcase.state != _showcase_off)
		return TRUE;
	if (!oxbox_setting_showcase)
		return FALSE;
	if (showcase.retry_after && (long)(system_milliseconds() - showcase.retry_after) < 0)
		return FALSE;
	showcase.retry_after = 0;
	if (!showcase_next_map())
		return FALSE;
	showcase.user_ended = FALSE;
	showcase.chain = FALSE;
	showcase.input_time = event_manager_time_of_last_event();
	showcase.last_time = system_milliseconds();
	showcase.alpha = 0.0f;
	ui_stop_main_menu_music();
	platform_log("showcase: the main menu has waited %lu seconds: the showcase%s", showcase_idle_milliseconds() / 1000,
		transport_network_available() ? "" : " (no network cable: the game is this console's alone anyway)");
	showcase_set_state(_showcase_menu_fade);
	return TRUE;
}

void showcase_update(
	boolean main_menu_loaded)
{
	unsigned long now = system_milliseconds();
	real seconds;

	if (showcase.state == _showcase_off)
		return;
	/* (the frame's time; a long gap, such as a map loading, counts as one frame) */
	seconds = (real)(now - showcase.last_time) * 0.001f;
	showcase.last_time = now;
	if (seconds < 0.0f || seconds > 0.5f)
		seconds = 1.0f / 30.0f;
	showcase.state_seconds += seconds;

	/* a press ends it, and goes no further: the menus and the game never see it (nor any press until the main
	menu fades back in) */
	if (showcase.state != _showcase_menu_return)
	{
		if (event_manager_time_of_last_event() != showcase.input_time)
		{
			showcase.input_time = event_manager_time_of_last_event();
			if (!showcase.user_ended)
			{
				showcase.user_ended = TRUE;
				showcase_leave(FALSE);
			}
		}
		event_manager_flush();
	}

	switch (showcase.state)
	{
	case _showcase_menu_fade:
		showcase.alpha += seconds / SHOWCASE_MENU_FADE_SECONDS;
		if (showcase.alpha >= 1.0f)
		{
			showcase.alpha = 1.0f;
			showcase_host();
		}
		break;

	case _showcase_hosting:
	{
		struct network_game_server *server = global_network_game_server_get();
		struct network_game_client *client = global_network_game_client_get();

		showcase.alpha = 1.0f;
		if (!server || !client || showcase.state_seconds > 20.0f)
		{
			platform_log("showcase: the lobby went away");
			showcase_leave(TRUE);
			showcase_failed();
			break;
		}
		/* the map and the variant (the fast setup clears them) */
		if (!showcase.map_set && showcase.state_seconds >= SHOWCASE_LOBBY_MAP_SECONDS)
		{
			struct game_variant variant;

			network_game_server_change_map_name(server, showcase.key);
			/* (CTF where the map has it, else Team Slayer: the bots go after the other team, or its flag, rather than
			fighting where they spawn; every vehicle the map places, for them to drive and fly) */
#if defined(OXBOX_TEST_SHOWCASE_VARIANT)
			/* (test builds: another game type, for watching the bots play it: OXBOX_TEST_SHOWCASE_VARIANT="oddball") */
			variant = *game_engine_get_variant_by_name(&variant, OXBOX_TEST_SHOWCASE_VARIANT);
#else
			variant = *game_engine_get_variant_by_name(&variant, hmp_browse_has_ctf(showcase.key) ? "ctf" : "team_slayer");
#endif
			platform_log("showcase: %s, %s, every vehicle the map places", showcase.key,
				hmp_browse_has_ctf(showcase.key) ? "CTF" : "Team Slayer (no CTF on its card)");
			variant.universal_variant.score_to_win = SHOWCASE_SCORE_TO_WIN;
			player_ui_set_game_variant(&variant);
			{
				/* (the vehicles: the gametype's PC options, each team's set, game_engine.h; the server takes the
				menus' options with their gametype) */
				struct game_variant_options options = *player_ui_get_game_variant_options();

				options.vehicle_set[0] = options.vehicle_set[1] = VARIANT_VEHICLE_SET_PC;
				player_ui_set_game_variant_options(&options);
			}
			network_game_server_change_game_variant(server, &variant);
			showcase.map_set = TRUE;
		}
		/* the watching player, as pressing A in the lobby adds one (the bots come once there's a person) */
		if (showcase.map_set && !showcase.player_added && showcase.state_seconds >= SHOWCASE_LOBBY_PLAYER_SECONDS)
			showcase.player_added = network_game_client_add_player(client, 0);
#if defined(OXBOX_TEST_SHOWCASE_SWITCH_TEAM)
		/* (test builds: once the bots are in, the teams logged, then the watching player changes team, as a person might
		in the lobby, and two seconds later the teams logged again: the bots should have evened them out,
		network_server_manager.c. The start waits for it) */
		{
			static short step;
			static unsigned long since;

			if (showcase.player_added && step < 2 && network_game_server_bot_count(NULL) >= showcase.bots)
			{
				if (!since)
					since = system_milliseconds();
				if (system_milliseconds() - since >= 2000)
				{
					struct network_game *test_game = network_game_client_get_game(client);
					short count[2] = { 0, 0 }, index;

					for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
					{
						struct network_player test_player = test_game->players[index];

						if (!network_player_is_present(&test_player) || test_player.team_index < 0 || test_player.team_index > 1)
							continue;
						count[test_player.team_index]++;
						/* (the watching player alone, as its team spinner does: the bots are on the host's machine too) */
						if (!step && !network_player_is_bot(&test_player))
						{
							test_player.team_index = (char)(test_player.team_index == 1 ? 0 : 1);
							network_game_client_update_local_player_data(client, &test_player);
						}
					}
					platform_log("showcase test: teams %d red, %d blue%s", (int)count[0], (int)count[1],
						step ? " (two seconds after the switch)" : "; the watching player switches team");
					step++;
					since = system_milliseconds();
				}
				showcase.state_seconds = 0.5f;
				break;
			}
		}
#endif
		/* the start, once the bots are in (or at the latest, bots or not) */
		if (showcase.player_added && showcase.state_seconds >= SHOWCASE_LOBBY_START_SECONDS &&
			(network_game_server_bot_count(NULL) >= showcase.bots ||
				showcase.state_seconds >= SHOWCASE_LOBBY_LONGEST_SECONDS))
		{
			network_game_client_request_immediate_start();
			showcase_set_state(_showcase_loading);
		}
		break;
	}

	case _showcase_loading:
		showcase.alpha = 1.0f;
		if (game_in_progress() && !main_menu_loaded && game_engine_running())
		{
			showcase_plan();
			showcase.shot_index = 0;
			showcase.shot_time = -SHOWCASE_SETTLE_SECONDS;
			showcase.map_seconds = 0.0f;
			showcase.drop_seconds = -SHOWCASE_FPS_DROP_SECONDS;
			showcase.fps_window_start = now;
			showcase.fps_frames = 0;
			showcase.failures = 0;
			showcase_camera_update(TRUE);
			showcase_set_state(_showcase_playing);
		}
		else if (showcase.state_seconds > 30.0f || (main_menu_loaded && !global_network_game_server_get()))
		{
			platform_log("showcase: %s didn't start", showcase.key);
			showcase_leave(TRUE);
			showcase_failed();
		}
		break;

	case _showcase_playing:
		if (main_menu_loaded || !game_in_progress())
		{
			/* (the game went away) */
			showcase.chain = TRUE;
			showcase_set_state(_showcase_returning);
			break;
		}
		/* (a game that ended: 999 kills, after hours) */
		if (!game_engine_running() || !game_engine_can_score())
		{
			showcase.alpha = 1.0f;
			showcase_leave(TRUE);
			break;
		}
		showcase.shot_time += seconds;
		showcase.map_seconds += seconds;
		showcase_frame_rate(now);
		/* (its limit reached, a shot or not: on to the next map, fading as a press does) */
		if (oxbox_setting_showcase_map_seconds > 0 && showcase.map_count > 1 &&
			showcase.map_seconds - SHOWCASE_SETTLE_SECONDS >= (real)oxbox_setting_showcase_map_seconds)
		{
			showcase_leave(TRUE);
			break;
		}
		while (showcase.shot_time >= showcase_file.shots[showcase.plan[showcase.shot_index]].seconds)
		{
			showcase.shot_time -= showcase_file.shots[showcase.plan[showcase.shot_index]].seconds;
			if (++showcase.shot_index >= showcase.plan_count)
			{
				showcase.shot_index = 0;
				/* the plan played through: the next map, once this one has played a minute or its limit (with only
				one, it plays on, shuffled again) */
				if (showcase.map_seconds - SHOWCASE_SETTLE_SECONDS >= showcase_minimum_seconds() &&
					(showcase.map_count > 1 || showcase.map_next < showcase.map_count))
				{
					showcase.shot_time = 0.0f;
					showcase.alpha = 1.0f;
					showcase_leave(TRUE);
					break;
				}
				showcase_plan();
			}
		}
		if (showcase.state == _showcase_playing)
			showcase_camera_update(TRUE);
		break;

	case _showcase_leaving:
		showcase.alpha += seconds / SHOWCASE_LEAVE_FADE_SECONDS;
		/* (the camera flies on while it fades) */
		if (showcase.camera_valid && game_in_progress() && !main_menu_loaded &&
			showcase.shot_time + seconds < showcase_file.shots[showcase.plan[showcase.shot_index]].seconds)
		{
			showcase.shot_time += seconds;
			showcase_camera_update(FALSE);
		}
		if (showcase.alpha >= 1.0f)
		{
			showcase.alpha = 1.0f;
			if (global_network_game_client_get() || global_network_game_server_get())
				network_game_abort();
			showcase.menu_seconds = 0.0f;
			showcase_set_state(_showcase_returning);
		}
		break;

	case _showcase_returning:
		showcase.alpha = 1.0f;
		if (main_menu_loaded)
			showcase.camera_valid = FALSE;
		/* (the main menu settling first, as network_test.c waits; with something over it, such as an error box,
		the main menu back after 3 seconds, to show it) */
		if (main_menu_loaded && !network_game_is_active())
		{
			showcase.menu_seconds += seconds;
			if (!main_menu_screen_is_active() && showcase.menu_seconds >= 3.0f)
			{
				showcase.chain = FALSE;
				showcase_back_at_menu();
			}
			else if (main_menu_screen_is_active() &&
				showcase.menu_seconds >= (showcase.chain && !showcase.user_ended ? SHOWCASE_MENU_SETTLE_SECONDS : 0.2f))
			{
				showcase_back_at_menu();
			}
		}
		else
		{
			showcase.menu_seconds = 0.0f;
			if (showcase.state_seconds > 60.0f)
			{
				platform_log("showcase: the main menu didn't come back");
				showcase.chain = FALSE;
				showcase_back_at_menu();
			}
		}
		break;

	case _showcase_menu_return:
		showcase.alpha -= seconds / SHOWCASE_MENU_FADE_SECONDS;
		if (showcase.alpha <= 0.0f)
		{
			showcase.alpha = 0.0f;
			bots_showcase(-1);
			showcase_set_state(_showcase_off);
		}
		break;
	}
}

struct observer_result const *showcase_camera(
	short local_player_index,
	struct observer_result const *camera)
{
	if (showcase.state == _showcase_off || !showcase.camera_valid || local_player_index == NONE ||
		!game_in_progress() || main_menu_is_active())
	{
		return camera;
	}
	return &showcase.camera;
}

/* the title card over the map's first seconds: the bars, and the name bottom right (cinematics.c's chapter titles:
the HUD's single player font in its default title bounds) */
static void showcase_title_render(
	void)
{
	real t, bars, fade;
	short width = (short)halo_screen_width();
	short left = (short)(-(width - 640) / 2), right = (short)(640 + (width - 640) / 2);
	rectangle2d bar;
	long font;

	if (showcase.state != _showcase_playing && showcase.state != _showcase_leaving)
		return;
	t = showcase.map_seconds - SHOWCASE_SETTLE_SECONDS;
	if (t <= 0.0f || t >= SHOWCASE_TITLE_SECONDS + SHOWCASE_TITLE_SLIDE_SECONDS)
		return;
	bars = t < SHOWCASE_TITLE_SLIDE_SECONDS ? t / SHOWCASE_TITLE_SLIDE_SECONDS :
		t > SHOWCASE_TITLE_SECONDS ? 1.0f - (t - SHOWCASE_TITLE_SECONDS) / SHOWCASE_TITLE_SLIDE_SECONDS : 1.0f;
	bars = bars < 0.0f ? 0.0f : bars > 1.0f ? 1.0f : bars;
	if (bars > 0.0f)
	{
		short height = (short)(bars * SHOWCASE_TITLE_BAR * 480.0f + 0.5f);

		bar.x0 = left;
		bar.x1 = right;
		bar.y0 = 0;
		bar.y1 = height;
		draw_quad(&bar, 0xFF000000);
		bar.y0 = (short)(480 - height);
		bar.y1 = 480;
		draw_quad(&bar, 0xFF000000);
	}
	fade = t < SHOWCASE_TITLE_SLIDE_SECONDS * 0.5f ? 0.0f :
		t < SHOWCASE_TITLE_SLIDE_SECONDS * 0.5f + SHOWCASE_TITLE_FADE_SECONDS ?
			(t - SHOWCASE_TITLE_SLIDE_SECONDS * 0.5f) / SHOWCASE_TITLE_FADE_SECONDS :
		t > SHOWCASE_TITLE_SECONDS - SHOWCASE_TITLE_FADE_SECONDS ?
			(SHOWCASE_TITLE_SECONDS - t) / SHOWCASE_TITLE_FADE_SECONDS : 1.0f;
	fade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
	if (fade <= 0.0f || !showcase.title[0] || !hud_globals)
		return;
	font = hud_globals->messaging.single_player_font.index;
	if (font == NONE)
		font = hud_globals->messaging.multi_player_font.index;
	if (font == NONE)
		return;
	{
		/* (the campaign's light grey, a little shadow) */
		real_argb_color color = { 1.0f, 0.8f, 0.8f, 0.8f };
		rectangle2d bounds = hud_globals->defaults.default_title_bounds;
		short shift;

		if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0 || bounds.y0 < 240)
		{
			bounds.y0 = 425;
			bounds.x0 = 0;
			bounds.y1 = 444;
			bounds.x1 = 597;
		}
		/* (kept in its place relative to the screen's sides on a wider screen, as cinematics.c does) */
		shift = (short)(((bounds.x0 + bounds.x1) / 2) * (width - 640) / 640);
		bounds.x0 += shift;
		bounds.x1 += shift;
		color.alpha = fade;
		draw_string_set_draw_mode(font, NONE, 1, 0, &color);
		draw_string_set_tab_stops(NULL, 0);
		rasterizer_text_set_shadow_color(((pixel32)(long)(fade * 255.0f) << 24));
		rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, showcase.title);
		rasterizer_text_set_shadow_color(0);
	}
}

void showcase_render(
	void)
{
	rectangle2d bounds;
	long alpha;

	if (showcase.state == _showcase_off)
		return;
	showcase_title_render();
	if (showcase.alpha <= 0.0f)
		return;
	alpha = (long)(showcase.alpha * 255.0f + 0.5f);
	if (alpha > 255)
		alpha = 255;
	/* (the whole screen, as the menus' own fade to black: ui_widget.c) */
	bounds.x0 = (short)(-(halo_screen_width() - 640) / 2);
	bounds.x1 = (short)(640 + (halo_screen_width() - 640) / 2);
	bounds.y0 = 0;
	bounds.y1 = 480;
	draw_quad(&bounds, (pixel32)alpha << 24);
}

#endif /* OXBOX_SHOWCASE */
