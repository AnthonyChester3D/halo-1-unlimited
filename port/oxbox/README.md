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
  drive vehicles (a Warthog's driver waits for a gunner) and fire their guns (a Ghost's, a Banshee's or a
  Scorpion's as its driver, a Warthog's as its gunner), man turrets, ride along in teammates' vehicles, and leave
  to make room when a person joins.
- **END GAME.** The host has an END GAME item in the pause menu.
- **Memory.** The texture cache borrows whatever part of Halo's 22 MB of tag memory the map leaves unused, from
  map load until the map closes. That's 12 to 14 MB on stock-sized maps. On a 128 MB console the texture cache
  grows to as much as 60 MB and the sound cache to 8 MB.
- **Detail levels.** A model's level of detail comes from its straight-line distance to the camera, not its depth
  in the view, so models don't change detail, or vanish at their lowest one, as the camera turns.
- **Logs.** It writes `debug.txt` and `oxbox_log.txt` beside the XBE, or in `T:` when the XBE runs from a
  disc or ISO. They record the kernel version, memory, map sharing, and any frame of 100 ms or more. A thread of
  its own writes `oxbox_log.txt` out (every 100 ms, to the disk every 2 s): the game thread only appends to a ring
  in memory, so a log line never waits on the hard drive.
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

## Online (the online branch's test builds)

Built with `OXBOX_EXTRA="-DOXBOX_NET_TEST -DOXBOX_NET_LWIP -DOXBOX_NET_P2P"`:

- **Its own network stack.** lwIP (`port/third_party/lwip`) runs on the Xbox's network card (nxdk's driver,
  `port/third_party/nxdk_net`) in place of Halo's XNet, which can't reach the internet. System link and map sharing
  run on it too, so these consoles play each other only, not XNet builds or retail Halo.
- **Internet play, Xbox to Xbox.** OpenCE's internet play as our fork has it (`port/linux/src/p2p*.c`): MQTT brokers
  for finding each other, STUN and hole punching through home routers, an encrypted UDP tunnel, and KCP carrying
  connections. Its platform calls are in `net/`.
- **ONLINE PLAY** (`port/linux/game/hmp_online.c`): a Multiplayer item under SYSTEM LINK PLAY that opens the same
  screens, with ONLINE GAMES at the top of the games list. A game hosted there is listed for other online Xboxes (on
  their own topics and version: PC builds don't see them); the list shows those games as "@ name", A reaches the
  host, and its game then joins as on a LAN. Map sharing works through the tunnel. SYSTEM LINK PLAY is the LAN alone:
  it lists no internet games, and a game hosted there isn't listed online.
- **UPnP** (`net/oxbox_upnp.c`): a console the internet reaches through its router asks the router to forward the
  tunnel's port, so consoles behind strict NATs can still reach it.
- **Settings** in `opence.ini`: `online = 0` turns internet play off (no ONLINE PLAY item); `online_upnp = 0` doesn't
  ask the router; `online_brokers`, `online_stun` (host:port lists) and `online_tunnel_port` change where it goes.
- The network test's lines (address, DNS, STUN, MQTT, ONLINE) are in `oxbox_log.txt`; `net_test_screen = 1` in
  `opence.ini` shows them on the main menu too.
- Third-party licences: lwIP (BSD-3-Clause), nxdk's driver and port (MIT, BSD-3-Clause) and KCP (MIT), each beside
  its code and in the test builds' `OpenCE_online_licenses.txt`; Monocypher under its CC0 option.

## The showcase screensaver (a test build switch)

Built with `-DOXBOX_SHOWCASE` added to `OXBOX_EXTRA` (`port/linux/game/showcase.c`):

- When the main menu has gone 75 seconds without a press, instead of an attract movie the Xbox hosts a closed
  system link game on a folder map that has a `showcase.txt` (Halo Map Porter writes one beside the map's
  `info.txt` and `preview.bin`), or a stock map that has a `maps\showcase\<its name>.txt` (`bloodgulch.txt`,
  `beavercreek.txt`: the same format), with bots, and flies the file's camera shots over the map while they play. There's
  no HUD, the watching player never spawns, and the game isn't advertised, listed or joinable, nor counted in the
  play stats. Any button fades it out and brings the main menu back.
- The file: `fade S`, then shots (`shot SECONDS label`, an optional `look X Y Z`, and keys
  `key T X Y Z YAW PITCH [FOV]`). Between keys the camera follows the same curves as the porter's preview (Hermite
  with Catmull-Rom tangents). Shots fade out over their last `fade` seconds and in over the next one's first.
