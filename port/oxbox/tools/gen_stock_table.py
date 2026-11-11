#!/usr/bin/env python3
"""gen_stock_table.py RETAIL_MAPS_DIR OUT.h: the installer's table of the retail maps (installer/stockmaps_table.h): each file's
size and CRC, and unpacked, as shared_pass.py reads them; where each of its blocks is in stock.shared (a level's blocks that
fewer than three levels use stay in it); and which it's the first copy of, by offset: all a console needs to build stock.shared
in one pass through the maps, with no hashing and no counting; each small map's length and CRC32 unpacked (what the console's
must come out as) and its file's size packed (zlib level 1, as the console packs it: where the one pass makes room for it);
where each map's moved data is (its blocks that go in stock.shared, merged into ranges as the installer's cover() merges them,
and the blocks that stay where they overlap those), packed with zlib: the one pass makes the small map as it reads the map,
before it reaches the tags at its end (it finds the blocks in them as before, for the fields and the restore table); the most
a map has from its tags' first sector to its end; and stock.index's entries (each block's SHA-256, place and size), the same on every console.
Each small map also reuses its retail map's own packed data where it can: the runs of the retail stream's deflate blocks whose
bytes (and the 32 KB before each, which a block can refer back to) the small map has unchanged go into it bit for bit, each run
followed by an empty stored block (so what follows starts on a byte), and only the rest is packed (zlib level 1, raw, from a
fresh start after each run): a console packs a fifth of what it did. The table has each map's runs (where they are unpacked,
and in the retail file's bits, and the Adler-32 of their bytes) and the small map's file as it must come out (its size and
CRC); and each retail map's CRC unpacked (the restore table's: the file's CRC and zlib's own check say it's that). It holds no
data of the maps' but those figures."""
import ctypes
import ctypes.util
import hashlib
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_pass as sp


def merge(ranges):
    """ranges sorted and merged where they overlap or touch (as stockmaps.c's cover() runs them together)"""
    out = []
    for a, b in sorted(ranges):
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


class ZStream(ctypes.Structure):
    _fields_ = [('next_in', ctypes.c_void_p), ('avail_in', ctypes.c_uint), ('total_in', ctypes.c_ulong),
                ('next_out', ctypes.c_void_p), ('avail_out', ctypes.c_uint), ('total_out', ctypes.c_ulong),
                ('msg', ctypes.c_char_p), ('state', ctypes.c_void_p), ('zalloc', ctypes.c_void_p), ('zfree', ctypes.c_void_p),
                ('opaque', ctypes.c_void_p), ('data_type', ctypes.c_int), ('adler', ctypes.c_ulong), ('reserved', ctypes.c_ulong)]


