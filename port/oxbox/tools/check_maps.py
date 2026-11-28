"""check_maps.py STOCKTAGS DIR [--csv OUT] [--verbose]: every Xbox map under DIR (a maps folder: packs' folders with
their <pack>-<checksum>.shared and map folders, and loose maps) checked for data that can't be what it says it is,
before anyone plays it (Llama, 2026-11-26: Aerial's h1_spillway drew scrambled textures, its own bitmaps' pixels
being other images and junk; junk sound data plays as loud noise).

- Sounds: every permutation's samples, wherever they live (the map, its pack's file; stock.shared's are Bungie's and
  aren't read). Xbox ADPCM (compression 1): 36-byte blocks a channel, each opening with a 16-bit sample, a step
  index 0-88 and a zero byte: real data passes on every block, junk fails within a few. 16-bit PCM (compression 0):
  RMS over 12000 with many samples at full scale is noise, not sound.
- Bitmaps: the map's and the pack's DXT bitmaps' top mip: neighbouring blocks' colours in a picture are close; in
  junk they jump (the score: mean distance between horizontally adjacent blocks' first colours, 0-1).

STOCKTAGS: stocktags.shared (raw or packed frames) for format 4 maps' tag data. A map is listed when a sound fails or
its bitmaps look like noise; --verbose lists every map. Exit status 1 when any map is listed."""
import collections, glob, os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tagrecipe as tr                      # noqa: E402
from shared_pass import TAG_BASE            # noqa: E402

NOISE_SCORE = 0.22          # a bitmap whose neighbouring blocks differ more than this looks like noise
NOISY_SHARE = 0.05          # a map is listed when this share of its own (map and pack) DXT bitmaps look like noise
                            # (Aerial's good maps: 0-2 of ~550; h1_spillway: 20 of 121)


def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def s16(b, o): return struct.unpack_from('<h', b, o)[0]


def unpack(path):
    d = open(path, 'rb').read()
    return bytearray(d[:0x800] + (zlib.decompress(d[0x800:]) if d[0x800:0x801] == b'\x78' else d[0x800:]))


def stock_pool(path):
    st = open(path, 'rb').read()
    size, count, table = u32(st, 0x14), u32(st, 0x18), u32(st, 0x1C)
    pool = bytearray()
    for k in range(count):
        o, n = struct.unpack_from('<II', st, table + 8 * k)
        piece = st[o:o + n]
        want = min(size, u32(st, 0x0C) - k * size)
        pool += piece if len(piece) == want else zlib.decompress(piece)
    return bytes(pool)


def tag_data(m, pool, pack):
    """the map's tag data: rebuilt from its recipe (format 4: stock frames, its pack's tag part, its own words; format 3:
    stock and pack runs), or as it is in the file"""
    to, ts = u32(m, 0x10), u32(m, 0x14)
    version = u32(m, 0x704) if len(m) > 0x780 and m[0x700:0x704] != b'\0\0\0\0' else 0
    ra, rs = u32(m, 0x768), u32(m, 0x76C)
    part = pack[u32(pack, 0x68):u32(pack, 0x68) + u32(pack, 0x6C)] if pack else b''
    if version == 4:
        stock, packg, own = tr.decode_v4(bytes(m[ra:ra + rs]))
        tags = bytearray(ts)
        tr.apply(stock, tags, pool)
        if packg: tr.apply(packg, tags, part)
        for d, words in own: tags[d * 4:d * 4 + len(words)] = words
        return tags
    tags = bytearray(m[to:to + ts])
    if version == 3 and rs:
        stock, packg = tr.decode(bytes(m[ra:ra + rs]))
        tr.apply(stock, tags, pool)
        if packg: tr.apply(packg, tags, part)
    return tags


def data_at(offset, size, m, pack):
    """a sound's or bitmap's bytes: the map's own, or its pack file's (None: stock.shared's, not read here)"""
    if offset & 0xC0000000 == 0xC0000000: return None
    if offset & 0x80000000:
        if not pack: return b''
        o = offset & 0x3FFFFFFF
        return bytes(pack[o:o + size])
    return bytes(m[offset:offset + size])


def adpcm_bad(data, channels):
    """the share of Xbox ADPCM block headers that can't be (step index over 88, or the reserved byte not 0)"""
    block = 36 * channels
    blocks = len(data) // block
    if not blocks: return 0.0
    bad = 0
    for b in range(blocks):
        for c in range(channels):
            h = b * block + c * 4
            if data[h + 2] > 88 or data[h + 3] != 0: bad += 1
    return bad / float(blocks * channels)


def pcm_loud(data):
    n = len(data) // 2
    if n < 256: return False
    samples = struct.unpack_from('<%dh' % n, data, 0)
    rms = (sum(s * s for s in samples) / float(n)) ** 0.5
    full = sum(1 for s in samples if s > 32000 or s < -32000)
    return rms > 12000 and full > n // 50


def rgb565(v): return ((v >> 11) & 31) / 31.0, ((v >> 5) & 63) / 63.0, (v & 31) / 31.0


