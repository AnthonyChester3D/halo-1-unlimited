/*
HALO_PORT_CAPACITY.H

Memory capacity of the native builds (Windows, Linux, Android), sized for the
session limits in halo_port_limits.h, which includes this file. The Xbox
sizes are given in parentheses below.

Every machine in a session must be built with the same values: the
distributed netcode names objects and players by their datum index, the
same on every machine (port/linux/game/network_objects.c tracks
MAXIMUM_TRACKED_OBJECTS = HALO_PORT_MAXIMUM_OBJECTS_PER_MAP objects, and a
client's own objects take the upper half of the object array).
*/

#ifndef __HALO_PORT_CAPACITY_H
#define __HALO_PORT_CAPACITY_H

/* ---------- game state

The Xbox game state is 0x345000 bytes at 0x80061000 and ends where the tag
cache begins (0x803A6000). Cache files are linked to that tag cache address,
so the game state cannot grow in place. The native builds put a 16 MB game
state above the tag cache (which ends at 0x819A6000), inside the Xbox memory
window (0x80000000-0xA0000000, Android's to 0x88000000:
port/linux/src/platform.h) and below everything the window hands out
top-down (texture and sound caches, Direct3D resources).

The CPU part holds about 17.2 MB of pools at the sizes below (the Xbox pools
fill 3,165,260 of its 0x305000 bytes); the GPU part holds only the decal
vertices, as on the Xbox. A change to a pool's size changes the game state's
layout: saved games of builds before it no longer load. */

#define HALO_PORT_GAME_STATE_BASE_ADDRESS 0x80061000 /* (xbox) */
#if defined(HALO_XBOX_TARGET) && defined(OXBOX_STRIP)
/* oxbox (test build 30): the game state as big as its pools, with the AI's trimmed (below): 2,626,380 bytes of
CPU pools and 163,840 of decal vertices, "game state:" in oxbox_log.txt. It still starts at 0x80061000, and the tag
cache at 0x803A6000; the 612 KB between them are free for the rest of the game (physical_memory_map.c allocates the
game state's size alone), and the texture cache is 512 KB bigger for it. */
#define HALO_PORT_GAME_STATE_CPU_SIZE 0x284000 /* (xbox: 0x305000) */
#define HALO_PORT_GAME_STATE_GPU_SIZE 0x28000 /* (xbox: 0x40000) */
#else
#define HALO_PORT_GAME_STATE_CPU_SIZE 0x305000 /* (xbox) */
#define HALO_PORT_GAME_STATE_GPU_SIZE 0x40000 /* (xbox) */
#endif
#define HALO_PORT_GAME_STATE_SIZE (HALO_PORT_GAME_STATE_CPU_SIZE+HALO_PORT_GAME_STATE_GPU_SIZE)

/* ---------- textures

The texture cache holds the textures being drawn in 16 KB pages, 22 MB of
them on the Xbox, which Xbox maps were made to fit. Halo Custom Edition maps
were made for Halo PC, which has no such bound: a texture that does not fit
is drawn as the default one ("YOU GOT STABBED" in debug.txt; Elite_Alpha_Siege
did at 22 MB), and a frame of bigass_v3 draws more than 64 MB (DamnationCE's
measurement). The desktop builds' cache is 256 MB, half their 512 MB memory
window (port/linux/src/platform.h), whose pages are backed as they are used.
Android's window is 128 MB, as the web build's, and its cache the Xbox's. */

#if defined(HALO_XBOX_TARGET) && defined(OXBOX_STRIP)
/* oxbox: the stripped build with its .bss over the retail game's unused code and globals (port/oxbox/tools/gen_overlay.py)
has 7.6 MB free in a game with 0x280; 0x4AC (18.7 MB, test build 06: plus the PC-sized heap buffers and a 256 KB XNet pool) leaves about 0.7 MB in a system link game, retail Halo leaves 0.5 MB */
/* (test build 18: 0x536, 20.8 MB: Halo Universal's map features in (~94 KB), and 370 KB of sizes trimmed: script
help texts, the profiler's per-section history, the update rings, vehicle homes, light and lens flare caches, damage
caches and trails, the AI's avoidance debug record, the co-op host's device record allocated for a co-op host only)
   (test build 14: 0x526, 20.6 MB: OpenCE's switched-off PC extras out of the build, the profiler's history: 96 KB)
   (test build 13: 0x520, 20.5 MB: the netcode's per-tick scratch sent in passes, host and client AI tables
sharing memory, co-op host tables allocated for a co-op host only, smaller per-object client tables: 400 KB more)
   (test build 10: 0x508, 20.1 MB: the Bink player loaded only for a movie, the PC menus, frame interpolation and
more debug buffers left out: 448 KB more)
   (test build 08: 0x4EC, 19.7 MB: asserts without their text, cold code compiled for size, smaller debug
buffers, and OpenCE code and data placed over the retail game's unused code as well: 1.1 MB more) */
/* (test build 30: 0x556, 21.3 MB: the AI's pools trimmed and the game state as big as its pools: 512 KB more) */
#define HALO_PORT_TEXTURE_CACHE_PAGE_COUNT 0x556
#elif defined(HALO_ARM64_GUEST) || defined(HALO_WEB)
#define HALO_PORT_TEXTURE_CACHE_PAGE_COUNT 0x280 /* (xbox: 0x580; the proof of concept frees 12 MB for its larger image) */
#else
#define HALO_PORT_TEXTURE_CACHE_PAGE_COUNT 0x280 /* (xbox: 0x580; the proof of concept frees 12 MB for its larger image) */
#endif
#define HALO_PORT_TEXTURE_CACHE_SIZE (HALO_PORT_TEXTURE_CACHE_PAGE_COUNT*0x4000) /* (0x1600000) */

