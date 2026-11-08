#!/usr/bin/env python3
"""verify_shared.py ORIGINAL_MAP CONVERTED_MAP PACK_SHARED STOCK_SHARED: a map converted to draw on shared files
(docs/shared_maps.md) checked against the map it came from: every texture, sound and BSP of the original, read through the
converted map's offsets from wherever it is now, against the original's bytes, and the tag data, the fields aside. Prints what
differs. PACK_SHARED may be a path that doesn't exist, for a map drawing on stock.shared alone. A wrong conversion can load and
play with no error: this is what finds it."""
import os, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_pass as sp
u32 = sp.u32
def raw_blocks(m):
    """every bitmap's and permutation's (field, offset, size), before the tool's filter"""
    d = m.data; out = []
    inst, count = u32(d, m.tag_offset), u32(d, m.tag_offset + 12)
    for k in range(count):
        e = m.at(inst) + k * 0x20
        g, base = u32(d, e), u32(d, e + 0x14)
        if not base: continue
        if g == sp.BITMAP:
            b = m.at(base)
            for i in range(u32(d, b + 0x60)):
                x = m.at(u32(d, b + 0x64)) + i * 0x30
                out.append(('bitm', k, x + 0x18, u32(d, x + 0x18), u32(d, x + 0x1C)))
        elif g == sp.SOUND:
            s = m.at(base)
            for p in range(u32(d, s + 0x98)):
                r = m.at(u32(d, s + 0x9C)) + p * 0x48
                for q in range(u32(d, r + 0x3C)):
                    x = m.at(u32(d, r + 0x40)) + q * 0x7C
                    out.append(('snd!', k, x + 0x48, u32(d, x + 0x48), u32(d, x + 0x40)))
    return out
def check(orig_path, conv_path, pack, stock):
    a = sp.Map(orig_path)
    raw, c = sp.read_map(conv_path)
    dt = u32(c, 0x10) - a.tag_offset
    bad = []; moved = stayed = 0; invalid = [0]
    fields = set()
    for kind, k, f, o, n in raw_blocks(a):
        # (an entry whose field isn't in the tag data, a broken tag's list running past its end: nothing to check)
        if not (a.tag_offset <= f and f + 8 <= a.tag_offset + a.tag_size):
            continue
        fields.add(f)
        v = u32(c, f + dt)
        # (an entry that was never valid in the original, nothing the game could read: left as it was)
        if not (n and sp.HEADER <= o and o + n <= len(a.data) and not o & 0x80000000 and
                not (a.tag_offset < o + n and o < a.tag_offset + a.tag_size)):
            invalid[0] += 1
            if v != o:
                bad.append('%s tag %d: an invalid entry (%08x, size %d) was changed to %08x' % (kind, k, o, n, v))
            continue
        if v & 0x80000000:
            src = stock if v & 0x40000000 else pack
            at = v & 0x3FFFFFFF
            got = src[at:at + n]; moved += 1
        else:
            got = bytes(c[v:v + n]); stayed += 1
        want = bytes(a.data[o:o + n])
        if got != want:
            bad.append('%s tag %d at %08x size %d -> %08x: DIFFERENT' % (kind, k, o, n, v))
    for field in a.bsp_fields:
        fields.add(field)
        o, n = u32(a.data, field), u32(a.data, field + 4)
        v = u32(c, field + dt)
        if bytes(c[v:v + n]) != bytes(a.data[o:o + n]):
            bad.append('BSP at %08x size %d -> %08x: DIFFERENT' % (o, n, v))
    # the tag data, the fields aside
    t0, t1 = bytearray(a.data[a.tag_offset:a.tag_offset + a.tag_size]), bytearray(c[u32(c, 0x10):u32(c, 0x10) + a.tag_size])
    for f in fields:
        for b in range(4):
            t0[f - a.tag_offset + b] = 0; t1[f - a.tag_offset + b] = 0
    if t0 != t1:
        diff = sum(1 for x, y in zip(t0, t1) if x != y)
        bad.append('tag data differs in %d bytes beyond the fields' % diff)
    return moved, stayed, bad
if __name__ == '__main__':
    pack = open(sys.argv[3], 'rb').read() if os.path.exists(sys.argv[3]) else b''
    if pack[0x800:0x801] == b'\x78': pack = pack[:0x800] + zlib.decompress(pack[0x800:])
    stock = open(sys.argv[4], 'rb').read()
    moved, stayed, bad = check(sys.argv[1], sys.argv[2], pack, stock)
    print(os.path.basename(sys.argv[1]), 'moved', moved, 'stayed', stayed, 'problems', len(bad))
    for b in bad[:12]: print('  ', b)
