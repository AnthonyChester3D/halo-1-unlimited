/* scancache.h: what the installer found looking at a folder or a file, kept from one run to the next (scancache.c),
so that a folder or a map that hasn't changed is never read again: its facts come back by its path and a key made
from what a folder listing tells for nothing (sizes and write times). Used by main.c's examine (a Halo folder's
XBEs and stock maps) and packs.c's packs_scan (a pack's maps' headers). */
#ifndef OXBOX_INSTALLER_SCANCACHE_H
#define OXBOX_INSTALLER_SCANCACHE_H

#define SCAN_CACHE_MOST_FACTS 160   /* (the biggest facts kept for a path) */

/* the facts kept for path (any case) with this key, copied to facts (size bytes): 1 when there are */
int scan_cache_get(char const *path, unsigned long key, void *facts, int size);

/* facts for path with this key, kept (in place of any it had) */
void scan_cache_put(char const *path, unsigned long key, void const *facts, int size);

/* the file read (at the start) and written (when anything was put since) */
void scan_cache_load(char const *file);
void scan_cache_save(char const *file);

/* how many gets found their facts, and didn't, since the start (for the log) */
void scan_cache_counts(unsigned long *hits, unsigned long *misses);

/* a key: FNV-1a over bytes, from key (0 to start) */
unsigned long scan_cache_hash(unsigned long key, void const *data, unsigned long size);

#endif
