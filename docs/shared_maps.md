# Shared files: maps that store their textures and sounds once

For Halo Map Porter (and anything else that writes maps for Halo 1 Unlimited on the Xbox). Format version 2, game
build v1.94 (version 1 maps, from v1.93, still load).

A map can keep its textures' and sounds' data in **shared files** instead of in itself, so maps that use the same data
store it once. Only that data moves: the map keeps its tag data, BSPs and everything else, so shared files cost no
memory while playing (textures and sounds are read from them as they're needed, as they are from a map).

A map draws on up to two:

- **its pack's** `<name>.shared`: data two or more of the pack's maps use;
- **`stock.shared`**: every texture and sound of the 13 stock multiplayer maps, built by the Halo 1 Unlimited
  installer from the player's own disc (and by this repo's tool from the same maps: the layout is fixed, below). Every
  player has it, so a pack never carries stock content.

Measured: the stock maps go from 269 MB to 78 MB (each 2.7-4 MB, stock.shared 41 MB); 33-60 MB of each of HMP's packs
is stock content.

## A shared file

A cache file exactly like an Xbox map, with no tags:

| Header field (offset) | Value |
|---|---|
| signature (0x000) | `'head'` (bytes `daeh`) |
| version (0x004) | 5 |
| file_length (0x008) | the whole file's length unpacked, header included (at most 0x11600000: 278 MiB) |
| tag_data_offset, tag_data_size (0x010, 0x014) | 0, 0 |
| name (0x020) | the file's name without `.shared` (1 to 30 characters, none of `\ / : * ? " < > \|`) |
| build (0x040) | anything (`shared`) |
| scenario_type (0x060) | 0 (campaign: it goes in a campaign cache file, the only ones big enough) |
| checksum (0x064) | CRC32 of everything after the header, unpacked: the maps name the version they were built against with it |
| footer (0x7FC) | `'foot'` (bytes `toof`) |

After the 2048-byte header, the rest of the file compressed as a map's is (one zlib stream), the file padded with zeros
to a whole 2048-byte sector. Give each block the same offset remainder modulo 512 as it had in its map (the game reads
whole sectors).

**Where it goes**: beside the map, or in the folder above the map's folder (a pack's folder holding its maps' folders,
or `maps\` itself); `stock.shared` may also be in the game's maps folder. The game looks in that order.

## The map

1. **Offsets**: a bitmap_data's `pixels_offset` (bitmap_data + 0x18) or a sound permutation's samples `file_offset`
   (permutation + 0x48) with its **top bit set** is an offset into a shared file (unpacked): bit 30 clear for the
   pack's (`0x80000000 | offset`), set for `stock.shared` (`0xC0000000 | offset`); offsets below 1 GiB. Any other
   offset is the map's own. On the Xbox the bitmap tag's own pixel data (bitmap + 0x30) is empty: each bitmap_data's
   offset is the file's.
2. **The reference**, in the map's header at **0x700** (92 bytes):

| Offset | Size | Value |
|---|---|---|
| 0x00 | 4 | `'shrd'` (the bytes `drhs`, as `'head'` is stored) |
| 0x04 | 4 | 2 (version 1 has the first 0x38 bytes alone, the pack's file only) |
| 0x08 | 32 | the pack's shared file's name, without `.shared`, zero-terminated (all zeros: none) |
| 0x28 | 4 | its checksum (its header's 0x064) |
| 0x2C | 4 | its file_length (its header's 0x008) |
| 0x30 | 4 | the map's identity: the size of the map built **without** shared files |
| 0x34 | 4 | and that build's header checksum (0x064) |
| 0x38 | 32 | the stock shared file's name: `stock` (all zeros: none) |
| 0x58 | 4 | its checksum |
| 0x5C | 4 | its file_length |

3. **Leave the moved data out** of the map: lay the map out again without it, moving the offsets of what stays down
   (the header's tag_data_offset, each BSP's file offset in the scenario's structure_bsp_references, and the textures
   and sounds that stay). A map must fit a multiplayer cache file unpacked (47 MiB). The tool drops every 2048-byte
   sector made wholly of moved data, so what stays keeps its alignment, and zeroes the rest of the moved bytes.

**Rules for a pack's pass**: only data used by two or more of its maps goes into its shared file (data one map uses
stays in that map: shared files stay under 278 MiB, and a deleted map takes its own data with it); data in
stock.shared is taken from there, never put in the pack's file.

## stock.shared's layout

Fixed, so every console and every PC building it from the same maps gets the same file (checksum `c028f8bf`, file_length
78260224 from the retail maps): the 13 stock multiplayer maps (`beavercreek`, `bloodgulch`, `boardingaction`,
`carousel`, `chillout`, `damnation`, `hangemhigh`, `longest`, `prisoner`, `putput`, `ratrace`, `sidewinder`, `wizard`)
in the order of their file names, lower case; in each, the tags in tag index order; a bitmap's bitmap_datas in block order, a sound's pitch ranges'
permutations in block order; each block's first copy (the same bytes and offset remainder modulo 512) placed after the
last, padded with zeros up to its remainder modulo 512; the file padded to a whole sector. `port/oxbox/tools/shared_pass.py`
is the reference: a pack built against stock.shared with `--stock <the stock maps' folder>` lays it out the same way.

