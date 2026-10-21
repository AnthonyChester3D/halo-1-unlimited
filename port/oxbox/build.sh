#!/bin/sh
# port/oxbox/build.sh [HALO_XBE]: builds build-xbox/halo1unlimited.xbe: the game code and the Xbox layer (this folder),
# compiled for the Xbox's CPU, linked against the libraries of the Halo XBE given, and added to it (port/oxbox/README.md).
# HALO_XBE defaults to default_16Xbox.xbe in the repository's root. Needs clang, ld.lld, llvm-objcopy, ninja,
# python3 and a 32-bit libgcc (LIBGCC=... to name one).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
XBE_IN=$(cd "$(dirname "${1:-$ROOT/default_16Xbox.xbe}")" && pwd)/$(basename "${1:-default_16Xbox.xbe}")
cd "$ROOT"
[ -f "$XBE_IN" ] || { echo "no Halo XBE at $XBE_IN (see port/oxbox/README.md)"; exit 1; }
LIBGCC=${LIBGCC:-$(ls /usr/lib/gcc/x86_64-linux-gnu/*/32/libgcc.a 2>/dev/null | tail -1)}
[ -f "$LIBGCC" ] || { echo "no 32-bit libgcc.a found: install gcc-multilib, or set LIBGCC"; exit 1; }
mkdir -p build-xbox
python3 -I port/oxbox/tools/gen_xbox.py > build-xbox/build.ninja
ninja -f build-xbox/build.ninja -j "${JOBS:-$(nproc)}"
link() {
  ld.lld -L build-xbox -m elf_i386 -T port/oxbox/link.ld -L port/oxbox -e oxbox_entry --gc-sections --warn-unresolved-symbols \
    -Map build-xbox/oxbox.map -o build-xbox/oxbox.elf $(cat build-xbox/objects.txt) "$LIBGCC" 2> build-xbox/link.err
}
# (twice: the first link finds where OpenCE's .bss can go over the retail game's unused code, the second puts it there)
echo '/* empty: first link */' > build-xbox/ovl.ld
link
python3 -I port/oxbox/tools/gen_overlay.py build-xbox/oxbox.map build-xbox/ovl.ld
link
grep -v ">>>" build-xbox/link.err | sort -u | head -20
if grep -q "undefined symbol" build-xbox/link.err; then echo "undefined symbols: no XBE made"; exit 1; fi
python3 -I port/oxbox/tools/mkxbe.py "$XBE_IN" build-xbox/oxbox.elf build-xbox/halo1unlimited.xbe
