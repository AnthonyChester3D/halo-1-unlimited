# Shared files: maps that store their textures and sounds once

For Halo Map Porter (and anything else that writes maps for Halo 1 Unlimited on the Xbox). Format version 3, game
build v1.102 (installer 29): a pack's shared file carries its list (the maps it was made from, and its blocks), its maps are
laid out in the order of their identities, a small map is a whole number of sectors long, and its reference names its
original. Version 2 files still load (the game reads none of what's new), but only version 3 packs can be shrunk into and
shared as small copies. **stock.shared changed in v1.97**: packs built against the earlier one (checksum `c028f8bf`) must
be rebuilt.

A map can keep its textures' and sounds' data in **shared files** instead of in itself, so maps that use the same data
store it once. Only that data moves: the map keeps its tag data, BSPs and everything else, so shared files cost no
memory while playing (textures and sounds are read from them as they're needed, as they are from a map).

A map draws on up to two for its textures and sounds (and, format 4, on up to two for its tag data: below):

- **its pack's** `<name>.shared`: data two or more of the pack's maps use;
- **`stock.shared`**: every texture and sound of the 13 stock multiplayer maps, and those of the stock campaign that three
  levels or more use, built by the Halo 1 Unlimited installer from the player's own disc (and by this repo's tool from the
  same maps: the layout is fixed, below). Every player has it, so a pack never carries stock content.

Measured on the retail maps: the 23 maps (13 multiplayer, 10 campaign) go from 1761 MiB to 950 MiB, with stock.shared 228 MiB
of that (238.7 MB, within the 278 MiB limit). The levels draw 1137 MB from it. Data two levels use would make it 329 MB:
too big, so it stays in the levels. 33-60 MB of each of HMP's packs is stock multiplayer content.

## A shared file

A cache file exactly like an Xbox map, with no tags:

| Header field (offset) | Value |
|---|---|
| signature (0x000) | `'head'` (bytes `daeh`) |
| version (0x004) | 5 |
| file_length (0x008) | the whole file's length unpacked, header included (at most 0x11600000: 278 MiB) |
| tag_data_offset, tag_data_size (0x010, 0x014) | 0, 0 |
| list_length (0x018) | a pack's file: its list's length, after the header (a whole number of sectors); 0 for none (stock.shared) |
| name (0x020) | the file's name without `.shared` (1 to 30 characters, none of `\ / : * ? " < > \|`) |
| build (0x040) | anything (`shared`) |
| scenario_type (0x060) | 0 (campaign: it goes in a campaign cache file, the only ones big enough) |
| checksum (0x064) | CRC32 of everything after the header and the list, unpacked: the maps name the version they were built against with it (the list isn't in it: the list names each small map's CRC, and each small map names this) |
| footer (0x7FC) | `'foot'` (bytes `toof`) |

After the 2048-byte header, the rest of the file either as it is, or compressed as a map's is (one zlib stream); the file
padded with zeros to a whole 2048-byte sector. Give each block the same offset remainder modulo 512 as it had in its map
(the game reads whole sectors).

**A pack's file's list**, right after its header (list_length bytes, zeros after it), says what the file was made from, so a
console can tell which full-size maps it serves and shrink them into it (the installer's *Shrink maps*), and anything can tell
an official pack's file from another (a library of known packs, by these identities):

| Offset | Size | Value |
|---|---|---|
| 0x00 | 4 | `OXPL` |
| 0x04 | 4 | 1 (its version) |
| 0x08 | 4 | maps: how many it was made from |
| 0x0C | 4 | blocks: how many it holds |
| 0x10 | 20 a map | each map, in the order they're laid out in: its **identity** (the full-size map's file's size, the CRC32 of its first 64 KiB, the CRC32 of its last 64 KiB: all of it when it's smaller), then its small map's length and CRC32, unpacked |
| then | 40 a block | each block, in the order it's in the file: its SHA-256, its place in the file, its size |

**A map's identity** is its file as it's stored: size and the two CRC32s. It's unique across the 467 distinct maps of 28 old
packs, and quick to take (128 KB read); a map's header checksum isn't (many modded maps have 0).

**Unpacked is read where it is**: when the file's size is its file_length, the game reads it from the disk with no copy, no
wait and no cache file. stock.shared is always this way. A pack's file is packed: one up to 47 MiB unpacked is copied
into a multiplayer cache file; a bigger one into a seventh cache file, q:\cache006.map (688 MiB), on a second cache drive
the game takes (cache_files_windows.c oxbox_shared_cache_drive: the cache drives' list at the hard drive's 0x800, a title
ID of its own, 4C4A4853), so both of the campaign's stay for the level being left and the next (a campaign one would
leave a level change no room: Covenant Edition's 143 MB file did, on a console). Without that drive it goes in a
campaign one. Only one bigger than 688 MiB is left unpacked (the installer's consolidate.c and shared_pass.py).

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
| 0x60 | 4 | a pack's map: its original's identity's CRC32 of the first 64 KiB (with 0x30, its size; 0 when not known) |
| 0x64 | 4 | and of the last 64 KiB |

(The reference keeps version 2: what's at 0x60 is new in format 3, and nothing that reads a reference reads past 0x60.)

3. **Leave the moved data out** of the map: lay the map out again without it, moving the offsets of what stays down
   (the header's tag_data_offset, each BSP's file offset in the scenario's structure_bsp_references, and the textures
   and sounds that stay). A sector something that stays needs is kept, even if it's otherwise moved data: some
   maps have a block that stays, or a BSP, sharing a sector with one that moves. A small map must fit a campaign cache file unpacked (278 MiB): a multiplayer one over 47 MiB goes in a campaign cache file, as a full-size one does (its pack's file is in a multiplayer one, or on the second cache drive when it's bigger; until v1.126 small multiplayer maps had to fit 47 MiB, from when pack files took the campaign ones). The tool drops every 2048-byte
   sector made wholly of moved data, so what stays keeps its alignment, and zeroes the rest of the moved bytes.
4. **A whole number of sectors**: the small map's length (its header's file_length and its stream) padded with zeros to a
   multiple of 2048. A map's stream can end short of a sector (its header's file_length rounded up): 249 of 627 multiplayer
   maps in the old packs do. A small map whose length isn't a whole number of sectors doesn't load (the game copies a map to
   its cache in whole sectors): format 2's tools, and installer 28, made such maps.

**Rules for a pack's pass**: only data used by two or more of its maps goes into its shared file (data one map uses
stays in that map: shared files stay under 278 MiB, and a deleted map takes its own data with it); data in
stock.shared is taken from there, never put in the pack's file. **The maps are laid out in the order of their identities**
(size, then the first CRC, then the last), whatever their names or folders: the same maps make the same files, byte for byte
unpacked, whoever makes them (the installer, Halo Map Porter, shared_pass.py). The pack's file: its header, its list, then
each map's blocks in that order, each map's in its tags' order, each placed after the last at its remainder modulo 512, the
file padded to a whole sector. Its name: `<pack>-<8 lowercase hex of its checksum>`.