def deflate_blocks(raw, length):
    """the retail file's deflate blocks (its zlib stream after the header), each (its first bit in the file, where its bytes
    start unpacked, and its type), ending with (the stream's end, length, None): zlib's inflate stopped at each (Z_BLOCK)"""
    lib = ctypes.CDLL(ctypes.util.find_library('z'))
    lib.zlibVersion.restype = ctypes.c_char_p
    lib.inflateInit_.argtypes = [ctypes.POINTER(ZStream), ctypes.c_char_p, ctypes.c_int]
    lib.inflate.argtypes = [ctypes.POINTER(ZStream), ctypes.c_int]
    lib.inflateEnd.argtypes = [ctypes.POINTER(ZStream)]
    z = ZStream()
    src = ctypes.create_string_buffer(bytes(raw[sp.HEADER:]), len(raw) - sp.HEADER)
    out = ctypes.create_string_buffer(length)
    version = lib.zlibVersion()
    assert lib.inflateInit_(ctypes.byref(z), version, ctypes.sizeof(z)) == 0
    z.next_in, z.avail_in = ctypes.addressof(src), len(raw) - sp.HEADER
    z.next_out, z.avail_out = ctypes.addressof(out), length
    starts = []
    while True:
        status = lib.inflate(ctypes.byref(z), 5)  # Z_BLOCK
        if status == 1:  # Z_STREAM_END
            break
        assert status == 0, status
        if z.data_type & 128:
            bit = (sp.HEADER + z.total_in) * 8 - (z.data_type & 7)
            starts.append((bit, sp.HEADER + z.total_out, (raw[bit // 8] | raw[bit // 8 + 1] << 8) >> (bit % 8 + 1) & 3))
    lib.inflateEnd(ctypes.byref(z))
    return starts + [((len(raw)) * 8, length, None)]



def reuse_runs(m, small, moved, tail_at):
    """the runs of the retail map's deflate blocks the small map can take as they are: (from, to unpacked, first bit, end bit),
    the same in the small map, and the most of them that waits in memory: the blocks after tail_at (the small map's part from
    there on is made once the tags are read), each run's empty stored block too"""
    S, length = sp.SECTOR, len(m.data)
    sectors = length // S + 1
    covered = bytearray(sectors + 1)
    for a, b in moved:
        for c in range((a + S - 1) // S, b // S):
            covered[c] = 1
    covered[0] = 0
    before, gone = [0] * (sectors + 1), 0
    for c in range(sectors + 1):
        before[c] = gone
        if c < sectors:
            gone += covered[c]
    assert len(small) == length - gone * S
    same = bytearray(sectors)
    for c in range(1, sectors):
        if not covered[c]:
            j, a = c - before[c], m.data[c * S:(c + 1) * S]
            same[c] = a == small[j * S:j * S + len(a)]
    blocks = deflate_blocks(m.raw, length)
    runs, waiting = [], 0
    # (a block refers back 32 KB at most: it's the same in the small map if those and its own bytes are. Not the last block, nor a
    # stored one: its bytes start on a byte of its own)
    for (bit, a, kind), (end_bit, b, next_kind) in zip(blocks, blocks[1:]):
        if next_kind is None or kind == 0 or b <= a:
            continue
        if all(same[c] for c in range(max(S, a - 32768) // S, (b - 1) // S + 1)):
            if runs and runs[-1][1] == a:
                runs[-1][1], runs[-1][3] = b, end_bit
            else:
                runs.append([a, b, bit, end_bit])
                waiting += 7 if b > tail_at else 0
            if b > tail_at:
                waiting += (end_bit - bit + 7) // 8 + 1
    small_at = lambda o: o - before[(o - 1) // S] * S  # (an offset unpacked in the small map: no sector inside a run is covered)
    for r in runs:
        r.append(zlib.adler32(bytes(m.data[r[0]:r[1]])))  # (its Adler-32: the console needn't count it)
    return runs, [(small_at(a), small_at(b), c, d) for a, b, c, d, e in runs], waiting


def spliced(raw, small, runs):
    """the small map's file as the console makes it: the header, then one zlib stream of the runs (bit for bit, each with an empty
    stored block after it) and the rest packed at level 1 from a fresh start each time, padded to a sector (runs: where they are
    in the small map)"""
    out, at = bytearray(small[:sp.HEADER]) + b'\x78\x01', sp.HEADER
    for a, b, bit, end_bit in runs:
        if a > at:
            z = zlib.compressobj(1, zlib.DEFLATED, -15, 8)
            out += z.compress(bytes(small[at:a])) + z.flush(zlib.Z_SYNC_FLUSH)
        n = end_bit - bit
        v = int.from_bytes(raw[bit // 8:(end_bit + 7) // 8], 'little') >> (bit % 8) & ((1 << n) - 1)
        out += v.to_bytes((n + 3 + 7) // 8, 'little') + b'\x00\x00\xff\xff'
        at = b
    z = zlib.compressobj(1, zlib.DEFLATED, -15, 8)
    out += z.compress(bytes(small[at:])) + z.flush(zlib.Z_FINISH)
    out += struct.pack('>I', zlib.adler32(bytes(small[sp.HEADER:])))
    out += bytes((sp.SECTOR - len(out) % sp.SECTOR) % sp.SECTOR)
    return bytes(out)


def main():
    folder, out = sys.argv[1], sys.argv[2]
    path = lambda n: os.path.join(folder, n + '.map')
    stock, _ = sp.stock_layout([path(n) for n in sp.STOCK_NAMES], [path(n) for n in sp.LEVEL_NAMES])
    lines = ['/* stockmaps_table.h: made by tools/gen_stock_table.py from the retail maps (the same on the three NTSC discs) */',
             '#define STOCK_LENGTH %uu' % len(stock.data), '#define STOCK_CHECKSUM 0x%08xu' % stock.checksum, '']
    rows, most_blocks, most_block, most_tags, most_length = [], 0, 0, 0, 0
    written = set()
    layout, most_tail = bytearray(), 0
    most_pending, most_runs, all_runs = 0, 0, []
    for n in sp.STOCK_NAMES + sp.LEVEL_NAMES:
        m = sp.Map(path(n))
        keys = m.keys()
        most_blocks = max(most_blocks, len(m.blocks))
        most_block = max([most_block] + [b[2] for b in m.blocks])
        most_tags = max(most_tags, m.tag_size)
        most_length = max(most_length, len(m.data))
        places = [stock.placed.get(k, 0xFFFFFFFF) for k in keys]
        firsts = []
        for (f, o, s), k in zip(m.blocks, keys):
            if k in stock.placed and k not in written:
                written.add(k)
                firsts.append((o, s, stock.placed[k]))
        firsts.sort()
        if n in sp.STOCK_NAMES:
            small = sp.rebuild(m, [(sp.STOCK_BIT, stock.placed[k]) for k in keys], None, stock, pad=False)
        else:
            small = sp.rebuild(m, [(sp.STOCK_BIT, stock.placed[k]) if k in stock.placed else None for k in keys], None, stock, pad=False)
        # (each map: how many moved ranges and staying ones, then each range as the gap from the last one's end and its size)
        moved = merge([(o, o + sz) for (f, o, sz), p in zip(m.blocks, places) if p != 0xFFFFFFFF])
        # (the file the console writes, reusing the retail map's packed data: checked by unpacking it)
        runs, in_small, waiting = reuse_runs(m, small, moved, m.tag_offset // sp.SECTOR * sp.SECTOR)
        most_pending = max(most_pending, waiting)
        most_runs = max(most_runs, len(runs))
        file = spliced(m.raw, small, in_small)
        assert zlib.decompress(file[sp.HEADER:]) == bytes(small[sp.HEADER:])
        packed = len(file)
        all_runs.append((n, runs))
        stays = [r for r in merge([(o, o + sz) for (f, o, sz), p in zip(m.blocks, places) if p == 0xFFFFFFFF])
                 if any(a < r[1] and r[0] < b for a, b in moved)]
        layout += struct.pack('<II', len(moved), len(stays))
        for ranges in (moved, stays):
            end = 0
            for a, b in ranges:
                layout += struct.pack('<II', a - end, b - a)
                end = b
        most_tail = max(most_tail, len(m.data) - m.tag_offset // sp.SECTOR * sp.SECTOR)
        lines.append('static uint32_t const places_%s[%d] = {' % (n, len(places)))
        for at in range(0, len(places), 12):
            lines.append('\t' + ','.join('%uu' % p for p in places[at:at + 12]) + ',')
        lines.append('};')
        lines.append('static struct reuse_run const runs_%s[%d] = {' % (n, max(len(runs), 1)))
        for r in runs:
            lines.append('\t{%uu,%uu,%uu,%uu,0x%08xu},' % tuple(r))
        if not runs:
            lines.append('\t{0,0,0,0,0},')
        lines.append('};')
        lines.append('static struct first_copy const firsts_%s[%d] = {' % (n, max(len(firsts), 1)))
        for f in firsts:
            lines.append('\t{%uu,%uu,%uu},' % f)
        if not firsts:
            lines.append('\t{0,0,0},')
        lines.append('};')
        rows.append('\t{"%s", %uu, 0x%08xu, %uu, 0x%08xu, 0x%08xu, %d, places_%s, firsts_%s, %d, %uu, 0x%08xu, %uu, 0x%08xu, runs_%s, %d},' % (n,
                    len(m.raw), zlib.crc32(m.raw) & 0xFFFFFFFF, len(m.data), sp.u32(m.data, 0x64), zlib.crc32(bytes(m.data)) & 0xFFFFFFFF,
                    len(m.blocks), n, n, len(firsts), len(small), zlib.crc32(bytes(small)) & 0xFFFFFFFF, packed, zlib.crc32(file) & 0xFFFFFFFF,
                    n, len(runs)))
        print(n, 'runs', len(runs), 'reused %.1f MB of %.1f' % (sum(r[1] - r[0] for r in runs) / 1e6, len(small) / 1e6), 'file %.1f MB' % (packed / 1e6), flush=True)
    assert len(written) == len(stock.placed)
    lines += ['', '#define MOST_MAP_BLOCKS %d' % most_blocks, '#define MOST_BLOCK_SIZE %uu' % most_block,
              '#define MOST_TAG_SIZE %uu' % most_tags, '#define MOST_MAP_LENGTH %uu' % most_length,
              '#define MOST_TAIL %uu' % most_tail, '#define MOST_RUNS %d' % most_runs, '#define MOST_PENDING %uu' % most_pending, '#define STOCK_BLOCKS %d' % len(stock.placed), '#define STOCK_LEVELS %d' % len(sp.LEVEL_NAMES), '',
              'static struct retail_map const retail_maps[STOCK_MAPS] = {'] + rows + ['};', '']
    # stock.index's entries, in stock.shared's order
    # the maps' blocks and BSPs, packed
    packed_layout = zlib.compress(bytes(layout), 9)
    lines += ['#define STOCK_LAYOUT_LENGTH %uu' % len(layout), '#define STOCK_LAYOUT_CRC 0x%08xu' % (zlib.crc32(bytes(layout)) & 0xFFFFFFFF),
              'static unsigned char const stock_layout[%d] = {' % len(packed_layout)]
    for at in range(0, len(packed_layout), 32):
        lines.append('\t' + ','.join('%u' % b for b in packed_layout[at:at + 32]) + ',')
    lines += ['};', '']
    lines.append('static struct index_block const stock_index[STOCK_BLOCKS] = {')
    for key in sorted(stock.placed, key=stock.placed.get):
        at, size = stock.placed[key], stock.sizes[key]
        digest = hashlib.sha256(bytes(stock.data[at:at + size])).digest()
        lines.append('\t{{%s}, %uu, %uu},' % (','.join('%u' % b for b in digest), at, size))
    lines += ['};', '']
    open(out, 'w').write('\n'.join(lines))
    for l in lines:
        if l.startswith('#define'):
            print(l)


main()
