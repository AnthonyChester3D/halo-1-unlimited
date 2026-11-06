#!/usr/bin/env python3
"""shared_pass.py: Xbox maps' texture and sound data moved into shared files (docs/shared_maps.md): the reference for
Halo Map Porter's own pass, and the test maker for the game's side.

  python3 port/oxbox/tools/shared_pass.py stock OUT_DIR STOCK_MAP...
      stock.shared from the stock multiplayer maps (what the installer builds from the player's disc), and each map
      drawing on it alone (OUT_DIR/<name>/<name>.map)
  python3 port/oxbox/tools/shared_pass.py [--stock STOCK_MAPS_DIR] NAME OUT_DIR MAP...
      a pack: NAME.shared and each MAP drawing on it (OUT_DIR/<the map's folder>/<file>, the folder's other files
      copied beside it). With --stock, data in the stock maps is taken from stock.shared instead (laid out from the
      stock maps in that folder, as `stock` lays it out).

A bitmap's pixels and a sound permutation's samples are each one block. A pack's shared file gets the blocks used by
two maps or more (data one map uses stays in it); stock.shared gets every block of the stock maps. The same bytes are
stored once, at an offset with the same remainder modulo 512 as in the map (the game reads whole sectors). The map's
offset to a block in a shared file gets its top bit set, and the next bit for stock.shared; the map is laid out again
without the moved data (whole 2048-byte sectors of it, so what stays keeps its alignment), its tag data's and BSPs'
offsets and its own blocks' moved down, and its header gets the shared reference at 0x700 (its identity in map sharing
kept: the size and checksum it had before the pass).

stock.shared's layout is fixed: the stock maps in the order of their file names, each map's tags in tag index order, a
bitmap's bitmaps then a sound's pitch ranges' permutations in block order; the first copy of each block placed after
the last, padded with zeros to its remainder modulo 512. Every console building it from the same maps gets the same file.
"""
import hashlib
import os
import shutil
import struct
import sys
import zlib

HEADER = 0x800
SECTOR = 2048
TAG_BASE = 0x803A6000
SHARED_REFERENCE_OFFSET = 0x700
BITMAP = 0x6269746D        # 'bitm'
SOUND = 0x736E6421         # 'snd!'
SCENARIO_BSPS = 0x5A4      # scenario's structure_bsp_references (0x20 each: file offset, size, ...)
PACK_BIT, STOCK_BIT = 0x80000000, 0xC0000000
LEVEL = int(os.environ.get('SHARED_PASS_LEVEL', '9'))
STOCK_NAMES = ('beavercreek', 'bloodgulch', 'boardingaction', 'carousel', 'chillout', 'damnation', 'hangemhigh',
               'longest', 'prisoner', 'putput', 'ratrace', 'sidewinder', 'wizard')


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def read_map(path):
    raw = open(path, 'rb').read()
    body = zlib.decompress(raw[HEADER:]) if raw[HEADER:HEADER + 1] == b'\x78' else raw[HEADER:]
    data = bytearray(raw[:HEADER] + body)
    if data[0:4] != b'daeh' or u32(data, 4) != 5:
        sys.exit('%s: not an Xbox cache file' % path)
    return raw, data


class Map:
    """a map's blocks (field, offset, size) and its BSPs' offset fields"""

    def __init__(self, path):
        self.path = path
        self.raw, self.data = read_map(path)
        data = self.data
        self.tag_offset, self.tag_size = u32(data, 0x10), u32(data, 0x14)
        instances, scenario, count = u32(data, self.tag_offset), u32(data, self.tag_offset + 4), \
            u32(data, self.tag_offset + 12)
        blocks = []
        for k in range(count):
            inst = self.at(instances) + k * 0x20
            group, base = u32(data, inst), u32(data, inst + 0x14)
            if not base:
                continue
            if group == BITMAP:
                # (each bitmap's own pixels: its pixels_offset is the file's, the tag's pixel data unused on the Xbox)
                b = self.at(base)
                for m in range(u32(data, b + 0x60)):
                    e = self.at(u32(data, b + 0x64)) + m * 0x30
                    blocks.append((e + 0x18, u32(data, e + 0x18), u32(data, e + 0x1C)))
            elif group == SOUND:
                s = self.at(base)
                for p in range(u32(data, s + 0x98)):
                    pr = self.at(u32(data, s + 0x9C)) + p * 0x48
                    for q in range(u32(data, pr + 0x3C)):
                        pm = self.at(u32(data, pr + 0x40)) + q * 0x7C
                        blocks.append((pm + 0x48, u32(data, pm + 0x48), u32(data, pm + 0x40)))
        self.blocks = [(f, o, n) for f, o, n in blocks if n and HEADER <= o and o + n <= len(data) and
                       not o & 0x80000000 and not (self.tag_offset < o + n and o < self.tag_offset + self.tag_size)]
        s = self.at(u32(data, self.at(instances) + (scenario & 0xFFFF) * 0x20 + 0x14))
        self.bsp_fields = [self.at(u32(data, s + SCENARIO_BSPS + 4)) + k * 0x20 for k in range(u32(data, s + SCENARIO_BSPS))]

    def at(self, address):
        o = self.tag_offset + address - TAG_BASE
        if not self.tag_offset <= o < self.tag_offset + self.tag_size:
            raise ValueError('%s: %08x is outside the tag data' % (self.path, address))
        return o

    def keys(self):
        """each block's key: its bytes' hash and its offset's remainder modulo 512"""
        return [(hashlib.sha1(bytes(self.data[o:o + n])).digest(), o % 512) for f, o, n in self.blocks]