## stock.shared's layout

Fixed, so every console and every PC building it from the same maps gets the same file (checksum `a7fe2f70`, file_length
238745600 from the retail maps, the same on the three NTSC discs): first the 13 stock multiplayer maps (`beavercreek`,
`bloodgulch`, `boardingaction`, `carousel`, `chillout`, `damnation`, `hangemhigh`, `longest`, `prisoner`, `putput`,
`ratrace`, `sidewinder`, `wizard`) in the order of their file names, lower case; then the 10 campaign levels (`a10`, `a30`,
`a50`, `b30`, `b40`, `c10`, `c20`, `c40`, `d20`, `d40`), only their blocks that three levels or more use (the same bytes and
offset remainder modulo 512, counted once per level). In each, the tags in tag index order; a bitmap's bitmap_datas in block
order, a sound's pitch ranges' permutations in block order; each block's first copy placed after the last, padded with zeros
up to its remainder modulo 512; the file padded to a whole sector. So a block's place from the 13 maps is what it was in
v1.94-v1.96. `port/oxbox/tools/shared_pass.py` is the reference: a pack built against stock.shared with `--stock <the folder
of those 23 maps>` lays it out the same way.

**`maps\stock.index`** lists every block for matching a pack's against it without reading stock.shared: `OXSI`, a version (1),
the count, stock.shared's checksum and file_length (each 4 bytes), then per block its SHA-256 (32 bytes), its place in
stock.shared and its size (4 bytes each). A block matches when its bytes' hash is the same and its offset modulo 512 is
the place's.

## What the game does

- Loading a map with a reference: a shared file stored unpacked is opened where it is. A packed one is copied into a
  cache file first (a multiplayer one when it fits, 47 MiB, so both campaign ones stay for the level being left and the
  next; else a campaign one), once per launch (the next map drawing on it finds it there), then the map, and stays
  cached while it plays.
- A packed shared file that's missing, or cached in another version (checksum or length different), is copied again; one
  that can't be found fails the map's load with the log line `shared: <map> draws on <name>.shared, which is neither
  beside it nor in the folder above`.
- A read past a shared file's end, or from one the map doesn't name, fails (the texture or sound doesn't load) and says
  so in the log.
- A stock map or campaign level in the maps folder may draw on stock.shared too.
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

**The small copy** (version 8): a console in the lobby whose copy of the host's map is full size, with a pack's shared file
beside it or in the folder above, asks once whether the host's copy is the small one (the request's `small`). If it is the
same map (the same identity) drawing on a pack's file this console has in that exact version (and the same stock.shared), the
host sends the small map, which takes the full-size one's place once the lobby has copied that one to the cache. It never
holds the lobby, never fetches a pack's file for it, and never says the map is missing: a game starting, no room or anything
else, and the console keeps the copy it has (the game starting puts it off to the next lobby). A full-size copy and a small
one are the same map in a game either way.