/* ---------- sounds

The sound cache holds the sounds being played in 4 KB pages, 4 MB of them on
the Xbox. Halo Custom Edition maps' sounds, converted when they load
(port/linux/game/custom_edition_sounds.c), and those of sound tags loaded
over a map's (loose_sounds.c), are longer than the Xbox maps' and fill it (a
campaign map's dialogue and music did, "SOUND CACHE BLOWN" in debug.txt).
The desktop builds' cache is 16 MB; Android's and the web build's windows keep
the Xbox's. */

#if defined(HALO_ARM64_GUEST) || defined(HALO_WEB)
#define HALO_PORT_SOUND_CACHE_SIZE 0x400000 /* (xbox) */
#else
#define HALO_PORT_SOUND_CACHE_SIZE 0x400000 /* (xbox) */
#endif

/* ---------- AI

Network co-op adds enemies for its players (port/linux/game/coop_enemies.c):
the actors, and their knowledge of the units about them (props: with many
players, more each), have room for several times a level's own. */

#if defined(HALO_XBOX_TARGET) && defined(OXBOX_STRIP)
/* oxbox (test build 30): multiplayer's AI, a quarter of the Xbox's. Maps that use AI with care have a few dozen at
most (Precipice: 10 actors and 8 props at the peak, test build 24's measurement); past 64, an encounter's next
actor isn't made (actor_place: "cannot create actor", its unit deleted). Distributed-netcode clients make no actors
at all. 538 KB less game state (the actors 351 KB, the props 176 KB, the swarm components 8 KB), which goes to the
texture cache; network co-op (coop_enemies.c) has no room for its extra enemies at this size. */
#define HALO_PORT_MAXIMUM_ACTORS 64 /* (xbox: 256) */
#define HALO_PORT_MAXIMUM_PROPS 192 /* (xbox: 768) */
#define HALO_PORT_MAXIMUM_SWARMS 32 /* (xbox) */
#define HALO_PORT_MAXIMUM_SWARM_COMPONENTS 128 /* (xbox: 256) */
#else
#define HALO_PORT_MAXIMUM_ACTORS 256 /* (xbox) */
#define HALO_PORT_MAXIMUM_PROPS 768 /* (xbox) */
#define HALO_PORT_MAXIMUM_SWARMS 32 /* (xbox) */
#define HALO_PORT_MAXIMUM_SWARM_COMPONENTS 256 /* (xbox) */
#endif

/* ---------- objects */

#define HALO_PORT_MAXIMUM_OBJECTS_PER_MAP 2048 /* (xbox) */
#define HALO_PORT_OBJECT_MEMORY_POOL_SIZE 0x100000 /* (xbox) */
/* each of the two reference lists of every cluster partition (collideable
objects, noncollideable objects, lights) */
#define HALO_PORT_MAXIMUM_CLUSTER_REFERENCES 2048 /* (xbox) */
#define HALO_PORT_MAXIMUM_RENDERED_OBJECTS 256 /* (xbox) */
#define HALO_PORT_MAXIMUM_CACHED_OBJECT_RENDER_STATES 256 /* (xbox) */
/* objects one explosion can damage */
#define HALO_PORT_MAXIMUM_AREA_OF_EFFECT_OBJECTS 64 /* (xbox) */
/* script object lists, and the object references they all share. The
lists of a tick's scripts are freed after it (object_list_gc): a Halo PC
map whose scripts test (players) in many places a tick took more than the
Xbox's 48 (coldsnap's), and its game halted */
#define HALO_PORT_MAXIMUM_OBJECT_LISTS_PER_MAP 48 /* (xbox) */
#define HALO_PORT_MAXIMUM_LISTED_OBJECTS_PER_MAP 128 /* (xbox) */
/* widgets (light volumes, antennas, flags, glows, lightning), each made with
its object and kept for its life: an assault rifle's flashlight beam, held or
dropped, and a plasma bolt's light volume; a full pool draws the object
without its widget */
#define HALO_PORT_MAXIMUM_WIDGETS 64 /* (xbox) */
#define HALO_PORT_MAXIMUM_LIGHT_VOLUMES 256 /* (xbox) */

/* ---------- effects, particles, lights and sounds */

#define HALO_PORT_MAXIMUM_EFFECTS 256 /* (xbox) */
#define HALO_PORT_MAXIMUM_EFFECT_LOCATIONS 512 /* (xbox) */
#define HALO_PORT_MAXIMUM_PARTICLES 1024 /* (xbox) */
#define HALO_PORT_MAXIMUM_PARTICLE_SYSTEMS 64 /* (xbox) */
#define HALO_PORT_MAXIMUM_SYSTEM_PARTICLES 512 /* (xbox) */
#define HALO_PORT_MAXIMUM_CONTRAILS 256 /* (xbox) */
#define HALO_PORT_MAXIMUM_CONTRAIL_POINTS 1024 /* (xbox) */
#define HALO_PORT_MAXIMUM_LIGHTS_PER_MAP 896 /* (xbox) */
#define HALO_PORT_MAXIMUM_GAME_LOOPING_SOUNDS 1024 /* (xbox) */

/* ---------- rasterizer

A frame's transparent geometry groups: each transparent part of a model drawn,
each widget drawn (light volumes, antennas, flags, glows), each batch of
dynamic geometry (particles, contrails), each transparent surface of the
structure. A full pool draws the frame without the
rest ("too many transparent geometry groups" in debug.txt): a b30 co-op game
with eight times the enemies asked for 464 in a frame. Each group is 160
bytes */
#define HALO_PORT_MAXIMUM_TRANSPARENT_GEOMETRY_GROUPS 384 /* (xbox) */

#endif /* __HALO_PORT_CAPACITY_H */
