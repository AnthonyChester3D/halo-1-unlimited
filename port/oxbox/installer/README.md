# Halo 1 Unlimited installer

An XBE of this project's own, built with [nxdk](https://github.com/XboxDev/nxdk), that makes OpenCE on the Xbox
from the player's own clean retail Halo XBE. It's the first step of the online branch's updater: a release is the
installer and a package, and neither holds any of Microsoft's code or any of Halo's art.

- **The package** (`opence.oxpk`, made by `port/oxbox/tools/mkpackage.py`) says how to make a build of OpenCE from
  a clean retail Halo XBE: which runs of the retail XBE's bytes go where, OpenCE's own bytes (its code), and where
  the pictures go. It covers each retail XBE it knows (NTSC versions 8 and 9) and is about 1.4 MB.
- **The pictures** (the main menu's OPTIONS and PROFILES, the postgame's gametype pictures, ONLINE PLAY's header)
  are made on the Xbox from the letters and pictures in the game's own `maps\ui.map` (`pictures.c`).

## For players

1. Copy the installer's folder (`default.xbe`, `opence.oxpk`) anywhere on the Xbox's hard disk, for example
   `E:\Apps\Halo 1 Unlimited installer`, and start it from your dashboard.
2. It lists each Halo it finds in E:, F: and G: and in their `Games` folders (and the folder it's in, if that's
   inside Halo's), with what's there:
   - *clean retail Halo (NTSC, version 8)*: ready to install.
   - *OpenCE (retail kept: ...)*: installed; A updates it to this package's.
   - *another XBE (retail kept: ...)*: a different XBE (a modified Halo, say) with the clean retail one beside it.
   - *not a clean retail Halo XBE this package knows*: nothing to install from. Put the retail `default.xbe` from
     your own disc in the folder, either as `default.xbe` or, to keep the XBE that's there, as
     `default_retail.xbe`.
3. **A** installs (or updates), **X** puts the retail Halo back, **START** plays what's in the folder, **B** goes
   back to the dashboard.

What it changes in the Halo folder:

| File | |
|---|---|
| `default.xbe` | OpenCE, made from the retail XBE |
| `default_retail.xbe` | the clean retail XBE, kept (made from `default.xbe` on the first install) |
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

A package is made from builds of the same commit, one from each retail XBE:

```sh
sh port/oxbox/build.sh retail_v8.xbe && cp build-xbox/halo1unlimited.xbe build_v8.xbe
sh port/oxbox/build.sh retail_v9.xbe && cp build-xbox/halo1unlimited.xbe build_v9.xbe
python3 port/oxbox/tools/mkpackage.py opence.oxpk "crossplay experiment 6" \
    retail_v8.xbe build_v8.xbe retail_v9.xbe build_v9.xbe
```

mkpackage checks that the package makes each build back, and before any of the build's bytes go into the package it
takes from the retail XBE every run of 16 bytes or more found there. `oxpatch` (`oxpatch.c`) does what the installer
does, on a PC, for testing a package; `mkpictures` (`mkpictures.c`) makes the pictures for a build
(`UI_MAP=path/to/ui.map sh port/oxbox/build.sh ...`). Both use the same code as the installer, so a build made on the
PC and an install made on the Xbox from the same retail XBE and package are the same, byte for byte.

| File | |
|---|---|
| `nxdk.mk` | the installer's build (nxdk's makefile) |
| `main.c` | the installer: the menu, finding Halo, installing, putting the retail Halo back |
| `package.c`, `package.h` | reading a package and making a build from it |
| `pictures.c`, `pictures.h` | the pictures, from ui.map |
| `sha256.c`, `sha256.h` | SHA-256 |
| `mkpictures.c`, `oxpatch.c` | the PC tools |

### Coming next

Signed packages (the signing key held by the project's owner alone), downloads from GitHub Releases, and the game
saying when an update is out and starting the installer to get it.

### Licences

This folder is part of the online branch (AGPL-3.0-or-later). The installer XBE also contains nxdk's libraries (MIT;
its USB stack, Apache-2.0; PDCLib, CC0), SDL2 (zlib licence) and zlib (zlib licence): a release carries their
notices beside it (`THIRD_PARTY.txt`).