A pack's file is named for its version (`<pack>-<8 hex of its checksum>.shared`), so two versions of a pack sit side by
side. **`stock.shared` is never sent**: every console builds its own from its own maps. One without it, or with another
version, stays in the lobby with `No stock.shared: run the installer`, as one with no room for a map does.

The room asked for, and what KEEP DOWNLOADS UNDER counts for the map that fetched it, include the pack's file. When a
map is deleted, or replaced by a copy that doesn't use it, its pack's file goes too if no other map in the list draws on
it. A replaced copy kept as `.hmp_prev` keeps its file while it stays, and that copy's deletion doesn't look for it.

**A dedicated server** (the Linux build, `port/linux/game/map_share_xbox.c`) hosts a converted map and shares it the same
way. It draws and plays nothing, so it loads the map without its shared files: it needs neither them nor `stock.shared` to
host it (their sounds, which its sound cache still asks for, are served as silence), and only needs the pack's file to send
it. Put `<pack>-<checksum>.shared` where a console would: beside the map or in the folder above. PCs playing the game are
unaffected: they get Custom Edition maps through `map_share.c`, and a PC game still refuses a converted map.

## The installer

After an install (never in the game's own updates) the installer offers to swap a Halo folder's 23 stock maps for small
ones drawing on `maps\stock.shared`, built from the player's own maps (`port/oxbox/installer/stockmaps.c`; about 800 MB
saved). The small maps are packed at zlib level 1: only 3% bigger than level 6, and far quicker on a console. It needs all 23: a folder without them isn't offered it, since stock.shared couldn't be the same file. Only the
retail maps are swapped, each checked by its file's size and CRC, so every console's stock.shared is the same file. All 24 maps are byte-identical on the three NTSC discs (the original, Rev 1 and Rev 2), and stock.shared and
the small maps built from each compare equal: the three builds need no table of their own. stock.shared is the same, byte for byte, as `shared_pass.py stock` makes, and the small maps unpack to the same bytes as its.

Most of a small map is its retail map's data as it was (a level's BSPs and models, most of its tags), so the one pass
reuses the retail map's own packed data instead of packing it again: where a run of the retail stream's deflate blocks
unpacks to bytes the small map has unchanged, and so do the 32 KB before them (all a block can refer back to), the small
map's stream takes those blocks bit for bit, then an empty stored block, so that what follows starts on a byte; the rest is
packed at level 1, raw deflate from a fresh start after each run, and the stream ends with the Adler-32 of all of it. The
table has each map's runs (where they are unpacked and in the retail file's bits; 494 runs in all, 82% of the small maps'
bytes) and the file as it must come out (its size and CRC: `gen_stock_table.py` makes the same file and checks it unpacks to
the small map), so the console packs a fifth of what it did (259 MB of 1430) and the small maps are 738 MB in all (755
packed through at level 1). One that doesn't come out as the table says is made from its retail map again, packed through.

It reads each retail map once: as a map's data goes into stock.shared, its small map is made in the maps folder beside it
(`<map>.map.oxsmall`) when the drive has the room for it (and 108 MB besides), else on a cache drive (X:, Y:, Z:), and once
all 23 are read and stock.shared is checked, each is renamed (or copied) into place, read back, and swapped in. When the
Halo folder's drive has the room for all of them, the cache drives aren't touched.
The small map's layout comes from the installer's table (`tools/gen_stock_table.py`: where each map's moved data is, so the
map's tags, at its end, needn't be read first). The cache drives are the installer's to use while it runs: if they haven't
room for the small maps between them, it empties them, the fullest first, and its own files there (and in the maps folder)
go when it's done (and after a power cut). Without a cache drive, a map is made from its retail map again after stock.shared is in place.

The swap and the restore do their reads and writes on a second thread (`port/oxbox/installer/mapstream.c`): the next 1 MB of
a file is read ahead and writes are made behind, so the disk works while zlib does. They're done in the order they were
made, and a flush, a close, a rename or a delete waits for what's queued first, so a power cut sees them as before.

**X** (*retail Halo back*) puts them back, the exact files: `maps\stock.restore` holds each map's restore table (its
header, each block's place, and for a moved one its place in stock.shared, its BSPs' offsets, and the original file's size
and CRC), and retail maps recompress to their own bytes with zlib level 6, memLevel 8. A level unpacks to as much as 280 MB
and a console has 64: nothing here holds a whole map, it goes through a temporary file a piece at a time. The stock maps come back before the
retail XBE does: if there's no room for them, nothing changes, and the folder still plays Halo 1 Unlimited. stock.shared
goes with them, so a map pack drawing on it needs another install's offer to play again.

