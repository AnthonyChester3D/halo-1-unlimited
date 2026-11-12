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

A map draws on up to two:

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
wait and no cache file. stock.shared is always this way, and so is a pack's file bigger than a multiplayer cache file
(47 MiB unpacked: the installer's consolidate.c and shared_pass.py leave it unpacked). A smaller packed one is copied into
a multiplayer cache file; a bigger packed one would need one of the campaign's two, which are for the level being left
and the next, so a level change would find no room (Covenant Edition's 143 MB file did, on a console).

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
   maps have a block that stays, or a BSP, sharing a sector with one that moves. A map drawing on a packed shared file must fit a multiplayer cache file unpacked (47 MiB). The tool drops every 2048-byte
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
right, or that would unpack to more than a multiplayer map may (47 MiB), keeps its link. If anything else goes wrong, what
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

`port/oxbox/tools/gen_stock_table.py RETAIL_MAPS_DIR OUT.h`: the installer's table.

`port/oxbox/tools/verify_shared.py ORIGINAL_MAP CONVERTED_MAP PACK_SHARED STOCK_SHARED`: a converted map checked against its
original, every block and BSP read back through its new offsets. Run it on anything a pass makes: a wrong one can load and
play with no error. Measured on 14 of the old packs: 211 maps, all clean, 5080 MB down to 1427 MB.

Tested in xemu (v1.94): Blood Gulch from stock.shared; CEX's Abyss from CEX.shared and stock.shared together.

## Not yet

- Shared tag data (loaded into memory at a fixed address): much harder, and only safe when every map loading it uses
  all of it.
