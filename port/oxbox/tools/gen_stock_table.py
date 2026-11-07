#!/usr/bin/env python3
"""gen_stock_table.py RETAIL_MAPS_DIR OUT.h: the installer's table of the retail maps (installer/stockmaps_table.h): each file's
size and CRC, and unpacked, as shared_pass.py reads them; where each of its blocks is in stock.shared (a level's blocks that
fewer than three levels use stay in it); and which it's the first copy of, by offset: all a console needs to build stock.shared
in one pass through the maps, with no hashing and no counting. It holds no data of the maps' but those figures."""
import os
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
        rows.append('\t{"%s", %uu, 0x%08xu, %uu, 0x%08xu, %d, places_%s, firsts_%s, %d},' % (n, len(m.raw),
                    zlib.crc32(m.raw) & 0xFFFFFFFF, len(m.data), sp.u32(m.data, 0x64), len(m.blocks), n, n, len(firsts)))
    assert len(written) == len(stock.placed)
    lines += ['', '#define MOST_MAP_BLOCKS %d' % most_blocks, '#define MOST_BLOCK_SIZE %uu' % most_block,
              '#define MOST_TAG_SIZE %uu' % most_tags, '#define MOST_MAP_LENGTH %uu' % most_length,
              '#define STOCK_BLOCKS %d' % len(stock.placed), '#define STOCK_LEVELS %d' % len(sp.LEVEL_NAMES), '',
              'static struct retail_map const retail_maps[STOCK_MAPS] = {'] + rows + ['};', '']
    open(out, 'w').write('\n'.join(lines))
    for l in lines:
        if l.startswith('#define'):
            print(l)


main()
