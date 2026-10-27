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

def add_section(d, name, flags, va, vsize, raw, rsize):
    """a section header added after the last one (the loader takes the section's raw data from raw, rsize bytes, and
    maps vsize bytes at va). The XBE header (one page, 0x10000-0x10FFF, under .text) has no room after its section
    headers: what follows them (the shared page counts, the section names, the library versions, the debug names and
    the logo) moves up by one header, and the new section's page counts (its own pages: none shared) and name go
    after it, in the page's unused end, which the header's size grows to take"""
    u = lambda o: struct.unpack_from('<I', d, o)[0]
    nsec, sh = u(0x11c), u(0x120) - base
    at = sh + nsec * 0x38                      # the new section header's place
    ends = [at]                                # the header's used end: as far as its pointers reach
    for i in range(nsec):
        hh = sh + i * 0x38
        ends += [d.index(b'\0', u(hh + 0x14) - base) + 1, u(hh + 0x1c) - base + 2, u(hh + 0x20) - base + 2]
    ends += [u(0x164) - base + 16 * u(0x160), u(0x168) - base + 16, u(0x16c) - base + 16, u(0x170) - base + u(0x174),
             d.index(b'\0', u(0x14c) - base) + 1, d.index(b'\0', u(0x150) - base) + 1]
    e = u(0x154) - base
    while d[e:e + 2] != b'\0\0': e += 2
    used = max(ends + [e + 2])
    assert not any(d[used:u(0x108)]), 'the XBE header past its used end'
    counts = (used + 0x38 + 3) & ~3
    name_at = counts + 4
    room = min(u(sh + i * 0x38 + 0xc) for i in range(nsec))   # (the first section's raw data)
    assert name_at + len(name) + 1 <= room, f'no room in the XBE header for {name}'
    assert not any(d[used:name_at + len(name) + 1]), 'the XBE header page past its used end'
    d[at + 0x38:used + 0x38] = d[at:used]
    d[at:at + 0x38] = bytes(0x38)
    def moved(o):
        p = u(o)
        if base + at <= p < base + used: struct.pack_into('<I', d, o, p + 0x38)
    for i in range(nsec):
        for f in (0x14, 0x1c, 0x20): moved(sh + i * 0x38 + f)
    for o in (0x14c, 0x150, 0x154, 0x164, 0x168, 0x16c, 0x170): moved(o)
    d[counts:counts + 4] = bytes(4)
    d[name_at:name_at + len(name) + 1] = name.encode() + b'\0'
    struct.pack_into('<IIIIIIIII', d, at, flags, va, vsize, raw, rsize, base + name_at, 0, base + counts, base + counts + 2)
    struct.pack_into('<I', d, 0x11c, nsec + 1)
    if u(0x108) < name_at + len(name) + 1:
        struct.pack_into('<I', d, 0x108, (name_at + len(name) + 1 + 3) & ~3)   # size of headers

def section_name(d, i):
    sh = struct.unpack_from('<I', d, 0x120)[0] - base
    nm = struct.unpack_from('<I', d, sh + i * 0x38 + 0x14)[0] - base
    return bytes(d[nm:d.index(b'\0', nm)]).decode('latin-1')

# a clean retail XBE (NTSC, version 8 or 9), or one without the 16-player XBE's .cap16 (16 KB at 0x3B5000, the section
# this port first went into): an empty .cap16 added, as that XBE has it, so the image goes at 0x3B9000 either way.
# Every retail section stays where it was; the retail certificate (title, keys, media, region) is kept
if section_name(d, struct.unpack_from('<I', d, 0x11c)[0] - 1) != '.cap16':
    nsec0 = struct.unpack_from('<I', d, 0x11c)[0]
    last = struct.unpack_from('<I', d, 0x120)[0] - base + (nsec0 - 1) * 0x38
    assert section_name(d, nsec0 - 1) == '$$XSIMAGE' and \
        struct.unpack_from('<IIII', d, last + 4) == (0x3b37a0, 0x1000, 0x269000, 0x1000), \
        'not a retail Halo XBE (NTSC, version 8 or 9) nor the 16-player one'
    assert len(d) == 0x26b000 and not any(d[0x26a000:]), 'not a retail Halo XBE: its end'
    d = d[:0x26a000] + bytearray(0x4000 + 0x1000)   # .cap16's raw data, then the empty page the 16-player XBE has
    add_section(d, '.cap16', 7, 0x3b5000, 0x4000, 0x26a000, 0x4000)   # writable (1), preload (2), executable (4)
    struct.pack_into('<I', d, 0x10c, 0x3b9000 - base)                   # size of image

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

