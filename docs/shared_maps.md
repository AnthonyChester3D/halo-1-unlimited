# Shared files: maps that store their textures and sounds once

For Halo Map Porter (and anything else that writes maps for Halo 1 Unlimited on the Xbox). Version 1, game build v1.93.

A folder map can keep its textures' and sounds' data in a **shared file** instead of in itself, so maps that use the
same data (a campaign split into pieces, a multiplayer pack) store it once. Only that data moves: the map keeps its tag
data, BSPs and everything else, so a shared file costs no memory while playing (textures and sounds are read from it
as they're needed, as they are from a map).

Measured on CEX (13 maps, 289 MB): 75% of the texture and sound data is in more than one map; with one shared file
the pack would take roughly 100-120 MB.

## The shared file: `<name>.shared`

A cache file exactly like an Xbox map, with no tags:

| Header field (offset) | Value |
|---|---|
| signature (0x000) | `'head'` (bytes `daeh`) |
| version (0x004) | 5 |
| file_length (0x008) | the whole file's length unpacked, header included (at most 0x11600000: 278 MiB) |
| tag_data_offset, tag_data_size (0x010, 0x014) | 0, 0 |
| name (0x020) | the file's name without `.shared` (at most 30 characters, none of `\ / : * ? " < > \|`) |
| build (0x040) | anything (`shared`) |
| scenario_type (0x060) | 0 (campaign: it goes in a campaign cache file, the only ones big enough) |
| checksum (0x064) | a checksum of its contents (CRC32 of everything after the header): the maps name the version they were built against with it |
| footer (0x7FC) | `'foot'` (bytes `toof`) |

After the 2048-byte header, the rest of the file compressed as a map's is (one zlib stream), padded with zeros to a
whole 2048-byte sector. Unpacked, its data is at offsets you choose; give each block the same remainder modulo 512 as
the reads that use it need (0 for anything sector aligned, as Halo's own maps are).

**Where it goes**: beside the map, or in the folder above the map's folder (a pack's folder holding its maps' folders,
or `maps\` itself). The game looks in that order.

## The map

1. **Offsets**: a bitmap's `pixels_offset` (bitmap_data + 0x18) or a sound permutation's samples `file_offset`
   (permutation + 0x48) with its **top bit set** is an offset into the shared file (unpacked), the rest of the bits
   the offset. Any other offset is the map's own, as now. A map may mix both (data used by one map only can stay in it).
   On the Xbox the bitmap tag's own pixel data (bitmap + 0x30) is empty: each bitmap_data's offset is the file's.
2. **The reference**, in the map's header at **0x700** (52 bytes, the header's otherwise unused bytes):

| Offset in the reference | Size | Value |
|---|---|---|
| 0x00 | 4 | `'shrd'` (the bytes `drhs`, as `'head'` is stored) |
| 0x04 | 4 | 1 (this version) |
| 0x08 | 32 | the shared file's name, without `.shared`, zero-terminated |
| 0x28 | 4 | the shared file's checksum (its header's 0x064) |
| 0x2C | 4 | the shared file's file_length (its header's 0x008) |
| 0x30 | 4 | the map's identity: the size of the map built **without** a shared file (the standalone build) |
| 0x34 | 4 | and that build's header checksum (0x064) |

3. **Leave the moved data out** of the map (lay its own offsets out without it). The test tool zeroes it instead
   (zeros compress to nothing), but a map left at its old size unpacked may be too big for a multiplayer cache file
   (47 MiB) and take a campaign one.

## What the game does

- Loading a map with a reference: the shared file is copied into a campaign cache file first (once per launch; the
  next map drawing on it finds it there), then the map. Both cache files are kept while it plays.
- A shared file that's missing, or cached in another version (checksum or length different), is copied again; one
  that can't be found fails the map's load with the log line `shared: <map> draws on <name>.shared, which is neither
  beside it nor in the folder above`.
- A read past the shared file's end, or from a map without a reference, fails (the texture or sound doesn't load) and
  says so in the log.
- **Map sharing**: the map's identity is the standalone build's (the reference's last two fields), so a console with
  the shared build and one with the standalone build play together without a download. Sending a shared build to a
  console that has neither isn't supported yet: the host sends the map without its shared file, and it fails to load
  there with the message above.

## Tools

`port/oxbox/tools/shared_pass.py NAME OUT_DIR MAP...` makes a shared file and the maps drawing on it from finished
maps: a reference implementation of all of the above (except leaving the data out), and what the game was tested with
(two CEX maps in xemu, each playing from one CEX.shared).

## Not yet

- Map sharing sending the shared file.
- Shared tag data (the 10% of a map that's loaded into memory): much harder (it has to load at a fixed address) and
  only safe when every map loading it uses all of it.
- More than one shared file per map (a level's and a campaign's): one per map for now.
