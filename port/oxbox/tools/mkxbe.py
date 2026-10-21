"""mkxbe.py retail.xbe oxbox.elf out.xbe: the retail Halo XBE with OpenCE's game code (oxbox.elf, linked at
0x3B9000 against the retail libraries) appended to its last section (.cap16), and the retail startup's call to
main redirected to oxbox_entry. Every retail section stays where it was."""
import hashlib, re, struct, subprocess, sys
import os
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE); import xbe

src, elf, dst = sys.argv[1:4]
sym = {}
for line in subprocess.run(['llvm-nm', elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3: sym[p[2]] = int(p[0], 16)
start, end, bss_end, entry = sym['__oxbox_image_start'], sym['__oxbox_image_end'], sym['__oxbox_bss_end'], sym['oxbox_entry']
assert start == 0x3B9000
subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', '--only-section=.rodata',
                '--only-section=.data', '--only-section=.init_array', elf, os.path.join(os.path.dirname(elf), 'oxbox_image.bin')], check=True)
image = open(os.path.join(os.path.dirname(elf), 'oxbox_image.bin'), 'rb').read()
assert len(image) == end - start, (hex(len(image)), hex(end - start))

d = bytearray(open(src, 'rb').read())
base = 0x10000

# OpenCE code and data placed over the retail game's (port/oxbox/tools/gen_overlay.py): written into the retail sections' raw
# data, which the XBE loader loads at those addresses
lovl = []
for line in subprocess.run(['llvm-readelf', '-S', '-W', elf], capture_output=True, text=True).stdout.splitlines():
    m = re.search(r'\]\s+(\.lovl\d+)\s+\S+\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)', line)
    if m: lovl.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
nsec0 = struct.unpack_from('<I', d, 0x11c)[0]
sh0 = struct.unpack_from('<I', d, 0x120)[0] - base
lovl_total = 0
for name, va, size in lovl:
    if not size: continue
    subprocess.run(['llvm-objcopy', '-O', 'binary', f'--only-section={name}', elf, os.path.join(os.path.dirname(elf), 'oxbox_lovl.bin')], check=True)
    blob = open(os.path.join(os.path.dirname(elf), 'oxbox_lovl.bin'), 'rb').read()
    assert len(blob) == size, (name, len(blob), size)
    for i in range(nsec0):
        hh = sh0 + i * 0x38
        sva, svs, sraw, srs = struct.unpack_from('<IIII', d, hh + 4)
        if sva <= va and va + size <= sva + srs:
            d[sraw + va - sva:sraw + va - sva + size] = blob
            break
    else: raise SystemExit(f'{name} at {va:#x}+{size:#x} is not in a retail section\'s raw data')
    lovl_total += size
nsec = struct.unpack_from('<I', d, 0x11c)[0]
sh = struct.unpack_from('<I', d, 0x120)[0] - base
cap = sh + (nsec - 1) * 0x38
assert d[struct.unpack_from('<I', d, cap + 0x14)[0] - base:][:6] == b'.cap16'
assert struct.unpack_from('<IIII', d, cap + 4) == (0x3b5000, 0x4000, 0x26a000, 0x4000)
assert len(d) == 0x26f000 and not any(d[0x26e000:])
raw = bytes(d[0x26a000:0x26e000]) + image
vsz = ((bss_end + 0xFFF) & ~0xFFF) - 0x3b5000
d = d[:0x26a000] + raw
d += bytes((-len(d)) % 0x1000)
struct.pack_into('<III', d, cap + 8, vsz, 0x26a000, len(raw))   # virtual size, raw address, raw size
struct.pack_into('<I', d, 0x10c, 0x3b5000 + vsz - base)          # size of image

# mainXapiStartup: call main(0, 0, 0) -> call oxbox_entry(0, 0, 0)
site = 0x15cb1
o = site - 0x11000 + 0x1000
assert d[o] == 0xE8 and struct.unpack_from('<i', d, o + 1)[0] == 0x56960 - (site + 5), 'main call site'
struct.pack_into('<i', d, o + 1, entry - (site + 5))

# the retail .text section writable: OpenCE's .bss overlays the retail game code in it (0x25000-0x180C00)
tx = sh
assert d[struct.unpack_from('<I', d, tx + 0x14)[0] - base:][:6] == b'.text\0'
struct.pack_into('<I', d, tx, struct.unpack_from('<I', d, tx)[0] | 1)

