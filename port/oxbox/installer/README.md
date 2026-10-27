# Halo 1 Unlimited installer

An XBE of this project's own, built with [nxdk](https://github.com/XboxDev/nxdk), that makes OpenCE on the Xbox
from the player's own retail Halo XBE. It's the first step of the online branch's updater: a release is the
installer and a package, and neither holds any of Microsoft's code or any of Halo's art.

- **The package** (`opence.oxpk`, made by `port/oxbox/tools/mkpackage.py`) says how to make a build of OpenCE from
  a retail Halo XBE: which runs of the retail XBE's bytes go where, OpenCE's own bytes (its code), and where the
  pictures go. It covers each of the NTSC discs' XBEs (versions 1, 8 and 9, the same code with different
  signatures), and a copy of one patched to run from the hard disk or in any region. It's about 1.6 MB.
- **The pictures** (the main menu's OPTIONS and PROFILES, the postgame's gametype pictures, ONLINE PLAY's header)
  are made on the Xbox from the letters and pictures in the game's own `maps\ui.map` (`pictures.c`).

## For players

1. Copy the installer's folder (`default.xbe`, `opence.oxpk`) anywhere on the Xbox's hard disk, for example
   `E:\Apps\Halo 1 Unlimited installer`, and start it from your dashboard.
2. It lists every Halo it finds on E:, F: and G: (in their folders, their `Games` folders, and one folder further
   in, such as `E:\Games\Halo mods\...`). The ones it can install to come first, in green:
   - *retail Halo: ready to install*: the `default.xbe` from a disc (or a copy patched for region or media).
   - *OpenCE (this package's)* or *OpenCE (another build)*: installed, the retail XBE kept beside it; A installs
     this package's.
   - *another XBE (retail kept)*: a different XBE (a modified Halo, say) with the retail one beside it.

   The others are grey, and can't be started from here:
   - *modified or unknown XBE*: nothing to install from. To install there, copy the `default.xbe` from your disc
     into the folder as `default_retail.xbe` (the XBE that's there is kept, as `default_old.xbe`).
   - *default_retail.xbe in the way*: that file isn't a retail Halo XBE; move it out first.
3. **A** installs (or updates), **X** puts the retail Halo back, **START** plays what's in the folder, **B** goes
   back to the dashboard. The d-pad or the left stick moves; the triggers move a page.

The top line says whether the package is signed: *signed (main)* in green is a release from the project; *not
signed* (a test package) or *signed by a key it doesn't know* in yellow can still be installed from a copy you put
there yourself. A package changed after it was signed isn't installed at all.

What it changes in the Halo folder:

| File | |
|---|---|
| `default.xbe` | OpenCE, made from the retail XBE |
| `default_retail.xbe` | the retail XBE, kept as it was (from `default.xbe` on the first install) |
| `default_old.xbe` | an XBE that was there and that the installer didn't make (a modified Halo), kept |

It checks the retail XBE's SHA-256 before it starts, writes OpenCE beside the old XBE (`default.xbe.new`), reads it
back and checks its SHA-256 (the pictures aside) before it swaps it in, and has the drive write everything out
before it says it's done. It never deletes an XBE it can't make again: only OpenCE and copies of the retail XBE it
has checked. What it did goes into `installer_log.txt`, in its own folder.

## For developers

Building the installer needs nxdk (with its submodules) and its tools on the `PATH`:

```sh
make -C port/oxbox/installer -f nxdk.mk NXDK_DIR=/path/to/nxdk     # bin/default.xbe
```

A package is made from builds of the same commit, one from each disc's XBE (NTSC versions 1, 8 and 9: the
README in `port/oxbox` has their SHA-256s):

```sh
for v in 1 8 9; do
  sh port/oxbox/build.sh halo_v$v.xbe && cp build-xbox/halo1unlimited.xbe build_v$v.xbe
done
python3 port/oxbox/tools/mkpackage.py opence.oxpk "crossplay experiment 6" \
    halo_v1.xbe build_v1.xbe halo_v8.xbe build_v8.xbe halo_v9.xbe build_v9.xbe
```

mkpackage checks that the package makes each build back, and before any of the build's bytes go into the package it
takes from the retail XBE every run of 16 bytes or more found there. `oxpatch` (`oxpatch.c`) does what the installer
does, on a PC, for testing a package; `mkpictures` (`mkpictures.c`) makes the pictures for a build
(`UI_MAP=path/to/ui.map sh port/oxbox/build.sh ...`). Both use the same code as the installer, so a build made on the
PC and an install made on the Xbox from the same retail XBE and package are the same, byte for byte.

| File | |
|---|---|
| `nxdk.mk` | the installer's build (nxdk's makefile) |
| `main.c` | the installer: the list, finding Halo, installing, putting the retail Halo back |
| `screen.c`, `screen.h` | the text screen, in colour, and the screen blanked while the next XBE starts |
| `package.c`, `package.h` | reading a package, checking its signature, making a build from it |
| `pictures.c`, `pictures.h` | the pictures, from ui.map |
| `sha256.c`, `sha256.h` | SHA-256 |
| `keys.txt` | the public keys it trusts (`keys.h` is made from it when it's built) |
| `text.h` | snprintf that always ends its text (nxdk's PDCLib doesn't when it cuts it short) |
| `mkpictures.c`, `oxpatch.c` | the PC tools |

### Signing releases (the project's owner)

A release is a package signed with the owner's key; the installer (and, later, the game's update check) trusts the
public keys in `keys.txt`. `port/oxbox/tools/oxsign.py` does it all, with plain Python 3 (nothing to install).

1. Once: make two keys, in a folder outside the repository:

   ```sh
   python3 oxsign.py keygen main      # main.oxkey (secret) and main.pub
   python3 oxsign.py keygen backup    # backup.oxkey and backup.pub
   ```

   Each asks for a passphrase, which locks its secret file: a copied `.oxkey` file is no use without it. Keep
   `main.oxkey` on your PC, put `backup.oxkey` somewhere offline (a USB stick in a drawer), and keep the passphrases
   in a password manager. If main is ever lost or leaked, a release signed with backup can trust a new main.
2. Once: put the two lines from `main.pub` and `backup.pub` in `keys.txt` (they're public) and commit it.
   Installers built from then on trust those keys.
3. Each release: `python3 oxsign.py sign path/to/main.oxkey opence.oxpk`, then
   `python3 oxsign.py verify opence.oxpk --keys port/oxbox/installer/keys.txt` to check it.

Or in one go, on Windows. Once: `py port\oxbox\tools\oxsign.py setup path\to\main.oxkey`. From then on,
double-clicking a `.oxpk` opens a window that shows the package (name, release, notes) and its signatures, checked
against `keys.txt`, and asks "Sign it with main? [y/N]": Y signs it and checks it again. Dropping packages on
`tools\sign.cmd` does the same. It asks main's passphrase the first time, and offers to remember it on this PC:
Windows keeps it encrypted so only your Windows login can read it back (`oxsign.py forget` forgets it;
`oxsign.py unsetup` undoes all of it). It always asks before signing, so opening a package someone sent you
can't sign it by accident.

`.oxkey` files are in `.gitignore`, so one can't be committed by mistake. A signature covers the package's header
(which holds its contents' SHA-256) and its manifest (`oxsign.py show` prints it: name, release, network version,
notes); `mkpackage.py --release N --network N --notes "..."` writes it.

### Coming next

Downloads from GitHub Releases (signed releases only), and the game saying when an update is out and starting the
installer to get it.

### Licences

This folder is part of the online branch (AGPL-3.0-or-later). The installer XBE also contains nxdk's libraries (MIT;
its USB stack, Apache-2.0; PDCLib, CC0), SDL2 (zlib licence), zlib (zlib licence), the unscii font (public domain)
and Monocypher (CC0): a release carries their notices beside it (`THIRD_PARTY.txt`).
