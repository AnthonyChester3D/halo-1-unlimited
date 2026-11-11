#!/usr/bin/env python3
"""gen_stock_table.py RETAIL_MAPS_DIR OUT.h: the installer's table of the retail maps (installer/stockmaps_table.h): each file's
size and CRC, and unpacked, as shared_pass.py reads them; where each of its blocks is in stock.shared (a level's blocks that
fewer than three levels use stay in it); and which it's the first copy of, by offset: all a console needs to build stock.shared
in one pass through the maps, with no hashing and no counting; each small map's length and CRC32 unpacked (what the console's
must come out as) and its file's size packed (zlib level 1, as the console packs it: where the one pass makes room for it);
where each map's moved data is (its blocks that go in stock.shared, merged into ranges as the installer's cover() merges them,
and the blocks that stay where they overlap those), packed with zlib: the one pass makes the small map as it reads the map,
before it reaches the tags at its end (it finds the blocks in them as before, for the fields and the restore table); the most
a map has from its tags' first sector to its end; and stock.index's entries (each block's SHA-256, place and size), the same on every console. It holds no data
of the maps' but those figures."""
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


def main():
    folder, out = sys.argv[1], sys.argv[2]
    path = lambda n: os.path.join(folder, n + '.map')
    stock, _ = sp.stock_layout([path(n) for n in sp.STOCK_NAMES], [path(n) for n in sp.LEVEL_NAMES])
    lines = ['/* stockmaps_table.h: made by tools/gen_stock_table.py from the retail maps (the same on the three NTSC discs) */',
             '#define STOCK_LENGTH %uu' % len(stock.data), '#define STOCK_CHECKSUM 0x%08xu' % stock.checksum, '']
    rows, most_blocks, most_block, most_tags, most_length = [], 0, 0, 0, 0
    written = set()
    layout, most_tail = bytearray(), 0
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
        # (the file the console writes: the header as it is, the rest one zlib stream at level 1, padded to a sector)
        z = zlib.compressobj(1, zlib.DEFLATED, 15, 8)
        packed = sp.HEADER + len(z.compress(bytes(small[sp.HEADER:])) + z.flush())
        packed += (sp.SECTOR - packed % sp.SECTOR) % sp.SECTOR
        # (each map: how many moved ranges and staying ones, then each range as the gap from the last one's end and its size)
        moved = merge([(o, o + sz) for (f, o, sz), p in zip(m.blocks, places) if p != 0xFFFFFFFF])
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
        lines.append('static struct first_copy const firsts_%s[%d] = {' % (n, max(len(firsts), 1)))
        for f in firsts:
            lines.append('\t{%uu,%uu,%uu},' % f)
        if not firsts:
            lines.append('\t{0,0,0},')
        lines.append('};')
        rows.append('\t{"%s", %uu, 0x%08xu, %uu, 0x%08xu, %d, places_%s, firsts_%s, %d, %uu, 0x%08xu, %uu},' % (n,
                    len(m.raw), zlib.crc32(m.raw) & 0xFFFFFFFF, len(m.data), sp.u32(m.data, 0x64), len(m.blocks), n, n,
                    len(firsts), len(small), zlib.crc32(bytes(small)) & 0xFFFFFFFF, packed))
    assert len(written) == len(stock.placed)
    lines += ['', '#define MOST_MAP_BLOCKS %d' % most_blocks, '#define MOST_BLOCK_SIZE %uu' % most_block,
              '#define MOST_TAG_SIZE %uu' % most_tags, '#define MOST_MAP_LENGTH %uu' % most_length,
              '#define MOST_TAIL %uu' % most_tail, '#define STOCK_BLOCKS %d' % len(stock.placed), '#define STOCK_LEVELS %d' % len(sp.LEVEL_NAMES), '',
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