class Shared:
    """a shared file being laid out"""

    def __init__(self):
        self.data = bytearray(HEADER)
        self.placed = {}

    def place(self, key, block):
        if key not in self.placed:
            self.data += bytes((key[1] - len(self.data) % 512) % 512)
            self.placed[key] = len(self.data)
            self.data += block
        return self.placed[key]

    def finish(self, name):
        self.data += bytes(-len(self.data) % SECTOR)
        self.checksum = zlib.crc32(bytes(self.data[HEADER:])) & 0xFFFFFFFF
        header = bytearray(HEADER)
        struct.pack_into('<4sII', header, 0, b'daeh', 5, len(self.data))
        header[0x20:0x20 + len(name)] = name.encode()
        header[0x40:0x46] = b'shared'
        struct.pack_into('<hhI', header, 0x60, 0, 0, self.checksum)
        header[0x7FC:0x800] = b'toof'
        self.data[:HEADER] = header
        self.name = name


def stock_layout(paths):
    """stock.shared from the stock maps (in the order of their file names), and the maps"""
    stock = Shared()
    maps = [Map(p) for p in sorted(paths, key=lambda p: os.path.basename(p).lower())]
    for m in maps:
        for (f, o, n), key in zip(m.blocks, m.keys()):
            stock.place(key, m.data[o:o + n])
    stock.finish('stock')
    return stock, maps


def write_cache_file(path, data):
    out = bytes(data[:HEADER]) + zlib.compress(bytes(data[HEADER:]), LEVEL)
    out += bytes(-len(out) % SECTOR)     # (whole sectors, as Xbox maps are: the game copies them unbuffered)
    open(path, 'wb').write(out)
    return len(out)


