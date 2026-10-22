# Halo 1 Unlimited: OpenCE on the original Xbox

This folder builds OpenCE's game code for the original Xbox. The result is an XBE that plays like stock Xbox
Halo, with OpenCE's netcode underneath it and the map features of the Halo Universal XBE on top.

## What it does

- **OpenCE's netcode on system link.** Each console predicts its own player and the host decides. Consoles can
  join a game in progress, and up to 16 consoles can play. The host also sends the AI to every console, so maps
  with AI play the same for everyone.
- **Folder maps.** Maps go in `maps\<Name>\` on the hard drive or in `T:\maps`, with an optional `info.txt` and
  preview picture. They show up in a map browser that has sorts, favourites, hidden maps and play counts.
- **Map sharing.** A console that lacks the host's map downloads it in the lobby (TCP port 5152), over up to
  three connections. The countdown waits for it. A console joining a game in progress downloads the map first,
  then joins.
- **END GAME.** The host has an END GAME item in the pause menu.
- **Memory.** The texture cache borrows whatever part of Halo's 22 MB of tag memory the map leaves unused, from
  map load until the map closes. That's 12 to 14 MB on stock-sized maps. On a 128 MB console the texture cache
  grows to as much as 60 MB and the sound cache to 8 MB.
- **Logs.** It writes `debug.txt` and `oxbox_log.txt` beside the XBE, or in `T:` when the XBE runs from a
  disc or ISO. They record the kernel version, memory, map sharing, and any frame of 100 ms or more.
- **Settings file.** An `opence.ini` next to the XBE, or `T:\opence.ini`, holds one `name = number` per line:
  - `audio_effects = 0` skips DirectSound's effects program.
  - `fps_counter = 1` shows the frame rate in the bottom right corner (off by default).

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
