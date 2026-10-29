# Halo 1 Unlimited installer

An XBE of this project's own, built with [nxdk](https://github.com/XboxDev/nxdk), that makes OpenCE on the Xbox
from the player's own retail Halo XBE. It's the first step of the online branch's updater: a release is the
installer and a package, and neither holds any of Microsoft's code or any of Halo's art.

- **The package** (`opence.oxpk`, made by `port/oxbox/tools/mkpackage.py`) says how to make a build of OpenCE from
  a retail Halo XBE: which runs of the retail XBE's bytes go where, OpenCE's own bytes (its code), and where the
  pictures go. It covers each of the NTSC discs' XBEs (versions 1, 8 and 9, the same code with different
  signatures), and a copy of one patched to run from the hard disk or in any region. It's about 1.6 MB (2.2 MB
  carrying the installer).
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
   The folder of your last install comes first, marked with a yellow `*`, and starts selected (the installer
   keeps it in `installer_last.txt`, in its own folder, after each install that worked; it's listed even if it's
   further in than the installer looks).
3. **A** installs (or updates), **X** puts the retail Halo back, **START** plays what's in the folder, **Y** looks
   online for a newer package, **B** goes back to the dashboard. The d-pad or the left stick moves; the triggers
   move a page.

### Updates

**Y** downloads the project's latest release (its `opence.oxpk`, from GitHub Releases), over the network the
dashboard's settings give (or an automatic address when there are none). It shows what it found (release, date,
notes) and asks before it uses it: **A** puts it in place of `opence.oxpk` (the old one is kept as
`opence_old.oxpk`), and the list then shows the folders it can update. With no `opence.oxpk` at all, Y gets the
first one. It only ever uses a package signed by a key it trusts (`keys.txt`), and never one older (a lower release
number) than a signed one it has; anything else is deleted again and nothing changes. `installer_url.txt` in its
folder, its first line an `https://` or `http://` address, fetches from there instead (a mirror, or a test).

The installer can update itself. A package signed by a key it trusts may carry an installer (`mkpackage.py
--installer`); when that installer's version is higher than the running one's (the top line says *version N*), it
asks, at the start or after a download: **A** writes the new installer beside itself, reads it back and checks its
SHA-256, keeps itself as `installer_old.xbe`, puts the new one in its place (under its own name, `default.xbe` or
whatever it's called) and starts it. If anything goes wrong before the swap, nothing changes. The new installer also
writes its `THIRD_PARTY.txt` beside itself.

The server's HTTPS certificate isn't checked: the installer has no certificate authorities to check it against, and
no clock it could trust to check dates. Nothing secret is sent, and what's downloaded is trusted only for its
signature, checked with the public keys built into the installer, so a server or network in between can at most
stop an update or offer an older signed package, which it refuses.

The top line says whether the package is signed: *signed (main)* in green is a release from the project; *not
signed* (a test package) or *signed by a key it doesn't know* in yellow can still be installed from a copy you put
there yourself. A package changed after it was signed isn't installed at all.

What it changes in the Halo folder:

| File | |
|---|---|
| `default.xbe` | OpenCE, made from the retail XBE |
| `default_retail.xbe` | the retail XBE, kept as it was (from `default.xbe` on the first install) |
| `default_old.xbe` | an XBE that was there and that the installer didn't make (a modified Halo), kept |

And in its own folder: `installer_log.txt` (what it did), `installer_last.txt` (the last install's folder),
`opence_old.oxpk` (the package a download replaced), `installer_old.xbe` (the installer an update replaced),
`THIRD_PARTY.txt` (rewritten when it differs from the installer's own), and `installer_url.txt` if you add one.

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
    halo_v1.xbe build_v1.xbe halo_v8.xbe build_v8.xbe halo_v9.xbe build_v9.xbe \
    --release 1 --installer port/oxbox/installer/bin/default.xbe
```

`--showcase DIR` carries the stock maps' showcase files (`DIR/bloodgulch.txt`...). Installer version 3 and later
writes them as `maps\showcase\<name>.txt` only when it first makes OpenCE in a Halo folder (one that wasn't OpenCE
already), and never over a file that's there: updates leave the folder alone, so a player can change or delete them.

`--installer` carries the installer in the package (about 600 KB more), so older installers can update themselves
from it. Its version comes from the XBE (`INSTALLER_VERSION` in `main.c`, raised with each installer that should
replace the last; `make ... INSTALLER_VERSION=N` builds another for testing). Installers before version 1 (installer
tests 4 to 9) read these packages as before and ignore the installer in them.

mkpackage checks that the package makes each build back, and before any of the build's bytes go into the package it
takes from the retail XBE every run of 16 bytes or more found there. `oxpatch` (`oxpatch.c`) does what the installer
does, on a PC, for testing a package; `mkpictures` (`mkpictures.c`) makes the pictures for a build
(`UI_MAP=path/to/ui.map sh port/oxbox/build.sh ...`). Both use the same code as the installer, so a build made on the
PC and an install made on the Xbox from the same retail XBE and package are the same, byte for byte.

| File | |
|---|---|
| `nxdk.mk` | the installer's build (nxdk's makefile) |
| `main.c` | the installer: the list, finding Halo, installing, putting the retail Halo back, updates |
| `download.c`, `download.h` | the network (nxdk's lwIP and network driver) and HTTPS downloads, following redirects |
| `tls_config.h` | Mbed TLS (`port/third_party/mbedtls`) as the installer builds it: a TLS 1.2 and 1.3 client |
| `screen.c`, `screen.h` | the text screen, in colour, and the screen blanked while the next XBE starts |
| `package.c`, `package.h` | reading a package, checking its signature, making a build from it |
| `pictures.c`, `pictures.h` | the pictures, from ui.map |
| `sha256.c`, `sha256.h` | SHA-256 |
| `keys.txt` | the public keys it trusts (`keys.h` is made from it when it's built) |
| `THIRD_PARTY.txt` | the notices of the code in its XBE, which it writes beside itself (`notices.h`, by `tools/mkembed.py`) |
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

### Publishing a release (the project's owner)

The installer's **Y** fetches `https://github.com/AnthonyChester3D/halo-1-unlimited/releases/latest/download/opence.oxpk`
(`DOWNLOAD_DEFAULT_URL` in `download.h`). GitHub sends that to the newest release's file named `opence.oxpk`, so:

1. Make the package with a release number higher than the last one's (`--release N`), with `--installer`, and sign
   it (above).
2. On GitHub: Releases, *Draft a new release*. A new tag (`release-N`, say) on the online branch's commit the package
   was built from, a title and notes; attach the signed `opence.oxpk` under exactly that name. Don't tick
   *pre-release* (GitHub's "latest" skips those), and publish it.
3. Attach `default.xbe` and `THIRD_PARTY.txt` too, for people installing for the first time (or put the installer's
   folder in a zip).

Only signed packages are ever used, so a release with an unsigned one is simply refused by every installer.

### Coming next

The game saying when an update is out and starting the installer to get it.

### Licences

This folder is part of the online branch (AGPL-3.0-or-later). The installer XBE also contains nxdk's libraries (MIT;
its USB stack, Apache-2.0; PDCLib, CC0; its lwIP, BSD 3-clause; its network driver, MIT), SDL2 (zlib licence), zlib
(zlib licence), the unscii font (public domain), Monocypher (CC0) and Mbed TLS (Apache-2.0): a release carries their
notices beside it (`THIRD_PARTY.txt`), and the installer writes them beside itself.