# the Bink player's sections (BINK*, BINKDATA) not loaded at boot: bink_playback.c loads them for a movie
# (oxbox_sdk.c oxbox_bink_sections), so a game doesn't carry ~110 KB of movie player
for i in range(nsec):
    hh = sh + i * 0x38
    nm = struct.unpack_from('<I', d, hh + 0x14)[0] - base
    if d[nm:nm + 4] == b'BINK':
        struct.pack_into('<I', d, hh, struct.unpack_from('<I', d, hh)[0] & ~2)

# init flags: drop XINIT_LIMIT_DEVKIT_MEMORY (4), so a console with 128 MB (xemu's option) gives the title all of it
struct.pack_into('<I', d, 0x124, struct.unpack_from('<I', d, 0x124)[0] & ~4)
# DirectSound's effects image (15 KB) after the sections, where the loader does not load it: sound_dsound_xbox.c
# reads it from the file for IDirectSound_DownloadEffectsImage and frees it (oxbox_entry.c oxbox_file_blob); its
# offset and size go into the image's marker (OXFXIMG1, then offset and size)
# (the bytes come from OpenCE's own copy of the array, source/sound/sound_dsound_xbox.c's dsound_effects_image)
_snd = open(os.path.join(ROOT, 'source', 'sound', 'sound_dsound_xbox.c'), encoding='latin1').read()
_at = _snd.index('dsound_effects_image[0x3A5C]=')
fx = bytes(int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', _snd[_snd.index('{', _at) + 1:_snd.index('};', _at)]))
assert len(fx) == 0x3A5C, len(fx)
mark = d.find(b'OXFXIMG1')
assert mark > 0 and d.find(b'OXFXIMG1', mark + 1) < 0, 'effects image marker'
while len(d) % 16: d.append(0)
struct.pack_into('<II', d, mark + 8, len(d), len(fx))
d += fx
# OpenCE's pictures, the same way: read from the file straight into their textures when they're drawn, never in
# memory otherwise (oxbox_entry.c oxbox_picture_read). 16 KB each: the main menu's OPTIONS and PROFILES words (normal,
# highlighted: DXT3 256 x 64, port/oxbox/tools/mkword.py), then the postgame's gametype pictures (CTF, King, Slayer, Oddball, Race:
# A8R8G8B8 64 x 64 swizzled, port/oxbox/tools/mkicons.py)
_pictures = os.path.join(ROOT, 'port', 'oxbox', 'pictures.bin')
if os.path.exists(_pictures):
    words = open(_pictures, 'rb').read()
else:
    # (not in the repository: made from the game's own ui.map art by mkword.py and mkicons.py, see port/oxbox/README.md.
    # Without it the main menu's OPTIONS and PROFILES and the postgame's gametype pictures are blank)
    print('note: port/oxbox/pictures.bin not found: the menu words and gametype pictures will be blank')
    words = bytes(9 * 16384)
assert len(words) == 9 * 16384
mark = d.find(b'OXWORDS1')
assert mark > 0 and d.find(b'OXWORDS1', mark + 1) < 0, 'menu words marker'
while len(d) % 16: d.append(0)
struct.pack_into('<II', d, mark + 8, len(d), len(words))
d += words
cert = struct.unpack_from('<I', d, 0x118)[0] - base
# Halo's own title ID, name and keys, as in the retail XBE (test build 19): the profiles and gametypes are the retail
# game's, read and written in its formats and signed with its keys. What retail Halo can't share is kept apart:
# the campaign checkpoint (game_state_xbox.c: opence.bin, not savegame.bin) and OpenCE's own settings (T:).
# (Test builds 01-18 used their own title ID, 0x4F430001 'OC' 0001.)
TITLE_ID = struct.unpack_from('<I', d, cert + 0x8)[0]
assert TITLE_ID == 0x4D530004, hex(TITLE_ID)
for i in range(nsec):
    hh = sh + i * 0x38
    r, rs = struct.unpack_from('<II', d, hh + 0xc)
    d[hh + 0x24:hh + 0x38] = hashlib.sha1(struct.pack('<I', rs) + bytes(d[r:r + rs])).digest()
open(dst, 'wb').write(d)
print(f'overlay code and data written into the retail sections: {lovl_total // 1024} KB in {len(lovl)} sections')
print(f'wrote {dst}: {len(d)} bytes, image {start:#x}-{end:#x}, bss to {bss_end:#x}, entry {entry:#x}, '
      f'.cap16 vsize {vsz:#x}, md5 {hashlib.md5(d).hexdigest()}')
