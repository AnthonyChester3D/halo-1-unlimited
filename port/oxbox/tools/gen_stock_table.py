#!/usr/bin/env python3
"""gen_stock_table.py RETAIL_MAPS_DIR OUT.h: the installer's table of the retail maps (installer/stockmaps_table.h): each file's
size and CRC, and unpacked, as shared_pass.py reads them; for each campaign level, which of its blocks go in stock.shared
(what three levels or more use: docs/shared_maps.md), so a console needn't count them over 2.4 GB of levels; and what
stock.shared comes out as. It holds no data of the maps' but those figures."""
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
    for n in sp.STOCK_NAMES + sp.LEVEL_NAMES:
        m = sp.Map(path(n))
        keys = m.keys()
        most_blocks = max(most_blocks, len(m.blocks))
        most_block = max([most_block] + [b[2] for b in m.blocks])
        most_tags = max(most_tags, m.tag_size)
        most_length = max(most_length, len(m.data))
        mask = 'NULL'
        if n in sp.LEVEL_NAMES:
            bits = bytearray((len(keys) + 7) // 8)
            for k, key in enumerate(keys):
                if key in stock.placed:
                    bits[k >> 3] |= 1 << (k & 7)
            mask = 'level_%s' % n
            lines.append('static unsigned char const %s[%d] = {' % (mask, len(bits)))
            for at in range(0, len(bits), 32):
                lines.append('\t' + ','.join(str(b) for b in bits[at:at + 32]) + ',')
            lines.append('};')
        rows.append('\t{"%s", %uu, 0x%08xu, %uu, 0x%08xu, %d, %s},' % (n, len(m.raw), zlib.crc32(m.raw) & 0xFFFFFFFF, len(m.data),
                     sp.u32(m.data, 0x64), len(m.blocks), mask))
    lines += ['', '#define MOST_MAP_BLOCKS %d' % most_blocks, '#define MOST_BLOCK_SIZE %uu' % most_block,
              '#define MOST_TAG_SIZE %uu' % most_tags, '#define MOST_MAP_LENGTH %uu' % most_length,
              '#define STOCK_BLOCKS %d' % len(stock.placed), '#define STOCK_LEVELS %d' % len(sp.LEVEL_NAMES), '',
              'static struct retail_map const retail_maps[STOCK_MAPS] = {'] + rows + ['};', '']
    open(out, 'w').write('\n'.join(lines))
    for l in lines:
        if l.startswith('#define'):
            print(l)


main()
