/* library.h: the map library (library.c, docs/map_library.md): maps to download, listed in a signed catalog
(port/oxbox/tools/library.py makes it, oxsign.py signs it), each map a zip of its folder, put in the install's
maps\Library\<folder>\ (Halo Map Porter's map folders, together in a pack of their own). */
#ifndef OXBOX_LIBRARY_H
#define OXBOX_LIBRARY_H

#include "download.h"

/* the library's catalog; its maps' zips are beside it, unless the catalog names another address */
#define LIBRARY_DEFAULT_URL "https://github.com/h1u-librarian/map-library/releases/latest/download/catalog.oxlb"

/* the library's maps go together in the install's maps\Library\ (a pack of their own in the game's list); a whole
pack from it ("kind: pack") goes in maps\<its folder>\. (A map's folder name: at most 22 characters, the game's 30
for a pack's and its map's together) */
#define LIBRARY_PACK "Library"
#define LIBRARY_MAP_NAME 22

/* a map's folder on the Xbox holds this, from the library: what was installed ("sha256: <its zip's>", "group: ...") */
#define LIBRARY_MARK "library.oxl"

enum
{
	LIBRARY_NEW,       /* not on this Xbox */
	LIBRARY_INSTALLED, /* this one, from the library */
	LIBRARY_UPDATE,    /* an older one of it, from the library */
	LIBRARY_OTHER,     /* a copy of it put in maps\<item>\ by hand (or a pack's folder not from the library) */
	LIBRARY_SWAP       /* its folder holds the library's other version of it (another group's) */
};

struct library_item
{
	char item[32];           /* its folder (maps\<item>\) */
	char name[49], group[25], version[13];
	char about[304];
	char file[200];          /* its zip: a name beside the catalog, or an address */
	unsigned long size, unpacked, files;
	int pack;                /* ("kind: pack": a whole pack, in maps\<item>\; else a map, in maps\Library\<item>\) */
	unsigned char sha256[32];
	int state;               /* (library_states) */
};

struct library
{
	unsigned long serial;
	int count;
	struct library_item *items;
	char base[512];          /* the catalog's address up to its last '/' */
	char signer[32];         /* the key that signed it ("" : not signed, used because allow_unsigned) */
};

/* whether the installer has the library's key (library_keys.txt): the Map Library is on the main menu then (else only
with developer.ini's library_url, a test library) */
int library_ready(void);

/* the catalog at url, checked: signed by a key in library_keys.txt (allow_unsigned: a test location's, used
unsigned too); 0 when it's in library, else why not in error (256 bytes): 1 it couldn't be downloaded, 2 what came
isn't a list this can use (not signed by the library's key, damaged, a newer kind) */
int library_fetch(char const *url, int allow_unsigned, struct library *library, char *error);
void library_free(struct library *library);

/* where an item goes: maps\Library\<item> (a map), maps\<item> (a pack) */
void library_folder(struct library_item const *item, char const *maps, char *folder, int size);

/* the library's maps put where they go now: ones downloaded before (installer 62, in maps\<item>\ with the mark) moved
into maps\Library\, and what an interrupted download left tidied (library_tidy) */
void library_settle(struct library const *library, char const *maps);

/* each item's state, from the folders in maps (the install's "F:\Games\H1U\maps") */
void library_states(struct library *library, char const *maps);

/* downloads an item and puts it in its folder (library_folder: in place of what's there; a copy of a map put in
maps\<item>\ by hand is deleted once it's in), checked as it comes; 0 when it's in,
else why not in error (256 bytes). A power cut leaves only .hmp_ names, which the game skips (library_tidy) */
int library_get(struct library const *library, struct library_item const *item, char const *maps,
	struct download_report const *report, char *error);

/* an item's folder deleted (the map off this Xbox) */
int library_remove(struct library_item const *item, char const *maps, char *error);

/* what an interrupted download left in a folder (<folder>.hmp_dl, .hmp_dl.zip, .hmp_old) */
void library_tidy(char const *maps);

#endif