It's built to take as little time and room as it can. A table in the installer says where every block goes, so `stock.shared`
is built in one pass through the maps, with no hashing and no temporary maps, and read back against its checksum. Each retail
map is checked against its size and CRC as it's read. One map is recompressed, to prove on that console that retail maps come
back exact. Then each map is made small on its own: built, its restore tried against the original's CRC, swapped in, and its
room given back. So about 350 MB free is enough, for `stock.shared` and the biggest small map beside its retail one.

A power cut can't lose a map. Before `stock.restore` goes in place the maps are untouched and the leftovers are deleted. After
it, the next look at the folder finishes any swap under way, and what's done stays small while the rest stay retail: both
play, the installer offers to finish, and **X** puts the small ones back. Putting them back needs about 1.2 GB.

## Consolidate packs

The installer's **White**, *See what consolidating would save*, measures a Halo folder's packs against `maps\stock.index`: it
only reads, and says each pack's size now and about what it would be (`port/oxbox/installer/consolidate.c`). A map's real
unpacked length can be a little under its header's file_length, which a pass may round up to a sector: read to the end of the
stream.

*Make its maps small (consolidate)*, on a pack's own Halo folder (one the installer imports from), does it: the pack's
multiplayer maps and modded campaign levels are imported (names, pictures and links, as an import's; a level's folder has
level.txt too, and the campaign picker plays it), then each is converted against the install's
`stock.index` and `stock.shared`, by the rules above and with shared_pass.py's layout (the same maps in the same order give
byte-identical files, unpacked). Each small map goes in its imported folder in the starred install's maps folder (`<install>\maps\<pack>\<map>`, never E:'s
storage) beside its `map.link` (the
game plays a folder's `.map` before its link; the link marks the folder as the import's), and the blocks two or more of the
maps use in `<pack>-<checksum>.shared` in the pack's folder there. The pack's own folder is left as it is: nothing is
deleted yet.

Each map is read three times: measured (its tag data, then its blocks' hashes), converted (its small map written, and its
part of the pack's file into a temporary unpacked copy, which is packed at the end and named), and read back before it's
used: every block it takes from stock.shared or the pack's file read there, and every block it keeps, every BSP and its tag
data (the fields aside) against the original's hashes, every field against what was laid out. A map that doesn't read back
right, or that would unpack to more than a campaign cache file holds (278 MiB), keeps its link. If anything else goes wrong, what
was made is deleted and nothing changes. HMP7's 13 maps (272 MB to 56 MB) took about 4 minutes in xemu, the import included.
It needs room for the pack's file twice and the small maps, by the measure's estimate. *Take out what's imported* deletes
the small maps and the pack's file too; importing again puts the links back alone.

Some packs' maps share one list of bitmaps or sounds between two tags, so a block's field can be there twice: both are
written and checked the same.

## Shrink maps

On the game's own install, **White** looks in its maps folder, the starred install's, and E:'s old storage (E:\TDATA\4c4a4855\maps, where installers
before 41 imported packs) for packs'
files with full-size maps they were made from where the game would find them (beside the map or in the folder above): maps
downloaded from a console that hadn't consolidated, or copied there. It says *You have N maps that can be shrunk to save
space* and offers *Shrink maps (saves about N MB)*. Each map is made small against stock.shared and that pack's file by its
list (consolidate_shrink): it must come out exactly as the list says (its small map's length and CRC32), which makes it the
same map, byte for byte unpacked, as the console that consolidated the pack has; it's read back as consolidating reads one,
then takes the full-size file's place (through `<name>.map.big`: a power cut leaves one of the two, which the next look puts
right). A map that doesn't come out as the list says stays as it is. Imported maps (links) aren't offered: their originals
stay where they are, so shrinking them saves nothing until deleting is in.

## Tools

`port/oxbox/installer/stockmaps_test.c`: the installer's own code on a PC, for testing a folder of stock maps.

`port/oxbox/installer/consolidate_test.c`: the installer's consolidating on a PC: `--convert STOCK_FOLDER PACK_FOLDER PREFIX MAP...` converts as the installer does; `--shrink STOCK_FOLDER PACK_FILE MAP...` shrinks full-size maps in place against a pack's file, as *Shrink maps* does.

`port/oxbox/tools/shared_pass.py`:

- `shared_pass.py stock OUT_DIR STOCK_MAP... [--levels LEVEL...]`: stock.shared and the stock maps drawing on it.
- `shared_pass.py [--stock STOCK_MAPS_DIR] NAME OUT_DIR MAP...`: a pack's NAME.shared and its maps.

