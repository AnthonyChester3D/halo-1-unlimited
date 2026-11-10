#!/usr/bin/env python3
"""gen_stock_table.py RETAIL_MAPS_DIR OUT.h: the installer's table of the retail maps (installer/stockmaps_table.h): each file's
size and CRC, and unpacked, as shared_pass.py reads them; where each of its blocks is in stock.shared (a level's blocks that
fewer than three levels use stay in it); and which it's the first copy of, by offset: all a console needs to build stock.shared
in one pass through the maps, with no hashing and no counting; each small map's length and CRC32 unpacked (what the console's
must come out as); and stock.index's entries (each block's SHA-256, place and size), the same on every console. It holds no data
of the maps' but those figures."""
import hashlib
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_pass as sp


def main():
    folder, out = sys.argv[1], sys.argv[2]
    path = lambda n: os.path.join(folder, n + '.map')
    stock, _ = sp.stock_layout([path(n) for n in sp.STOCK_NAMES], [path(n) for n in sp.LEVEL_NAMES])
    lines = ['/* stockmaps_table.h: made by tools/gen_stock_table.py from the retail maps (the same on the three NTSC discs) */',
             '#define STOCK_LENGTH %uu' % len(stock.data), '#define STOCK_CHECKSUM 0x%08xu' % stock.checksum, '']
    rows, most_blocks, most_block, most_tags, most_length = [], 0, 0, 0, 0
    written = set()
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
        rows.append('\t{"%s", %uu, 0x%08xu, %uu, 0x%08xu, %d, places_%s, firsts_%s, %d, %uu, 0x%08xu},' % (n, len(m.raw),
                    zlib.crc32(m.raw) & 0xFFFFFFFF, len(m.data), sp.u32(m.data, 0x64), len(m.blocks), n, n, len(firsts),
                    len(small), zlib.crc32(bytes(small)) & 0xFFFFFFFF))
    assert len(written) == len(stock.placed)
    lines += ['', '#define MOST_MAP_BLOCKS %d' % most_blocks, '#define MOST_BLOCK_SIZE %uu' % most_block,
              '#define MOST_TAG_SIZE %uu' % most_tags, '#define MOST_MAP_LENGTH %uu' % most_length,
              '#define STOCK_BLOCKS %d' % len(stock.placed), '#define STOCK_LEVELS %d' % len(sp.LEVEL_NAMES), '',
              'static struct retail_map const retail_maps[STOCK_MAPS] = {'] + rows + ['};', '']
    # stock.index's entries, in stock.shared's order
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
