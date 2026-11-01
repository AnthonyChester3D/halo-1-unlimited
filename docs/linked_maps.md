# Linked maps

A dedicated server can play a Custom Edition map with Xboxes: the server and its PCs load the Custom Edition map
(`custom_maps\<name>.map`), each Xbox loads that map's Xbox port (a folder map, `maps\<folder>\`). A link file says
the two are the same game and gives what the server needs to translate between them.

## Setting one up

1. Put the Custom Edition map in the server's `custom_maps` folder (`paths.custom_maps`), and its Xbox port in the
   server's maps folder as a folder map: `maps\<folder>\` with the `.map`, its info `.txt` and `preview.bin`.
2. Put the link file (`<name>.h1ulink`) in the same folder as the Xbox port's `.map`.
3. Put the Custom Edition map in the rotation as usual (`custom_maps\<name>:ctf`).

When the server's game is on that map, its log says `linked maps: custom_maps\<name> (version ...) is linked to the
Xbox map +<folder>`. Xboxes then see the game (with the Xbox beside it), join it, and download the Xbox port from the
server if they haven't got it. If the log says `no link`, the link names another build of the Custom Edition map (its
hash), or the folder's `.map` isn't the build the link names.

## The link file

Plain text, one fact per line, `#` comments. Halo Map Porter writes it ("Perfect Conversion", with its pair checker's
`check` lines); `tools/h1ulink.py CE_MAP XBOX_MAP` writes one with the identities and the tag table only, for testing.

```
h1u-link 1
tool <what wrote it>
ce   custom_maps\<name>   <size>   <hash>
xbox <folder>\<file>.map   <size>   <hash>
level ce   <the CE map's scenario path>
level xbox <the Xbox map's scenario path>
placed <type> <count>               (one per placement block)
tag <group> <CE tag ID> <Xbox tag ID> <path>    (hex, - where the map hasn't it)
check <name> <result>
```

The hash is map sharing's (`port/linux/game/map_share_files.c`): BLAKE2b-256 of the 12 bytes `OpenCE map 1`, the
file's size (4 bytes, little-endian), and the BLAKE2b-256 of each 256 KiB piece in order, as 64 lowercase hex digits.
Its first 32 bits are the version the game's settings carry.

## What the server does (port/linux/game/linked_maps.c)

- An Xbox's copy of the game's settings names the Xbox map ("+folder") at its version as the Xbox works it out
  (its size and Halo Map Porter's header checksum): it plays its own copy, or downloads it from the server
  (`map_share_xbox.c`, TCP 5152).
- The tag IDs in the server's messages to an Xbox are the Xbox map's, and those in an Xbox's to the server the Custom
  Edition map's: objects made, pickups, damage, hit reports, and the scripts' messages.
- The scripts run on the server alone (as `server.host_scripts` does): the two copies may carry different scripts
  (Halo Map Porter optimizes the Xbox's). The Xboxes run none of them; PCs of other builds run their own copy, as
  before.

Everything else the game sends names objects by their datum index, which is the same on every machine only if both
maps place the same objects in the same order, with the same gameplay data: that's what Perfect Conversion and its
pair checker guarantee, and why a link written by `tools/h1ulink.py` (which checks none of it) is for tests.