`port/oxbox/tools/gen_stock_table.py RETAIL_MAPS_DIR OUT.h STOCK_RECIPES`: the installer's table, and beside it
`stock_recipes.h`, the 23 retail maps' recipes (from tagpool.c's `stock_recipes.bin`), each packed. `tools/tagrecipe.py`:
format 4's recipes in Python (read, written, the pool made as the installer makes it, a small stock map's recipe).

`port/oxbox/installer/tagpool.c RETAIL_MAPS_DIR OUT_DIR [--maps]` (a PC tool, `-lz`): format 4's stock tag pool,
stocktags.shared, the 23 stock recipes and, with `--maps`, the retail maps with recipes for testing.
`tagpool --pack STOCKTAGS PACK_SHARED NAME OUT_DIR MAP...`: format 4's pack pass over a pack's maps (made by
shared_pass.py or Halo Map Porter, drawing on PACK_SHARED): `NAME-<checksum>.shared` with its tag part, and each map with
its recipe, each checked to rebuild the same. Measured on CEX's 13 maps: a tag part of 2.4 MB, 9% of the tag data left in
the maps as their own words; the maps and the pack's file 70.5 MB -> 44.1 MB (zlib 9).

`port/oxbox/tools/verify_shared.py ORIGINAL_MAP CONVERTED_MAP PACK_SHARED STOCK_SHARED`: a converted map checked against its
original, every block and BSP read back through its new offsets. Run it on anything a pass makes: a wrong one can load and
play with no error. Measured on 14 of the old packs: 211 maps, all clean, 5080 MB down to 1427 MB.

Tested in xemu (v1.94): Blood Gulch from stock.shared; CEX's Abyss from CEX.shared and stock.shared together.

## Shared tag data (format 4)

Status: agreed with Halo Map Porter (2026-11-22) and **locked** (2026-11-23) by the console's timing test (v1.175, pass
10): `stocktags.shared`'s frames packed load as fast as raw or faster, so they're packed. The game reads it from v1.175
(stock groups) and v1.176 (pack groups); the installer makes it from installer 66 (v1.177). No released build has it yet.

Measured on the console (v1.175, the lab build that also checks each CRC32), the same maps from a packed and a raw
stocktags.shared:

| Map | Frames | Unpacked | Packed: read, time | Raw: read, time |
|---|---|---|---|---|
| Blood Gulch | 374 | 5.8 MB | 2.8 MB, 382-434 ms | 5.8 MB, 356-421 ms |
| c20 | 775 | 12.1 MB | 5.4 MB, 882 ms | 12.1 MB, 1105 ms |
| c40 | 1310 | 20.5 MB | 9.5 MB, 1438 ms | 20.5 MB, 1396 ms |
| d20 | 1158 | 18.1 MB | 8.5 MB, 1229 ms | 18.1 MB, 1331 ms |

The console's disk reads the raw frames about as slowly as its CPU inflates the packed ones, so the packed file (21.9 MB
against 52.9 MB) costs nothing at load. A load takes about 0.4 s longer for a multiplayer map, 0.9-1.4 s for a level.

A map's **tag data** can be rebuilt as it loads from bytes other maps already have, so it's stored once. Two places hold
them:

- **`stocktags.shared`**, per console: the **stock tag pool**, every word of the 23 retail maps' tag data stored once,
  built by the installer from the player's own disc (and by this repo's tool from the same maps; never distributed). It's
  a file of its own beside `stock.shared`, which stays exactly as it is.
- **the pack's own `.shared` file**, per pack: its **tag part**, the words two or more of the pack's maps use that aren't
  in the stock pool. No separate file.

A map keeps in place only the words that are its own; every other word of its tag data is written over, as it loads, by a
**recipe** (stored in the map) that copies ranges from the stock pool and the pack's tag part. Measured: the 23 retail
maps' tag data, 105.5 MB packed in their small maps today, takes 22.1 MB of pool (packed) + 2.8 MB of recipes; CEX's 13
maps 31.2 MiB -> 3.1 MiB in the maps + 2.2 MiB in the pack's file (HMP's matcher).

### The stock tag pool

Defined by the retail maps alone, so every console and tool makes the same words (only the pool's words have to match:
how a file stores them may differ):

1. The 23 maps in this order: beavercreek, bloodgulch, boardingaction, carousel, chillout, damnation, hangemhigh,
   longest, prisoner, putput, ratrace, sidewinder, wizard, a10, a30, a50, b30, b40, c10, c20, c40, d20, d40. Each map's
   tag data (header 0x010 offset, 0x014 size; all 23 are a whole number of words) as little-endian 4-byte words.
