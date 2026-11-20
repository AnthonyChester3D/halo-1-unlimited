# The map library

Maps made for Halo 1 Unlimited, downloaded from the installer's **Map Library** screen straight into the install's
map list. The files live in a separate public repository's releases (github.com/h1u-librarian/map-library), never in
this one: the maps hold Halo's content.

## What a player sees

Main menu > Map Library: the library's maps, each with its group (Halo Map Porter's "Crossplay" or "Xbox"), size and
state: nothing (not on this Xbox), Installed, Update (a newer zip of the same map), Other version (the other group's
copy of it is installed), Replaces yours (that folder holds a map that didn't come from the library). A downloads it
into `<install>\maps\<folder>\`, as Halo Map Porter's "Copy to Xbox maps" folders are put by hand; an installed one
can be removed. The game lists it with no other step.

The item is on the main menu once the installer has the library's key (`port/oxbox/installer/library_keys.txt`);
before that only with developer.ini's `library_url` (a test library).

## The files

**catalog.oxlb**: `"OXLB"`, u32 format (1), u32 the text's size, the text, then the signatures as a package's
(`"OXSG"`, u32 count, each an 8-byte key id and an Ed25519 signature of `"Halo 1 Unlimited map library, signed\n"` +
the 12 bytes before the text + the text). The text is ASCII `key: value` lines: the library's (`library`, `format`,
`serial`: one more each build, `date`), then each item's from its `item:` line:

    item: Coldsnap            its folder: maps\Coldsnap\ (and its name in map sharing, "+Coldsnap"); at most 30 characters
    name: Coldsnap            info.txt's first line
    group: Crossplay          Halo Map Porter's output folder it came from
    about: By Tiamat, 2006.  Players: 4-16  ...   info.txt's lines up to its [sections], without Version:
    version: 5                info.txt's Version:
    file: Coldsnap-166c49b9.zip   beside the catalog (or a full address)
    size: 16169405            the zip's bytes
    unpacked: 17098444        its files' bytes
    files: 6
    sha256: 166c49b9...       the zip's

The same folder in two groups is two versions of one map: an Xbox has one or the other.

**Each map's zip** holds its folder's files (a folder in it at most), `.map` files stored (Xbox maps are zlib inside
already), the rest deflated, every time byte for byte the same for the same files: an unchanged map keeps its file
name and isn't uploaded again.

## On the Xbox (port/oxbox/installer/library.c)

The catalog is downloaded and its signature checked against `library_keys.txt` (a test library's catalog may be
unsigned with developer.ini's `allow_unsigned = 1`). A map: its zip downloaded beside its folder
(`<folder>.hmp_dl.zip`), its size and SHA-256 checked against the catalog, unpacked into `<folder>.hmp_dl\` (names
checked: nothing outside the folder; each file's CRC-32 checked; no more than the catalog's `unpacked`), the mark
`library.oxl` written in it (the item's file, group and SHA-256: how the screen knows what's installed), then swapped
in: the folder there now renamed `<folder>.hmp_old`, the new one renamed `<folder>`, the old one deleted. The game
skips folders with `.hmp_` in their names, so a power cut never shows half a map; the next visit puts an `.hmp_old`
back if the swap was cut between its renames, and deletes the rest. Needs the zip's and the files' room on the
install's drive.

## Making and publishing a library (on the owner's PC)

    python3 port/oxbox/tools/library.py build OUT "...\output\Copy to Xbox maps\Crossplay" "...\Copy to Xbox maps\Xbox" --previous CURRENT\catalog.oxlb
    python3 port/oxbox/tools/oxsign.py sign library.oxkey OUT\catalog.oxlb
    python3 port/oxbox/tools/oxsign.py verify OUT\catalog.oxlb --keys port/oxbox/installer/library_keys.txt

`build` prints the zips that are new; upload those and the signed `catalog.oxlb` to the library repository's release
(the installer reads `releases/latest/download/catalog.oxlb`). `--previous` keeps the items of the catalog now
published that this build doesn't make again, and numbers the new one after it.

The key: `python3 oxsign.py keygen library` once (library.oxkey stays on the owner's PC, with a backup offline); its
public line (library.pub) goes in `library_keys.txt`. It's not the package key: a leaked library key can't sign an
update.

A release holds up to 1000 files; past that, a second release, its zips named with their address in `file:`.

## Testing (the lab)

A folder with the zips and a catalog served by `python3 -m http.server 8765`, and the installer's developer.ini:

    developer = 1
    library_url = http://10.0.2.2:8765/catalog.oxlb
    allow_unsigned = 1

(xemu's NAT reaches the PC at 10.0.2.2; without `allow_unsigned`, build the installer with
`LIBRARY_KEYS=<a test key's list>` and sign the catalog with that key.)