- Maps: favourites first, each group shuffled, hidden maps skipped. A map's list plays through (for at least a
  minute), then the next map loads (through the main menu, as every system link game does, with no title music).
  CTF where the map has CTF (its card's "Gametypes:" line, as Halo Map Porter writes it; every stock map), else Team
  Slayer, to 999, with every vehicle the map places, bots at Normal (the map's suggested players, 4 to 10, an even number of bots), no announcer.
  While the game runs under 15 frames a second, bots drop out one at a time (while dead, so none vanishes on screen),
  down to two; each map starts with the full count again.
- A map's first ten seconds have a title card, as the campaign's chapter titles do: black bars top and bottom and the
  map's name (its `info.txt` name) bottom right, in the chapter titles' font and place.
- Between maps, the lobby starts on the next map rather than the default (Carousel), and starts as soon as the bots
  are in (they join faster than in a person's lobby).
- It plays without the network cable too (the game is the console's alone, as a split screen game is).
- Settings in `opence.ini`: `showcase = 0` plays the attract movies instead; `showcase_seconds = N` (5 and up)
  changes the 75-second wait; `showcase_bots = N` (0 to 15) sets its bots; `showcase_min_fps = N` the frame rate
  under which bots drop out (15; 0: never); `showcase_map_seconds = N` how long each map plays at most (90; 0: no
  limit). A map with more shots than fit plays its first, a random pick of the others, and its last.
- Without the switch, the build is byte for byte what it was.

## Bot tuning (a test build switch)

Built with `-DOXBOX_BOT_TUNING` added to `OXBOX_EXTRA` (`port/linux/game/bots.c`):

- A bot's periodic looks (its goal, a better weapon, an item, a vehicle, the look ahead, a look round) start at its
  own point in their intervals when it spawns. Bots that spawned together, as all of them do when a game starts,
  did each of these on the same tick for the rest of their lives.
- A bot standing on the power weapon it walked to holds the action button to take it (with two weapons already,
  walking over it takes only its ammunition).
- A bot carrying a flag or a ball forgets the weapon, item or vehicle it was going for, and doesn't look for others.
- A bot driving a vehicle plans no route through a teleporter, nor onto one's way in.
- A hop to a waypoint well above or below searches the whole 60 world units for the way (a ramp or a walkway round
  to a raised flag stand), not twice the straight distance.
- A team game's bots keep the teams even in the lobby: while the teams differ by more than a player (a person
  changing team, or leaving), a bot moves from the bigger team to the smaller (`network_server_manager.c`).
- Without the switch, the build is byte for byte what it was.

## System link without a network cable (a test build switch)

Built with `-DOXBOX_OFFLINE_SYSTEM_LINK` added to `OXBOX_EXTRA`: SYSTEM LINK PLAY opens without the cable (it isn't
dimmed, and no "network connection lost" box), so an Xbox that isn't on a network can host a game alone, with bots,
as it plays split screen. A host plays its own game through its own address (127.0.0.1), and bots need no network.
The network checks that end a game when the cable is out (`network_client_manager.c`, `network_server_manager.c`)
pass for a console that hosts or is looking for games (`network_game_offline_allowed`); a console in another's game
still leaves it when its network goes. With the cable in, nothing changes, except that a host whose cable comes out
in a game plays on (the other consoles drop out on their own time-outs).

Since Halo's box no longer says it, the player is told when the network isn't there (`port/linux/game/net_status.c`):
the games list, while it lists no game, adds why under "Looking for games..." (no network connection; connecting to
the network; in ONLINE PLAY no router, connecting to the online servers, or can't reach them); a host's lobby says "No
network connection" in its countdown bar; and in a game it hosts, the network going or coming back is a HUD message.
When the network is back, internet play reconnects to its brokers at once instead of at their next retry.

## What isn't here

The game data isn't in this repository, and the Halo XBE the build starts from isn't either. That's the same
as upstream OpenCE, which asks for your own disc image. Other files are also left out:

- **`port/oxbox/pictures.bin`** holds the main menu's OPTIONS and PROFILES words and the postgame's gametype pictures,
  and **`port/oxbox/online_games.dxt3`** (online builds only) ONLINE PLAY's ONLINE GAMES header. They're made from the
  letters and pictures in your own `ui.map` by `installer/mkpictures.c` (`UI_MAP=path/to/ui.map` when you build).
  Without them, those pictures are blank.
- **DirectSound's effects program** is taken at build time from OpenCE's own copy of it, in
  `source/sound/sound_dsound_xbox.c`.

## Building

You need Linux (or WSL), plus clang, ld.lld, llvm-objcopy, ninja, python3 and a 32-bit libgcc (on Debian or
Ubuntu, the `gcc-multilib` package provides it).

The input is your own retail Halo XBE, the `default.xbe` from the disc. The NTSC discs have three, with the same
code and different signatures: version 1 (SHA-256 `ed3a8e962351ad6c4b3b620768fb6a0bda658963390252439ea036d5ede3a3ac`),
version 8 (`e0eca392e4aa0c174e60329a7a6799de6c1b18292cddb1905e46c20ca2fbfdde`) and version 9
(`4094e994243ddeae3f1b478bde6a7ee81498218ccd7c9d7bc2327db547d95aae`). A copy patched to run from the hard disk or in any
region (its certificate's media and regions changed) builds the same game. (The early builds started from a Halo XBE
with a 16-console system link patch, `default_16Xbox.xbe`, SHA-256
`473b2c3ae3d503edd27f48e5844fe32209904cd0f505a76845860f08f4e13d89`, which still works.) The build keeps that XBE's
sections where they are. It uses the XBE's own libraries (Direct3D, DirectSound, XNet), links the game code against
them, adds that code after its last section, and points the startup at it.

```sh
UI_MAP=path/to/maps/ui.map ./port/oxbox/build.sh path/to/default.xbe
```

The output is `build-xbox/halo1unlimited.xbe`. Copy it to the Xbox in place of the game's `default.xbe`,
next to the game's `maps` folder. It holds the retail game's code, so it isn't something to share: a release is the
installer and a package instead (`installer/README.md`), which make the same XBE on the player's Xbox from their
own retail XBE.

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
- `tools/mkxbe.py` writes the XBE: it adds the image (and the empty `.cap16` section, on a retail XBE), redirects
  `main`, appends the effects program and the pictures, and fixes up the section digests.
- `installer/` is the installer XBE, and `tools/mkpackage.py` makes its packages. The installer downloads the
  latest signed release's package from GitHub (Y) and updates itself from a signed package that carries a newer
  installer (`installer/README.md`).

## Credits

This builds on [OpenCE](https://github.com/OpenCommunityEdition/OpenCE), the port of the Halo: Combat Evolved
decompilation by [bnunu/halo-1](https://github.com/bnunu/halo-1), which was forked from
[punpckhdq/halo](https://github.com/punpckhdq/halo). The map features come from the Halo Universal XBE. Maps for
it are made with [Halo Map Porter](https://github.com/AnthonyChester3D/halo-map-porter).