2. Walking each map's words in order against the pool so far (every earlier map's new words, and this map's own as
   they're added): at a word, its 8-word window is looked up; the candidate is the **earliest** pool position whose 8
   words are equal (compared word for word, not just hashed), extended forward while the words match (up to the pool's
   end). A match of **16 words (64 bytes) or more** is a copy; otherwise the words are **new**: appended to the pool
   one by one, until a word starts an 8-word window found in the pool. (A shorter match found there goes back through
   the same rule.)
3. The pool is those new words in the order they were appended: 13,213,509 words, **52,854,036 bytes, CRC32 `c52152d9`**
   (MD5 b9320efdb01018bd41161750e05e9e09; built the same by H1U's tool and HMP's own code from this rule). Each stock
   map's tag data is then wholly pool ranges: its recipe.

`stocktags.shared`:

| Offset | Size | Value |
|---|---|---|
| 0x000 | 4 | `OXST` |
| 0x004 | 4 | 1 (its version) |
| 0x008 | 4 | file_length |
| 0x00C | 4 | the pool's size in bytes (unpacked) |
| 0x010 | 4 | the pool's CRC32 (zlib's, of its unpacked bytes): its identity |
| 0x014 | 4 | frame size: 16384 |
| 0x018 | 4 | frame count (the last frame may be shorter) |
| 0x01C | 4 | the frame table's offset: 0x800 |
| 0x020-0x7FF | | zeros |
| 0x800 | 8 a frame | the frame table: each frame's offset in the file and its stored size |
| then | | the frames, in order |

A **frame** is 16 KB of the pool. It's stored either raw (stored size = the frame's own size) or as one zlib stream
(2-byte header, deflate, Adler-32) when that's smaller: a builder stores a frame raw whenever zlib's output is the frame's
size **or more**, so a stored size equal to the frame's size always means raw. Any zlib level may be used. An unpacked
`stocktags.shared` is the same format with every frame raw.

### The map

**The reference** (the header's 0x700) is version **3** when the map has a recipe; version 3 adds after 0x68:

| Offset | Size | Value |
|---|---|---|
| 0x68 | 4 | the recipe's offset in the map (unpacked; after the tag data, outside it and the BSPs, within file_length) |
| 0x6C | 4 | the recipe's size in bytes |
| 0x70 | 4 | the stock tag pool's CRC32 it was made against |
| 0x74 | 4 | and the pool's size in bytes |
| 0x78 | 4 | the CRC32 (zlib's) of the tag data as rebuilt (all tag_data_size bytes, before the game checks or fixes anything) |

The reference is then 0x7C bytes (0x700-0x77B); a tool taking a map's identity zeroes all of it, as now.

The tag data keeps its size and place. The words a recipe writes **must be zeros** in the map: one map then has one
encoding (whoever builds it, byte for byte), a non-zero word under a run means a mis-built map (any tool can check), and
a recipe map carries none of the words it copies (the pool, Microsoft's tag data, is never distributed, and the maps
don't carry its words either). The words no run writes are the map's own. A map without a recipe keeps reference version
2 and loads as today.

Tools check 0x78 whenever they read a map back. The game doesn't on a normal load (it costs a pass over up to 17 MB);
a lab build may.

**The recipe**, unsigned LEB128 varints throughout, lengths and offsets in words:

```
stock group count
  per stock group (frames ascending):
    frame: the frame's index for the first group, then the difference from the last group's (>= 1)
    run count (>= 1)
    per run (dest ascending within the group):
      dest delta: words from the end of the group's previous run (from word 0 for the first)
      offset in the frame
      length (>= 1; offset + length <= the frame's words)
pack group count
  per pack group: the same, its frames 16 KB slices of the pack's tag part
```

No two runs (in any group) write the same word. A run never crosses a frame, and runs are **maximal**: two runs that
continue each other in both the tag data and the source, within one frame, are one run (so a matched range is split
only where it crosses a frame). The recipe's size is exactly what it holds.

**A pack's tag part** is in its `.shared` file: its offset and size (bytes, a whole number of words; 0 and 0 for none)
at the file header's **0x068 and 0x06C**. It goes after the file's last block, starting at a 2048-byte boundary, the
file padded after it, so a pack's texture and sound offsets are the same whether or not its maps have recipes, and the
tag part never moves a block. It's stored as it is (the file as a whole may be packed, as now) and covered by the file's
checksum (0x064), so a map's reference to the pack names its tag part too.

**The pack pass**, defined as exactly as the pool so a pack's files are the same whoever makes them:

1. The pack's maps in the order of their identities (as the textures and sounds pass lays them out). Each map's tag data
   as words, matched against the stock pool and the pack's **candidates**: the words earlier maps of the pack kept as
   their own, in the order they came. A map's own words become candidates when that map is done (a map never matches
   its own words).
2. At a word: its 8-word window's earliest equal position in the pool, extended forward (to the pool's end at most);
   the same in the candidates (to their end at most); the longer wins, a tie goes to the pool. 16 words or more is a
   run (then the walk goes on after it); otherwise this one word is the map's own, and the walk goes on at the next word.
3. The tag part is every candidate word some later map's run uses, in candidate order (so each run's words stay
   contiguous in it); runs into the candidates are given as tag part offsets. A map's own words that went
   into the tag part are runs in its recipe too (from the tag part), zeros in the map; its other own words stay in
   place.