## What the game does

- Loading a map with a reference: each shared file it draws on is copied into a campaign cache file first (once per
  launch; the next map drawing on it finds it there), then the map. They stay cached while it plays. (The two
  campaign cache files hold the two shared files; the map then goes in a multiplayer one.)
- A shared file that's missing, or cached in another version (checksum or length different), is copied again; one
  that can't be found fails the map's load with the log line `shared: <map> draws on <name>.shared, which is neither
  beside it nor in the folder above`.
- A read past a shared file's end, or from one the map doesn't name, fails (the texture or sound doesn't load) and says
  so in the log.
- A stock map in the maps folder may draw on stock.shared too.
- **Map sharing**: the map's identity is its standalone build's (the reference's 0x30 and 0x34), so a console with the
  shared build and one with the standalone build play together without a download. A copy whose shared files aren't
  all here has no identity: it can't load, so it's treated as missing.

## Map sharing

A console joining a game on a map it hasn't asks the host for it (TCP 5152, version 7), and a map drawing on shared files
takes three steps:

1. The host answers `REP_SHARED`: the shared files' names and versions (checksum and file_length) and the sizes of the
   map's files. Nothing is sent yet.
2. **A pack's file** the console lacks in that exact version comes first, as a download of its own: resumable, its blocks
   CRC-checked like a map's, its header checked against the version named, then put in the folder above the map's. It
   only ever sends the file the hosted map names, and never one called `stock`.
3. **The map**, as for any other. A console whose copy lacked only the shared file is the same build now and gets no map.

A pack's file is named for its version (`<pack>-<8 hex of its checksum>.shared`), so two versions of a pack sit side by
side. **`stock.shared` is never sent**: every console builds its own from its own maps. One without it, or with another
version, stays in the lobby with `No stock.shared: run the installer`, as one with no room for a map does.

The room asked for, and what KEEP DOWNLOADS UNDER counts for the map that fetched it, include the pack's file. When a
map is deleted, or replaced by a copy that doesn't use it, its pack's file goes too if no other map in the list draws on
it. A replaced copy kept as `.hmp_prev` keeps its file while it stays, and that copy's deletion doesn't look for it.

## The installer

After an install (never in the game's own updates) the installer offers to swap a Halo folder's 13 stock maps for small
ones drawing on `maps\stock.shared`, built from the player's own maps (`port/oxbox/installer/stockmaps.c`; about 190 MB
saved). Only the retail maps are swapped, each checked by its file's size and CRC, so every console's stock.shared is
the same file. The small maps and stock.shared are the same, byte for byte, as `shared_pass.py stock` makes.

**X** (*retail Halo back*) puts them back, the exact files: `maps\stock.restore` holds each map's restore table (its
header, each moved block's place and its place in stock.shared, its BSPs' offsets, and the original file's size and
CRC), and retail maps recompress to their own bytes with zlib level 6, memLevel 8. The stock maps come back before the
retail XBE does: if there's no room for them, nothing changes, and the folder still plays Halo 1 Unlimited. stock.shared
goes with them, so a map pack drawing on it needs another install's offer to play again.

Nothing is touched until everything is built and proved on that console: each retail map is recompressed to its own
bytes, each small map is restored from the files on the disk and checked against the original's CRC, and stock.shared
and the table are read back. A power cut can't lose a map: before `stock.restore` goes in place the retail maps are
untouched and the leftovers (`stock.unpacked`, `*.oxtmp`, `*.oxnew`) are deleted; after it, the next look at the folder
finishes the swap. It needs about 205 MB free for a while; putting the maps back needs about 300 MB.

## Tools

`port/oxbox/installer/stockmaps_test.c`: the installer's own code on a PC, for testing a folder of stock maps.

`port/oxbox/tools/shared_pass.py`:

- `shared_pass.py stock OUT_DIR STOCK_MAP...`: stock.shared and the stock maps drawing on it.
- `shared_pass.py [--stock STOCK_MAPS_DIR] NAME OUT_DIR MAP...`: a pack's NAME.shared and its maps.

Tested in xemu (v1.94): Blood Gulch from stock.shared; CEX's Abyss from CEX.shared and stock.shared together.

## Not yet

- Shared tag data (loaded into memory at a fixed address): much harder, and only safe when every map loading it uses
  all of it.