# the developers' perf log (port/oxbox/oxbox_perf.c: link.ld's .oxperf and .oxperf.bss) as a section of its own, OXPERF,
# after .cap16: not loaded at boot (no preload flag), so with perf_log off it takes no memory; oxbox_sdk.c loads it
# with the kernel's XeLoadSection when opence.ini turns the log on (add_section).
perf_start, perf_load_end, perf_end = sym['__oxperf_start'], sym['__oxperf_load_end'], sym['__oxperf_end']
perf_size = 0
if perf_end > perf_start:
    subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.oxperf', elf, os.path.join(os.path.dirname(elf), 'oxbox_perf.bin')], check=True)
    perf = open(os.path.join(os.path.dirname(elf), 'oxbox_perf.bin'), 'rb').read()
    assert len(perf) == perf_load_end - perf_start, (hex(len(perf)), hex(perf_load_end - perf_start))
    assert perf_start == 0x3b5000 + vsz and not perf_start & 0xFFF
    perf_raw = len(d)
    d += perf + bytes((-len(perf)) % 0x1000)
    perf_size = ((perf_end + 0xFFF) & ~0xFFF) - perf_start
    # flags: writable (1), executable (4); not preload (2)
    add_section(d, 'OXPERF', 5, perf_start, perf_end - perf_start, perf_raw, len(perf))
    struct.pack_into('<I', d, 0x10c, perf_start + perf_size - base)   # size of image
    nsec += 1

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
# highlighted: DXT3 256 x 64), then the postgame's gametype pictures (CTF, King, Slayer, Oddball, Race: A8R8G8B8 64 x 64
# swizzled): port/oxbox/installer/pictures.c, which the installer runs on the Xbox too
_pictures = os.path.join(ROOT, 'port', 'oxbox', 'pictures.bin')
if os.path.exists(_pictures):
    words = open(_pictures, 'rb').read()
else:
    # (not in the repository: made from the game's own ui.map art by installer/mkpictures, see port/oxbox/README.md.
    # Without it the main menu's OPTIONS and PROFILES and the postgame's gametype pictures are blank)
    print('note: port/oxbox/pictures.bin not found: the menu words and gametype pictures will be blank')
    words = bytes(9 * 16384)
assert len(words) == 9 * 16384
mark = d.find(b'OXWORDS1')
assert mark > 0 and d.find(b'OXWORDS1', mark + 1) < 0, 'menu words marker'
while len(d) % 16: d.append(0)
struct.pack_into('<II', d, mark + 8, len(d), len(words))
d += words
# the online test builds' ONLINE GAMES header (DXT3 512 x 64, port/oxbox/installer/pictures.c), if the build has its marker
# (OXONLIN1: oxbox_entry.c, OXBOX_NET_P2P only), the same way
mark = d.find(b'OXONLIN1')
if mark > 0:
    _header = os.path.join(ROOT, 'port', 'oxbox', 'online_games.dxt3')
    if os.path.exists(_header):
        header = open(_header, 'rb').read()
    else:
        # (not in the repository: made from the game's own ui.map art by installer/mkpictures. Without it ONLINE PLAY's games
        # list has no header)
        print('note: port/oxbox/online_games.dxt3 not found: the ONLINE GAMES header will be blank')
        header = bytes(32768)
    assert len(header) == 32768 and d.find(b'OXONLIN1', mark + 1) < 0
    while len(d) % 16: d.append(0)
    struct.pack_into('<II', d, mark + 8, len(d), len(header))
    d += header
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
print(f'OXPERF (the perf log, loaded only with perf_log = 1): {perf_end - perf_start} bytes at {perf_start:#x} '
      f'({perf_load_end - perf_start} from the file, {perf_size // 4096} pages)' if perf_size else 'OXPERF: none')
print(f'wrote {dst}: {len(d)} bytes, image {start:#x}-{end:#x}, bss to {bss_end:#x}, entry {entry:#x}, '
      f'.cap16 vsize {vsz:#x}, md5 {hashlib.md5(d).hexdigest()}')
