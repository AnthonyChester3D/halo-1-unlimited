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


# ---- revision 2 (reference version 4): the recipe in sections, the own words in it, the tag area not stored

SECTION_STOCK, SECTION_PACK, SECTION_OWN = 1, 2, 3


def groups_bytes(groups):
    out = bytearray()
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


def own_runs(groups_list, words):
    """the maximal runs of the words no run of the groups writes: (dest, length), dest ascending"""
    covered = bytearray(words)
    for groups in groups_list:
        for frame, runs in groups:
            for d, o, n in runs:
                assert not any(covered[d:d + n]), 'a word written twice'
                covered[d:d + n] = b'\1' * n
    out, w = [], 0
    while w < words:
        if covered[w]:
            w = covered.find(0, w)
            if w < 0:
                break
            continue
        end = covered.find(1, w)
        end = words if end < 0 else end
        out.append((w, end - w))
        w = end
    return out


def encode_v4(stock, pack, own, tags):
    """the sectioned recipe: stock and pack groups, and the own runs ((dest, length)) with their words from tags"""
    own_bytes = bytearray()
    leb(own_bytes, len(own))
    end = 0
    for d, n in own:
        leb(own_bytes, d - end)
        leb(own_bytes, n)
        own_bytes += tags[d * 4:(d + n) * 4]
        end = d + n
    out = bytearray()
    leb(out, 3)
    for kind, body in ((SECTION_STOCK, groups_bytes(stock)), (SECTION_PACK, groups_bytes(pack)), (SECTION_OWN, own_bytes)):
        leb(out, kind)
        leb(out, len(body))
        out += body
    return bytes(out)


def decode_groups(b):
    i, groups, frame = 0, [], 0
    count, i = read_leb(b, i)
    for g in range(count):
        delta, i = read_leb(b, i)
        frame = delta if g == 0 else frame + delta
        n, i = read_leb(b, i)
        runs, end = [], 0
        for _ in range(n):
            d, i = read_leb(b, i)
            o, i = read_leb(b, i)
            ln, i = read_leb(b, i)
            runs.append((end + d, o, ln))
            end += d + ln
        groups.append((frame, runs))
    assert i == len(b)
    return groups


def decode_v4(recipe):
    """(stock groups, pack groups, own runs as (dest, the words' bytes))"""
    i = 0
    count, i = read_leb(recipe, i)
    sections, last = {}, 0
    for _ in range(count):
        kind, i = read_leb(recipe, i)
        size, i = read_leb(recipe, i)
        assert kind > last, 'sections out of order'
        last = kind
        sections[kind] = recipe[i:i + size]
        i += size
    assert i == len(recipe) and set(sections) >= {1, 2, 3} and all(k >= 64 or k <= 3 for k in sections)
    own, b, j, end = [], sections[SECTION_OWN], 0, 0
    n, j = read_leb(b, j)
    for _ in range(n):
        d, j = read_leb(b, j)
        ln, j = read_leb(b, j)
        own.append((end + d, b[j:j + ln * 4]))
        j += ln * 4
        end += d + ln
    assert j == len(b)
    return decode_groups(sections[SECTION_STOCK]), decode_groups(sections[SECTION_PACK]), own


def stock_small_v4(small, m, fields, recipe, pool_size, pool_crc):
    """revision 2's small stock map: its recipe's stock section the retail map's less the words its fields are in (as
    stock_small), no pack groups, its own words (the fields) in section 3; the file stops at its tag area (always its
    last part), the recipe at the next 2048-byte boundary, the map padded to one; reference version 4. Returns (the map,
    its recipe, the CRC32 of its tag data as rebuilt)"""
    tag_at, size = struct.unpack_from('<I', small, 0x10)[0], m.tag_size
    assert all(not any(small[tag_at + size:]) for _ in [0]), 'something after the tag area'
    words = sorted(set((f - m.tag_offset) // 4 for f in fields))
    assert all((f - m.tag_offset) % 4 == 0 and 0 <= f - m.tag_offset < size for f in fields)
    stock, pack = decode(recipe)
    assert not pack
    groups = without(stock, words)
    tags = bytes(small[tag_at:tag_at + size])
    own = own_runs([groups], size // 4)
    assert [w for d, n in own for w in range(d, d + n)] == words
    r4 = encode_v4(groups, [], own, tags)
    tag_crc = zlib.crc32(tags) & 0xFFFFFFFF
    recipe_at = (tag_at + 2047) & ~2047
    length = (recipe_at + len(r4) + 2047) & ~2047
    out = bytearray(small[:tag_at]) + bytes(recipe_at - tag_at) + r4 + bytes(length - recipe_at - len(r4))
    struct.pack_into('<I', out, 8, length)
    struct.pack_into('<I', out, 0x704, 4)
    struct.pack_into('<IIIIII', out, 0x768, recipe_at, len(r4), pool_crc, pool_size, tag_crc, 0)
    return out, r4, tag_crc