**Budget**: tag data + 64 KB <= the tag cache's 22 MiB as the map loads, and, as today, tag data + its largest BSP
<= 22 MiB while it plays. The rebuild runs before any BSP loads, with its 64 KB of working
memory (32 KB of recipe, a packed frame, an unpacked frame; the game's zlib state is its own) at the top of the tag
cache, so a map's BSPs may sit where they always have (all 23 retail maps' end flush at the top). Shared BSP data, later,
would need its working memory while a BSP is loaded: its maps would then keep their BSPs 64 KB below the top (Invader
`-T 23003136`). Format 4 doesn't (tight maps like B30 Evolved can't spare the 64 KB).

### Revision 2: the own words in the recipe (reference version 4)

Agreed with Halo Map Porter 2026-11-23, after the console's load timing (v1.178): a recipe map's tag area is almost all zeros, yet the game
reads all of it (0.27-0.65 s a load) only for the recipe to write over it, and every copy into the cache partition
writes it (3.5-20 MB a map: 30-55% of a pack map unpacked, Boxfight's and CEX's). Revision 2 moves a map's own words
into its recipe and leaves the tag area out of the file. Everything else stays: the stock tag pool, `stocktags.shared`,
its frames, the pack pass, a pack's tag part. Version 3 maps still load as above.

**The map**

- Its tag area is the last part of the map: nothing else in it (the header, the BSPs, the data blocks) ends after
  tag_data_offset. (So it is in all 23 retail maps and in every map Halo Map Porter and shared_pass.py make: checked on
  Boxfight's and CEX's.) A tool that brings blocks back into a map (a re-share, when only one map still uses data its
  pack's file held) places them before the tag area, at their remainders modulo 512 as before, never after it.
- Its tag_data_size is a whole number of words (all 23 retail maps' and Halo Map Porter's are), so the stock, pack and
  own runs cover all of it. A tool gives no recipe to a map whose tag data isn't.
- The file stops there: from tag_data_offset on it holds no tag data. The rest of tag_data_offset's own 2048-byte sector
  is zeros; the recipe starts at the next 2048-byte boundary, round_up(tag_data_offset, 2048), and the map ends after
  it, padded to a 2048-byte boundary: that's file_length. tag_data_offset and tag_data_size keep their values (the game
  sizes the tag memory by them; nothing else moves).
