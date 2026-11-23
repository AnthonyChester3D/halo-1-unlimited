"""tagrecipe.py: format 4's recipes (docs/shared_maps.md, "Shared tag data (format 4)"), in Python: read, written, and the
stock maps' (gen_stock_table.py): the stock tag pool made from the retail maps' tag data and their recipes, as the installer
makes it, and each small stock map's recipe and tag data.

A recipe is groups of runs; a group is (frame, [(dest, offset, length), ...]), lengths and places in words."""
import bisect
import struct
import zlib

FRAME = 16384
FRAME_WORDS = FRAME // 4


def leb(out, v):
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | 0x80 if v else b)
        if not v:
            return


def read_leb(b, i):
    v, shift = 0, 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7F) << shift
        shift += 7
        if not c & 0x80:
            return v, i


def decode(recipe):
    """(stock groups, pack groups)"""
    i, out = 0, []
    for _ in range(2):
        count, i = read_leb(recipe, i)
        groups, frame = [], 0
        for g in range(count):
            delta, i = read_leb(recipe, i)
            frame = delta if g == 0 else frame + delta
            runs_count, i = read_leb(recipe, i)
            runs, end = [], 0
            for _ in range(runs_count):
                d, i = read_leb(recipe, i)
                o, i = read_leb(recipe, i)
                n, i = read_leb(recipe, i)
                runs.append((end + d, o, n))
                end += d + n
            groups.append((frame, runs))
        out.append(groups)
    assert i == len(recipe)
    return out[0], out[1]


def encode(stock, pack=()):
    out = bytearray()
    for groups in (stock, pack):
        leb(out, len(groups))
        last = 0
        for g, (frame, runs) in enumerate(groups):
            leb(out, frame - last if g else frame)
            leb(out, len(runs))
            end = 0
            for d, o, n in runs:
                leb(out, d - end)
                leb(out, o)
                leb(out, n)
                end = d + n
            last = frame
    return bytes(out)


def without(groups, words):
    """the groups with every word in words (sorted, dest places) left out: a run they're in is cut there, a group left with
    no run goes"""
    out = []
    for frame, runs in groups:
        pieces = []
        for d, o, n in runs:
            i = bisect.bisect_left(words, d)
            while n:
                cut = words[i] if i < len(words) and words[i] < d + n else d + n
                if cut > d:
                    pieces.append((d, o, cut - d))
                step = cut - d + (1 if cut < d + n else 0)
                d, o, n = d + step, o + step, n - step
                i += 1
        if pieces:
            out.append((frame, pieces))
    return out


def apply(groups, tags, source):
    """the runs copied into tags (bytearray) from source (the pool's bytes, or a pack's tag part)"""
    for frame, runs in groups:
        base = frame * FRAME_WORDS
        for d, o, n in runs:
            tags[d * 4:(d + n) * 4] = source[(base + o) * 4:(base + o + n) * 4]


def pool_add(pool, groups, tags):
    """the pool (bytearray) grown by one stock map's new words, as the installer grows it: its runs that copy from past the
    pool's end, as it is now, are those words (any run that covers a word has it: they're the same words). Returns the
    pool's size after it, in words"""
    have = len(pool) // 4
    end = max(frame * FRAME_WORDS + o + n for frame, runs in groups for d, o, n in runs)
    pool += bytes((end - have) * 4 if end > have else 0)
    for frame, runs in groups:
        base = frame * FRAME_WORDS
        for d, o, n in runs:
            if base + o + n > have:
                skip = max(have - base - o, 0)
                pool[(base + o + skip) * 4:(base + o + n) * 4] = tags[(d + skip) * 4:(d + n) * 4]
    return len(pool) // 4


def stock_recipes(path):
    """stock_recipes.bin (tagpool.c): its pool size and CRC32, and each map's (name, tag size, rebuilt CRC32, recipe)"""
    b = open(path, 'rb').read()
    assert b[:4] == b'OXSR' and struct.unpack_from('<I', b, 4)[0] == 1
    count, pool_size, pool_crc = struct.unpack_from('<III', b, 8)
    maps = {}
    for k in range(count):
        e = 0x14 + k * 32
        name = b[e:e + 16].split(b'\0')[0].decode()
        tag_size, tag_crc, at, size = struct.unpack_from('<IIII', b, e + 16)
        maps[name] = (tag_size, tag_crc, b[at:at + size])
    return pool_size, pool_crc, maps


def stock_small(small, m, fields, recipe, pool_size, pool_crc):
    """a small stock map (shared_pass.rebuild's, unpadded) with its recipe: every word of its tag data the pool's, but those
    its fields are in (each block's and BSP's place: it holds them), so its recipe is the retail map's with those words left
    out; its tag data zeros but for them; the recipe after its data, at a 2048-byte boundary, the map then padded to one;
    its reference version 3. Returns (the map, its recipe, the CRC32 of its tag data as rebuilt)"""
    tag_at, size = struct.unpack_from('<I', small, 0x10)[0], m.tag_size
    words = sorted(set((f - m.tag_offset) // 4 for f in fields))
    assert all((f - m.tag_offset) % 4 == 0 and 0 <= f - m.tag_offset < size for f in fields)
    stock, pack = decode(recipe)
    assert not pack
    own = encode(without(stock, words))
    tag_crc = zlib.crc32(bytes(small[tag_at:tag_at + size])) & 0xFFFFFFFF
    out = bytearray(small)
    tags = bytearray(size)
    for w in words:
        tags[w * 4:w * 4 + 4] = small[tag_at + w * 4:tag_at + w * 4 + 4]
    out[tag_at:tag_at + size] = tags
    recipe_at = (len(out) + 2047) & ~2047
    length = (recipe_at + len(own) + 2047) & ~2047
    out += bytes(recipe_at - len(out)) + own
    out += bytes(length - len(out))
    struct.pack_into('<I', out, 8, length)
    struct.pack_into('<I', out, 0x704, 3)
    struct.pack_into('<IIIII', out, 0x768, recipe_at, len(own), pool_crc, pool_size, tag_crc)
    return out, own, tag_crc