def rebuild(m, where, pack, stock):
    """the map drawing on the shared files: where[i] = (bit, offset) for a block that moved, None for one that stays"""
    data = bytearray(m.data)
    moved = []
    for (f, o, n), w in zip(m.blocks, where):
        if w:
            struct.pack_into('<I', data, f, w[0] | w[1])
            moved.append((o, o + n))
    for (f, o, n), w in zip(m.blocks, where):
        if w:
            data[o:o + n] = bytes(n)     # (what's left of it in a sector that stays: zeros, which compress away)
    for (f, o, n), w in zip(m.blocks, where):
        if not w:
            data[o:o + n] = m.data[o:o + n]     # (a block that stays, sharing a sector or bytes with one that moved)
    # the sectors wholly of moved data go; what stays moves down by whole sectors
    covered = bytearray(len(data) // SECTOR + 1)
    moved.sort()
    merged = []
    for a, b in moved:
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    for a, b in merged:
        for c in range((a + SECTOR - 1) // SECTOR, b // SECTOR):
            covered[c] = 1
    covered[0] = 0
    shift = [0] * len(covered)
    gone = 0
    for c in range(len(covered)):
        shift[c] = gone
        gone += covered[c]

    def moved_down(o):
        if covered[o // SECTOR]:
            raise ValueError('%s: %08x lies in data that moved' % (m.path, o))
        return o - shift[o // SECTOR] * SECTOR

    for (f, o, n), w in zip(m.blocks, where):
        if not w:
            struct.pack_into('<I', data, f, moved_down(o))
    for field in m.bsp_fields:
        struct.pack_into('<I', data, field, moved_down(u32(data, field)))
    struct.pack_into('<I', data, 0x10, moved_down(m.tag_offset))
    out = bytearray(data[:SECTOR])
    for c in range(1, len(data) // SECTOR + (1 if len(data) % SECTOR else 0)):
        if not covered[c]:
            out += data[c * SECTOR:(c + 1) * SECTOR]
    struct.pack_into('<I', out, 8, len(out))
    reference = struct.pack('<Il32sIlII32sIl', 0x73687264, 2,
                            pack.name.encode() if pack else b'', pack.checksum if pack else 0,
                            len(pack.data) if pack else 0, len(m.raw), u32(m.raw, 0x64),
                            stock.name.encode() if stock else b'', stock.checksum if stock else 0,
                            len(stock.data) if stock else 0)
    out[SHARED_REFERENCE_OFFSET:SHARED_REFERENCE_OFFSET + len(reference)] = reference
    return out


def main():
    args = sys.argv[1:]
    stock_dir = None
    if args[:1] == ['--stock']:
        stock_dir, args = args[1], args[2:]
    if len(args) < 3:
        sys.exit(__doc__)
    name, out_dir, paths = args[0], args[1], args[2:]
    if len(name) > 30 or any(c in name for c in '\\/:*?"<>|'):
        sys.exit('the shared file name must be 1 to 30 characters, none of \\/:*?"<>|')
    os.makedirs(out_dir, exist_ok=True)
    total_before = total_after = 0
    if name == 'stock':
        stock, maps = stock_layout(paths)
        size = write_cache_file(os.path.join(out_dir, 'stock.shared'), stock.data)
        for m in maps:
            where = [(STOCK_BIT, stock.placed[k]) for k in m.keys()]
            base = os.path.splitext(os.path.basename(m.path))[0]
            os.makedirs(os.path.join(out_dir, base), exist_ok=True)
            out = rebuild(m, where, None, stock)
            n = write_cache_file(os.path.join(out_dir, base, base + '.map'), out)
            print('%s: %d KB -> %d KB (%d KB unpacked)' % (base, len(m.raw) >> 10, n >> 10, len(out) >> 10))
            total_before += len(m.raw)
            total_after += n
        print('stock.shared: %d KB (%d KB unpacked)' % (size >> 10, len(stock.data) >> 10))
        print('in all: %d KB -> %d KB' % (total_before >> 10, (total_after + size) >> 10))
        return
    stock = None
    if stock_dir:
        stock_paths = [os.path.join(stock_dir, f) for f in os.listdir(stock_dir)
                       if f.lower().endswith('.map') and f.lower()[:-4] in STOCK_NAMES]
        stock, _ = stock_layout(stock_paths)
    maps = [Map(p) for p in paths]
    keys = [m.keys() for m in maps]
    used = {}
    for ks in keys:
        for k in set(ks):
            used[k] = used.get(k, 0) + 1
    pack = Shared()
    wheres = []
    for m, ks in zip(maps, keys):
        where = []
        for (f, o, n), k in zip(m.blocks, ks):
            if stock and k in stock.placed:
                where.append((STOCK_BIT, stock.placed[k]))
            elif used[k] >= 2:
                where.append((PACK_BIT, pack.place(k, m.data[o:o + n])))
            else:
                where.append(None)
        wheres.append(where)
    has_pack = len(pack.data) > HEADER
    pack.finish(name)
    size = write_cache_file(os.path.join(out_dir, name + '.shared'), pack.data) if has_pack else 0
    for m, where in zip(maps, wheres):
        folder = os.path.basename(os.path.dirname(os.path.abspath(m.path)))
        dest = os.path.join(out_dir, folder)
        os.makedirs(dest, exist_ok=True)
        out = rebuild(m, where, pack if has_pack else None, stock)
        n = write_cache_file(os.path.join(dest, os.path.basename(m.path)), out)
        for other in os.listdir(os.path.dirname(os.path.abspath(m.path))):
            if not other.lower().endswith('.map'):
                shutil.copy(os.path.join(os.path.dirname(os.path.abspath(m.path)), other), dest)
        kept = sum(b[2] for b, w in zip(m.blocks, where) if not w)
        print('%s/%s: %d KB -> %d KB (%d KB unpacked; %d KB of its own textures and sounds kept in it)' % (
            folder, os.path.basename(m.path), len(m.raw) >> 10, n >> 10, len(out) >> 10, kept >> 10))
        total_before += len(m.raw)
        total_after += n
    if has_pack:
        print('%s.shared: %d KB (%d KB unpacked)' % (name, size >> 10, len(pack.data) >> 10))
    print('in all: %d KB -> %d KB%s' % (total_before >> 10, (total_after + size) >> 10,
                                        ' (and stock.shared)' if stock else ''))


if __name__ == '__main__':
    main()