- The reference is version **4**, 0x80 bytes (0x700-0x77F, all of it zeroed when a tool takes a map's identity): as
  version 3 (0x68 the recipe's offset, which must be round_up(tag_data_offset, 2048); 0x6C its size; 0x70 and 0x74 the
  pool's CRC32 and size; 0x78 the CRC32 of the tag data as rebuilt), and **0x7C, flags: 0**. A reader refuses a map with
  a flag it doesn't know (none are defined yet).

**The recipe** is a list of sections, unsigned LEB128 as before:

```
section count
  per section (types ascending, each once):
    type
    size: the section's bytes, which follow
```

| Type | Section |
|---|---|
| 1 | stock groups: version 3's stock part (group count, then the groups) |
| 2 | pack groups: version 3's pack part, its frames 16 KB slices of the pack's tag part |
| 3 | own words |
| 4-63 | (none yet) a reader that doesn't know one refuses the map |
| 64 and up | (none yet) a reader that doesn't know one skips it: for additions an older game can do without |

Types 1, 2 and 3 are all present, in that order (one with nothing to say holds a count of 0). A known section's size
must be exactly what its contents take (a mismatch refuses the map, as a run outside the tag data does), and the
recipe's size is exactly its section count and its sections.

**The own words**, section 3:

```
run count
  per run (dest ascending):
    dest delta: words from the end of the previous own run (from word 0 for the first)
    length (>= 1)
    the words: length 4-byte little-endian words, as they are (not LEB128)
```

Every word of the tag data is written exactly once: by a stock run, a pack run or an own run. The own runs are the
maximal runs of the words no stock or pack run writes, so they follow from the other two and a map still has one
encoding whoever builds it. A zero the map holds is an own word like any other: nothing is left for the game to clear.
What moves: Halo's maps have 1,400-8,700 own words (6-35 KB; a small stock map's own words are its fields, as now);
a pack map, the words it keeps as its own (CEX: about 9% of its tag data).

**What the game does with one**: it doesn't read the tag area; it reads the recipe a piece at a time, as now, and copies
each section's runs into the tag memory, the own words straight from the recipe. A copy into the cache partition copies
file_length, which no longer has the tag area in it.

**The pack pass** is unchanged; a map's own words that didn't go into the pack's tag part go in its own-words section
instead of staying in place. (Halo Map Porter measured the 34 maps' own words: 32.4 MiB, 15.7 MiB of them zero words,
but never 16 or more zeros in a row (the pool's runs cover those), so the words stay as they are, with no form for runs
of zeros.)

**Who makes which**: once the game reads version 4, Halo Map Porter writes version 4 only; it still reads version 3, and
a re-share rewrites a version 3 map as the version 4 map a fresh pass would make. H1U's installer makes its stock maps
in version 4 (their own words are their fields).

**Measured gains it's for** (the console, v1.178): no tag read (0.27-0.65 s a load); each copy into the cache shorter by
the tag area (Blood Gulch CEX 12.9 -> 5.9 MB, Boxfight 2a 41.9 -> 28.1 MB, Boxfight Start 26.0 -> 13.7 MB; Halo's levels
about 10% less), about 0.3-1.4 s at the console's ~14 MB/s; and a cache slot holds bigger maps.

### What the game does

In `scenario_tags_load` (cache_files.c), once the tag data is read and before it's checked and the free tag memory is
lent to the texture cache: a version 3 reference with a recipe opens `maps\stocktags.shared` (found as stock.shared is),
compares the pool's CRC32 and size with the reference's, then reads the recipe from the map a piece at a time; for each
stock group it reads the frame's table entry (the table read forward in pieces), the frame, inflates it (zlib checks its
Adler-32) or takes it raw, and copies its runs into the tag data; the pack groups the same from the pack's tag part.
Anything wrong (no stocktags.shared, another pool, a run outside the tag data or a frame, a damaged frame) refuses the map
with a log line, as a map that can't load. A log line gives the frames, megabytes inflated, runs and milliseconds.
The recipe may start anywhere after the tag data: the game reads it from the start of its sector. The pack's tag part is
read as the pack's textures are, from the pack's file where it is or from the cache file it's copied into.
Built in v1.175 (`cache_file_tags_rebuild`, cache_files_windows.c) for stock groups, v1.176 for pack groups, v1.179 for
revision 2 (version 4: the tag area not read, nor first filled with its 0xCD pattern; the sections read in order, the own
words copied from the recipe as they come). Tested in
xemu: Blood Gulch, c20 and d20 from recipes, as stock maps and as folder maps, from a packed and a raw stocktags.shared;
CEX's maps from stocktags.shared and their pack's tag part (one with its recipe off a sector boundary); each CRC32 as its
reference says (`-DOXBOX_TAG_RECIPE_CHECK`).

### The installer

Builds `stocktags.shared` from the player's retail maps as it builds stock.shared, and the 23 small maps with recipes.
A console can't run the matcher (the pool and its index don't fit 64 MB), so the 23 stock recipes come baked in the
installer (`stock_recipes.h`, 2.8 MB packed): their runs that copy from past the pool's end, as it then is, are the
pool's new words (any run over a word has it), so the recipes alone also say how the pool is made. It checks the pool's
CRC32 as it's made, and the file as it reads back. Its frames: zlib level 1 (23.3 MB; level 9 would save 1.4 MB at
several times the time on the console's CPU).

A small stock map's tag data differs from its retail map's only in its fields (each block's place in stock.shared or
where it moved down to, each BSP's): every field is a whole word in the tag data. So its recipe is **its retail map's
recipe less the words its fields are in** (a run cut there, a group left with none dropped; the other runs as they are,
so a piece may be shorter than 16 words), and those words, holding the fields, are its own words. From installer 70
(v1.179) it's a revision 2 map (reference version 4): the file stops at its tag area, the recipe's sections are the stock
groups, no pack groups, and the fields as its own words. (Installer 66-69 made version 3: the tag area stored, zeros but
for the fields.) Only the installer (and `gen_stock_table.py`, which bakes what each must come out as) makes
these, so this is the installer's rule, not the pack pass's. Putting a map back makes its tag data from the pool with its
retail map's recipe (every word), then undoes the rest as before; `stocktags.shared` goes with stock.shared.

Measured: the small maps 737.9 MB -> 640.8 MB on the disk (multiplayer 40.7 -> 7.3 MB), plus stocktags.shared's
23.3 MB: an install takes 74 MB less. Revision 2 (installer 70): about the same on the disk (639.6 MB), but what the
game copies into the cache 1434 -> 1192 MB unpacked (multiplayer 138 -> 45 MB). Built in installer 66 (stockmaps.c); on a PC (stockmaps_test.c) the own-folder
build, the swap, its restore and Rebuild original Halo all give back the retail maps byte for byte.

## Not yet

- Shared BSP data (above).
