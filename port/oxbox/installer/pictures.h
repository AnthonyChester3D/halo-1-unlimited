/* pictures.h: OpenCE's own menu pictures, made from the art in the player's own ui.map (port/oxbox/README.md).
None of Halo's art is in this repository or in an OpenCE package: the installer makes these on the Xbox from the ui.map
beside the game, and port/oxbox/installer/mkpictures (the same code, built for the PC) makes them for a build.

	pictures (PICTURES_SIZE bytes, 9 pictures of 16 KB): the main menu's OPTIONS and PROFILES words, normal and
		highlighted (DXT3 256 x 64), then the postgame's gametype pictures: CTF, King, Slayer, Oddball, Race
		(A8R8G8B8 64 x 64, swizzled)
	headers (HEADER_COUNT of HEADER_SIZE bytes, DXT3 512 x 64): ONLINE PLAY's ONLINE GAMES header, the gametype
		editor's VEHICLE OPTIONS header and the main menu's OPTIONS screen's header (port/oxbox/oxbox_menus.c)

Both come from letters and pictures of Halo's own: the words from CAMPAIGN, MULTIPLAYER, SETTINGS and GAME DEMOS, the
gametype pictures from the gametype editor's, ONLINE GAMES from SYSTEM LINK GAMES and SELECT PROFILE, VEHICLE OPTIONS
from LOAD LEVEL, SELECT PROFILE, CHOOSE DIFFICULTY and ITEM OPTIONS. */
#ifndef OXBOX_PICTURES_H
#define OXBOX_PICTURES_H

#include <stddef.h>

#define PICTURES_SIZE (9 * 16384)
#define HEADER_SIZE 32768
#define HEADER_COUNT 3

/* reads ui.map (an Xbox map: zlib after its first 2048 bytes) twice, the second time for the art, and makes them all
(header: HEADER_COUNT * HEADER_SIZE bytes); 0 when done, else an error's text (error[256]) */
int pictures_make(char const *ui_map_path, unsigned char *pictures, unsigned char *header, char *error);

/* (for the tool and the installer's progress) */
typedef void (*pictures_progress_t)(char const *step);
extern pictures_progress_t pictures_progress;

#endif
