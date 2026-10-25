# Halo 1 Unlimited: OpenCE on the original Xbox

This folder builds OpenCE's game code for the original Xbox. The result is an XBE that plays like stock Xbox
Halo, with OpenCE's netcode underneath it and the map features of the Halo Universal XBE on top.

## What it does

- **OpenCE's netcode on system link.** Each console predicts its own player and the host decides. Consoles can
  join a game in progress, and up to 16 consoles can play. The host also sends the AI to every console, so maps
  with AI play the same for everyone. Up to 64 AI are alive at once (a quarter of the Xbox game's pool, which
  leaves 512 KB more for textures); past that, an encounter's next AI isn't made.
- **Folder maps.** Maps go in `maps\<Name>\` on the hard drive or in `T:\maps`, with an optional `info.txt` and
  preview picture. They show up in a map browser that has sorts, favourites, hidden maps and play counts.
- **Map sharing.** A console that lacks the host's map downloads it in the lobby (TCP port 5152), over up to
  three connections. The countdown waits for it. A console joining a game in progress downloads the map first,
  then joins.
- **Bots.** In the system link lobby the host presses Y to pick a bot mode: Off, Suggested (the map's
  suggested player count, or 8), Max (16 players) or Custom (RT and LT set the count), and Black for how well
  they fight (Easy, Normal, Hard, Legendary). Bots are named BOT plus one of Halo's random names. They walk the
  whole map with Halo's own AI pathfinding, take teleporters, crouch under low gaps, jump and crouch-jump onto
  ledges, fight (spotting, aiming, bursts, weapon choice and pickups, grenades, melee; only the other team in team
  games), switch to their other weapon and reload, scan with a sniper's scope, dodge vehicles coming at them, go
  for power weapons and powerups, back off when hurt, play the objective (CTF, Oddball, King of the Hill, Race),
  drive vehicles (a Warthog's driver waits for a gunner), man guns and turrets, ride along in teammates' vehicles,
  and leave to make room when a person joins.
- **END GAME.** The host has an END GAME item in the pause menu.
- **Memory.** The texture cache borrows whatever part of Halo's 22 MB of tag memory the map leaves unused, from
  map load until the map closes. That's 12 to 14 MB on stock-sized maps. On a 128 MB console the texture cache
  grows to as much as 60 MB and the sound cache to 8 MB.
- **Detail levels.** A model's level of detail comes from its straight-line distance to the camera, not its depth
  in the view, so models don't change detail, or vanish at their lowest one, as the camera turns.
- **Logs.** It writes `debug.txt` and `oxbox_log.txt` beside the XBE, or in `T:` when the XBE runs from a
  disc or ISO. They record the kernel version, memory, map sharing, and any frame of 100 ms or more.
- **Settings file.** An `opence.ini` next to the XBE, or `T:\opence.ini`, holds one `name = number` per line:
  - `audio_effects = 0` skips DirectSound's effects program.
  - `fps_counter = 1` shows the frame rate in the bottom right corner (off by default).
  - `bot_log = 1` turns on the bots' developer log (off by default): a line every 10 seconds in
    `oxbox_log.txt`, and `opence_bot_log.txt` beside it, listing the spots where bots got stuck.
  - `perf_log = 1` turns on the developers' performance and memory log (off by default). It writes a CSV file per
    map in `perf\` beside the XBE: memory at map load, a line a second (frame times split into game ticks,
    drawing, waiting on the graphics chip and the vertical blank; the graphics chip's busy share; free memory;
    texture reads; the camera's position, BSP and cluster), and a line for each frame over `perf_slow_frame_ms`
    (34 by default). `perf_gpu_sample_ms` (1 by default, 0 for none) sets how often the graphics chip is asked
    whether it's busy. Each second's line also has the time of every step of a game tick and of the render window
    (the visibility traversals, sky, objects, lightmaps, lights, decals, reflections, fog, particles, transparent
    geometry, HUD and more) and counts (objects drawn and updated, actors, effects, particles, dynamic lights, the
    level's triangles and clusters drawn, dynamic triangles used). It also splits up the rest of the frame (input,
    network, menus, the camera, between frames), the object updates (their steps, each object type's time and
    count, object_translate and the clusters it touches) and the HUD's parts; it names the three object tags that
    took the most update time; and it counts the weather's particles. Slow frames' lines also say what the log's
    writer thread was doing, and fill in the game tick's steps when the tick took over 25 ms.
    A camera tour measures the same views every run: a text file a map in `perf\tours`, named after the map's place
    in `maps` (`perf\tours\sidewinder.txt`, `perf\tours\unsorted\Precipice.txt` for `maps\unsorted\Precipice\`),
    lists stops (position, direction or "look around", optional label). A few seconds after the map loads, the view
    flies to each stop, waits for it to settle, measures it and writes a line for it (T) with the same columns.
    Meanwhile the player stands on the map's first spawn point (or a spot the file names), unhurt, so the game's cost
    around it is the same every run. Holding Back and clicking the right stick stops a tour, or, when none is
    running, adds the current view to the map's tour file. The log's code and buffers are in an XBE section of their
    own (OXPERF), loaded only when the setting is on: with it off, the game has the same free memory as without it.

Saves use retail Halo's formats and title ID, so profiles and gametypes are shared with the retail game.

## What isn't here

The game data isn't in this repository, and the Halo XBE the build starts from isn't either. That's the same
as upstream OpenCE, which asks for your own disc image. Two other files are also left out:

- **`port/oxbox/pictures.bin`** holds the main menu's OPTIONS and PROFILES words and the postgame's gametype pictures.
  They're made from the art in your own `ui.map` by `tools/mkword.py` and `tools/mkicons.py`. Without the
  file, those pictures are blank.
- **DirectSound's effects program** is taken at build time from OpenCE's own copy of it, in
  `source/sound/sound_dsound_xbox.c`.

## Building

You need Linux (or WSL), plus clang, ld.lld, llvm-objcopy, ninja, python3 and a 32-bit libgcc (on Debian or
Ubuntu, the `gcc-multilib` package provides it).

The input is the Halo XBE with the 16-console system link patch, `default_16Xbox.xbe`. Its SHA-256 is
`473b2c3ae3d503edd27f48e5844fe32209904cd0f505a76845860f08f4e13d89`. The build keeps that XBE's sections
where they are. It uses the XBE's own libraries (Direct3D, DirectSound, XNet), links the game code against
them, adds that code after its last section, and points the startup at it.

```sh
./port/oxbox/build.sh path/to/default_16Xbox.xbe
```

The output is `build-xbox/halo1unlimited.xbe`. Copy it to the Xbox in place of the game's `default.xbe`,
next to the game's `maps` folder.

Most of the game is compiled for size (`-Os`); the drawing, physics, collision, objects and maths folders are compiled
for speed (`-O2`). To compile more folders for speed, name them in `OXBOX_HOT_EXTRA`, for example
`OXBOX_HOT_EXTRA="units devices items ai interface"` (test build 33's experiment, which costs about 80 KB of
memory). Such a build's menus say "v1.33 O2", and so do its perf logs.

How the pieces fit:

- `tools/gen_xbox.py` writes the ninja file. It covers the game units (minus the PC-only ones), the port units
  this layer reuses, zlib, musl's maths, and this folder's C files.
- `link.ld` puts the image after the retail XBE's last section (`.cap16`, ending at 0x3B9000). `retail.ld`
  (made by `tools/gen_retail_ld.py` from `retail_syms.txt`) names the retail libraries' functions and data.
- `tools/gen_overlay.py` places OpenCE's larger globals over retail game code that nothing runs.
- `tools/mkxbe.py` writes the XBE: it adds the image, redirects `main`, appends the effects program and the
  pictures, and fixes up the section digests.

## Credits

This builds on [OpenCE](https://github.com/OpenCommunityEdition/OpenCE), the port of the Halo: Combat Evolved
decompilation by [bnunu/halo-1](https://github.com/bnunu/halo-1), which was forked from
[punpckhdq/halo](https://github.com/punpckhdq/halo). The map features come from the Halo Universal XBE. Maps for
it are made with [Halo Map Porter](https://github.com/YOUR-GITHUB-NAME/halo-map-porter).