def dxt_noise(data, w, h, fmt):
    """mean distance (0-1) between horizontally adjacent DXT blocks' first colours"""
    bs = 8 if fmt == 14 else 16
    bw, bh = max(1, w // 4), max(1, h // 4)
    total = count = 0.0
    for by in range(0, bh, max(1, bh // 32)):
        row = by * bw * bs
        prev = None
        for bx in range(0, min(bw, 64)):
            i = row + bx * bs + bs - 8
            if i + 2 > len(data): break
            c = rgb565(struct.unpack_from('<H', data, i)[0])
            if prev is not None:
                total += (abs(c[0] - prev[0]) + abs(c[1] - prev[1]) + abs(c[2] - prev[2])) / 3.0
                count += 1
            prev = c
    return total / count if count else 0.0


_packs = {}


def check_map(path, pool):
    m = unpack(path)
    pack = None
    if len(m) > 0x780 and m[0x700:0x704] != b'\0\0\0\0':
        name = bytes(m[0x708:0x728]).split(b'\0')[0].decode('latin1')
        if name and name != 'stock':
            for folder in (os.path.dirname(path), os.path.dirname(os.path.dirname(path))):
                p = os.path.join(folder, name + '.shared')
                if os.path.exists(p):
                    if p not in _packs:
                        _packs.clear()          # (one pack at a time: they come in folder order)
                        _packs[p] = unpack(p)
                    pack = _packs[p]
                    break
    tags = tag_data(m, pool, pack)
    at = lambda a: a - TAG_BASE
    inst, count = u32(tags, 0), u32(tags, 12)
    result = {'sounds': 0, 'bad_sounds': [], 'stock_sounds': 0, 'dxt': 0, 'noisy': [], 'missing_pack': False}
    for k in range(count):
        e = at(inst) + k * 0x20
        group = bytes(tags[e:e + 4][::-1])
        if group not in (b'snd!', b'bitm'): continue
        name_at = at(u32(tags, e + 0x10))
        name = bytes(tags[name_at:tags.index(0, name_at)]).decode('latin1')
        g = at(u32(tags, e + 0x14))
        if group == b'snd!':
            channels = 2 if s16(tags, g + 108) == 1 else 1
            for r in range(u32(tags, g + 152)):
                pr = at(u32(tags, g + 156)) + r * 72
                for p in range(u32(tags, pr + 60)):
                    pe = at(u32(tags, pr + 64)) + p * 124
                    comp, size, offset = s16(tags, pe + 40), u32(tags, pe + 64), u32(tags, pe + 72)
                    if not size: continue
                    data = data_at(offset, size, m, pack)
                    if data is None:
                        result['stock_sounds'] += 1
                        continue
                    if not data:
                        result['missing_pack'] = True
                        continue
                    result['sounds'] += 1
                    why = None
                    if comp == 1:
                        bad = adpcm_bad(data, channels)
                        if bad > 0.01: why = '%.0f%% of its ADPCM blocks impossible' % (100 * bad)
                    elif comp == 0 and pcm_loud(data):
                        why = 'full-scale noise (PCM)'
                    if why: result['bad_sounds'].append('%s [%d.%d]: %s' % (name, r, p, why))
        else:
            for j in range(u32(tags, g + 96)):
                b = at(u32(tags, g + 100)) + j * 48
                w, h, typ, fmt, offset, size = (s16(tags, b + 4), s16(tags, b + 6), s16(tags, b + 10), s16(tags, b + 12),
                                                u32(tags, b + 24), u32(tags, b + 28))
                # (small ones (masks, decals, illumination) are speckled by design: Corverie's Longest's 32x32s)
                if fmt not in (14, 15, 16) or typ != 0 or w < 64 or h < 64: continue
                data = data_at(offset, size, m, pack)
                if not data: continue
                result['dxt'] += 1
                score = dxt_noise(data, w, h, fmt)
                if score > NOISE_SCORE: result['noisy'].append('%s %dx%d (%.2f)' % (name.split('\\')[-1], w, h, score))
    return result


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    csv_path = sys.argv[sys.argv.index('--csv') + 1] if '--csv' in sys.argv else None
    if csv_path in args: args.remove(csv_path)
    verbose = '--verbose' in sys.argv
    pool = stock_pool(args[0])
    maps = sorted(p for p in glob.glob(os.path.join(args[1], '**', '*.map'), recursive=True) if os.path.isfile(p))
    out = open(csv_path, 'w') if csv_path else None
    if out: out.write('map,sounds,bad_sounds,dxt_bitmaps,noisy_bitmaps,missing_pack,first_problem\n')
    listed = 0
    for path in maps:
        rel = os.path.relpath(path, args[1])
        try:
            r = check_map(path, pool)
        except Exception as e:      # (a map the tools can't read: say so, go on)
            print('%s: could not be read (%s)' % (rel, e))
            continue
        noisy = len(r['noisy']) > max(1, NOISY_SHARE * r['dxt'])
        problem = bool(r['bad_sounds']) or noisy or r['missing_pack']
        if problem: listed += 1
        if problem or verbose:
            print('%s: %d sounds checked, %d bad; %d DXT bitmaps, %d look like noise%s' % (
                rel, r['sounds'], len(r['bad_sounds']), r['dxt'], len(r['noisy']),
                '; ITS PACK FILE ISN\'T HERE' if r['missing_pack'] else ''))
            for s in r['bad_sounds'][:5]: print('    sound %s' % s)
            if noisy:
                for s in r['noisy'][:5]: print('    bitmap %s' % s)
        if out:
            first = (r['bad_sounds'] or (r['noisy'] if noisy else []) or [''])[0]
            out.write('"%s",%d,%d,%d,%d,%d,"%s"\n' % (rel, r['sounds'], len(r['bad_sounds']), r['dxt'], len(r['noisy']),
                                                     int(r['missing_pack']), first.replace('"', "'")))
    print('%d maps checked, %d listed' % (len(maps), listed))
    sys.exit(1 if listed else 0)


if __name__ == '__main__':
    main()
