/* packs.h: a map pack (another Halo folder on the hard drive, a modded Halo's) imported into Halo 1 Unlimited:
its multiplayer maps as a pack in the game's map list, its campaign as one the game can pick (packs.c). */
#ifndef OXBOX_PACKS_H
#define OXBOX_PACKS_H

#define PACK_MAPS 96
#define PACK_KEY 16

struct pack_map
{
	char name[40];                /* the file's name, without .map */
	int type;                     /* its header's scenario type: 0 campaign, 1 multiplayer, 2 the menus */
	unsigned long length, checksum;
	int halos;                    /* the same file as Halo's own (Halo's 13 multiplayer maps, or the install's) */
};

struct pack_scan
{
	char path[160];               /* the pack's Halo folder ("F:\Games\Halo NHE") */
	char key[PACK_KEY];           /* its name in the game's storage (maps\<key>, campaigns\<key>) */
	int count;
	struct pack_map maps[PACK_MAPS];
	int multiplayer;              /* multiplayer maps that aren't Halo's own */
	int campaign;                 /* campaign levels, when one of them isn't Halo's own (else 0) */
	int multiplayer_imported;     /* already imported: maps in maps\<key>, levels in campaigns\<key> */
	int campaign_imported;
};

/* what it's doing, for the installer's log */
typedef void (*packs_say_t)(int warning, char const *text);

/* the maps in path\maps (their headers), next to the install's own in stock_folder (its maps folder: "" for none),
and what's imported from it already; 0 when it read */
int packs_scan(char const *path, char const *stock_folder, struct pack_scan *scan);

/* imports its multiplayer maps and/or its campaign (again: what was there is replaced); 0 when done, else error[256] */
int packs_import(struct pack_scan *scan, int multiplayer, int campaign, packs_say_t say, char *error);

/* takes out what was imported from it (the pack's own files are never touched) */
void packs_remove(struct pack_scan *scan, int multiplayer, int campaign, packs_say_t say);

#endif
