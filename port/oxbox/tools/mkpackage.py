"""mkpackage.py: an OpenCE package for the installer (port/oxbox/installer): how to make a build of OpenCE from the
player's own retail Halo XBE, with none of Microsoft's code or Halo's art in it.

Usage: python3 port/oxbox/tools/mkpackage.py out.oxpk "name" RETAIL.xbe BUILD.xbe [RETAIL2.xbe BUILD2.xbe ...]
           [--release N] [--network N] [--notes "text"]

(--release: the release's number, which goes up by one each release (0, the default: a test); --network: the
network version its online play speaks; --notes: a line about it.) The package isn't signed: the project's owner signs
it on their PC with port/oxbox/tools/oxsign.py, which appends the signature.

Each BUILD is port/oxbox/build.sh's halo1unlimited.xbe made from the RETAIL XBE beside it (a retail Halo, the
disc's: NTSC version 1, 8 or 9). The package says, for each, how to make the build from the retail XBE: runs of its
bytes taken from the retail XBE (wherever they are in it), bytes of OpenCE's own (its code, in a pool the builds
share), runs of zeros, and the pictures (pictures.h), made on the Xbox from the player's ui.map. Before any of
OpenCE's own bytes go into the pool, any 16 bytes or more found in the retail XBE are taken from it instead, so the
pool holds only what the build added. (A copy of a disc's XBE patched for media or regions is matched by the
installer as the disc's: package.c, package_match.)

The format (little-endian), read by port/oxbox/installer/package.c:
    header, 128 bytes:
        0   "OXPK"
        4   u32 format (2; 1 had no manifest and no signatures, and zeros at 112)
        8   u32 body's size (zlib), 12 u32 its size unpacked
        16  SHA-256 of the body (as stored)
        48  the package's name (64 bytes, ASCII, NUL-padded)
        112 u32 the manifest's size, u32 release, u32 network version, u32 (zero)
    the manifest: "key: value" lines (ASCII): name, release, network, date, notes, makes (the retail XBEs)
    the body (zlib)
    then, if it's signed: "OXSG", u32 count, and for each signature: the key's id (the first 8 bytes of the SHA-256 of
        its public key), and an Ed25519 signature of "Halo 1 Unlimited package, signed\n" + the header + the manifest
        (the header holds the body's SHA-256, so the signature covers it all). oxsign.py appends them.
    body (zlib), unpacked:
        u32 number of XBEs it makes
        for each, 112 bytes:
            SHA-256 of the retail XBE, u32 its size
            u32 the build's size, SHA-256 of the build (its pictures as zeros)
            u32 offset of its steps in the body, u32 their size
            its name (32 bytes, ASCII, NUL-padded)
        the steps, then the pool
    steps: a byte, then u32s:
        1 COPY from the retail XBE: offset, size
        2 POOL: offset in the body, size
        3 ZERO: size
        4 PICTURES: size (pictures.h's pictures: 147456)
        5 HEADER: size (pictures.h's ONLINE GAMES header: 32768)
        0 the end
"""
import argparse, bisect, datetime, hashlib, re, struct, sys, zlib

FORMAT = 2
MIN_MATCH = 16
MIN_ZEROS = 64
PICTURES_SIZE = 9 * 16384
HEADER_SIZE = 32768
COPY, POOL, ZERO, PICTURES, HEADER, END = 1, 2, 3, 4, 5, 0

def match_length(a, i, b, j, limit):
    """how many bytes a[i:] and b[j:] have alike (up to limit)"""
    n = 0
    step = 4096
    while n < limit:
        k = min(step, limit - n)
        if a[i + n:i + n + k] == b[j + n:j + n + k]:
            n += k
            continue
        if k <= 16:
            while n < limit and a[i + n] == b[j + n]:
                n += 1
            return n
        step = max(16, k // 8)
    return n


class Index:
    """where each 16 bytes are in a buffer (every 4th offset)"""
    def __init__(self, data):
        self.data = data
        self.at = {}
        self.done = 0
        self.add()

    def add(self):
        """(after the buffer grew)"""
        for j in range(self.done, len(self.data) - MIN_MATCH + 1, 4):
            self.at.setdefault(bytes(self.data[j:j + MIN_MATCH]), []).append(j)
            self.done = j + 4

    def best(self, out, i, prefer):
        """the longest run at out[i:] found here: (offset, length), or None"""
        found = self.at.get(bytes(out[i:i + MIN_MATCH]))
        if not found:
            return None
        best = None
        for j in ([prefer] if prefer in found else []) + found[:16]:
            n = match_length(out, i, self.data, j, min(len(out) - i, len(self.data) - j))
            if best is None or n > best[1]:
                best = (j, n)
        return best


def generated_regions(build):
    """the build's pictures (OXWORDS1) and ONLINE GAMES header (OXONLIN1, online builds), from their markers"""
    regions = []
    for magic, kind, size in ((b'OXWORDS1', PICTURES, PICTURES_SIZE), (b'OXONLIN1', HEADER, HEADER_SIZE)):
        mark = build.find(magic)
        if mark < 0:
            assert kind == HEADER, 'no pictures marker in the build'
            continue
        assert build.find(magic, mark + 1) < 0
        offset, length = struct.unpack_from('<II', build, mark + 8)
        assert length == size and offset + length <= len(build), (magic, offset, length)
        regions.append((offset, length, kind))
    return sorted(regions)


def steps_for(retail, build, pool, pool_index, retail_index):
    """the steps that make build from retail, adding to the pool what isn't in either"""
    steps = []
    literal = bytearray()
    regions = generated_regions(build)

    def flush():
        if literal:
            # (the pool may already have it: the other retail version's build)
            k = 0
            while k < len(literal):
                if k + MIN_MATCH <= len(literal):
                    m = pool_index.best(literal, k, -1)
                    if m and m[1] >= MIN_MATCH:
                        steps.append((POOL, m[0], m[1]))
                        k += m[1]
                        continue
                # a run new to the pool: to the next place the pool has
                start = k
                k += 1
                while k < len(literal):
                    if k + MIN_MATCH <= len(literal):
                        m = pool_index.best(literal, k, -1)
                        if m and m[1] >= MIN_MATCH:
                            break
                    k += 1
                at = len(pool)
                pool.extend(literal[start:k])
                pool_index.add()
                steps.append((POOL, at, k - start))
            literal.clear()

    # (the runs of zeros long enough to be steps of their own)
    zeros = [(m.start(), m.end()) for m in re.finditer(b'\x00{%d,}' % MIN_ZEROS, build)]
    zero_starts = [z[0] for z in zeros]

    i = 0
    region = 0
    while i < len(build):
        if region < len(regions) and i == regions[region][0]:
            flush()
            steps.append((regions[region][2], regions[region][1]))
            i += regions[region][1]
            region += 1
            continue
        end = regions[region][0] if region < len(regions) else len(build)
        # zeros
        if build[i] == 0:
            z = bisect.bisect_right(zero_starts, i) - 1
            n = min(zeros[z][1], end) - i if z >= 0 and zeros[z][1] > i else 0
            if n >= MIN_ZEROS:
                flush()
                steps.append((ZERO, n))
                i += n
                continue
        if i + MIN_MATCH <= end:
            m = retail_index.best(build, i, i)
            if m and m[1] >= MIN_MATCH:
                j, n = m
                n = min(n, end - i)
                # (back into the bytes not yet placed, if they match too)
                back = 0
                while back < len(literal) and j - back > 0 and retail[j - back - 1] == literal[-1 - back]:
                    back += 1
                if back:
                    del literal[len(literal) - back:]
                flush()
                steps.append((COPY, j - back, n + back))
                i += n
                continue
        literal.append(build[i])
        i += 1
    flush()
    # (runs that follow on from each other joined)
    joined = []
    for s in steps:
        if joined and s[0] in (COPY, POOL) and joined[-1][0] == s[0] and joined[-1][1] + joined[-1][2] == s[1]:
            joined[-1] = (s[0], joined[-1][1], joined[-1][2] + s[2])
        elif joined and s[0] == ZERO and joined[-1][0] == ZERO:
            joined[-1] = (ZERO, joined[-1][1] + s[1])
        else:
            joined.append(s)
    return joined


def encode_steps(steps, pool_at):
    out = bytearray()
    for s in steps:
        if s[0] == COPY:
            out += struct.pack('<BII', COPY, s[1], s[2])
        elif s[0] == POOL:
            out += struct.pack('<BII', POOL, pool_at + s[1], s[2])
        else:
            out += struct.pack('<BI', s[0], s[1])
    out += bytes([END])
    return bytes(out)


def apply(body, entry, retail):
    """(as the installer does) the build, its pictures as zeros"""
    steps_at, steps_size = entry
    out = bytearray()
    k = steps_at
    while True:
        op = body[k]
        k += 1
        if op == END:
            break
        if op in (COPY, POOL):
            offset, size = struct.unpack_from('<II', body, k)
            k += 8
            out += (retail if op == COPY else body)[offset:offset + size]
        else:
            size, = struct.unpack_from('<I', body, k)
            k += 4
            out += bytes(size)
    assert k == steps_at + steps_size
    return bytes(out)


def blank(build):
    out = bytearray(build)
    for offset, length, kind in generated_regions(build):
        out[offset:offset + length] = bytes(length)
    return bytes(out)


def retail_name(retail):
    """'Halo (NTSC, version 8)': the certificate's version"""
    base = struct.unpack_from('<I', retail, 0x104)[0]
    cert = struct.unpack_from('<I', retail, 0x118)[0] - base
    title, = struct.unpack_from('<I', retail, cert + 8)
    assert title == 0x4D530004, 'not Halo: title %08x' % title
    regions, = struct.unpack_from('<I', retail, cert + 0xA0)
    version, = struct.unpack_from('<I', retail, cert + 0xAC)
    region = 'NTSC' if regions & 1 else 'PAL' if regions & 4 else 'region %x' % regions
    return 'Halo (%s, version %d)' % (region, version)


def main():
    parser = argparse.ArgumentParser(usage=__doc__)
    parser.add_argument('out')
    parser.add_argument('name')
    parser.add_argument('xbes', nargs='+')
    parser.add_argument('--release', type=int, default=0)
    parser.add_argument('--network', type=int, default=0)
    parser.add_argument('--notes', default='')
    args = parser.parse_args()
    if len(args.xbes) % 2:
        sys.exit(__doc__)
    out_path, name = args.out, args.name
    for text in (name, args.notes):
        if not all(32 <= ord(c) < 127 for c in text):
            sys.exit('mkpackage: the name and notes are plain ASCII')
    pairs = [(open(args.xbes[k], 'rb').read(), open(args.xbes[k + 1], 'rb').read())
             for k in range(0, len(args.xbes), 2)]
    pool = bytearray()
    pool_index = Index(pool)
    made = []
    for retail, build in pairs:
        steps = steps_for(retail, build, pool, pool_index, Index(retail))
        made.append((retail, build, steps))
    table_size = 4 + 112 * len(made)
    encoded = [encode_steps(s, 0) for _, _, s in made]
    pool_at = table_size + sum(len(e) for e in encoded)
    encoded = [encode_steps(s, pool_at) for _, _, s in made]
    body = bytearray(struct.pack('<I', len(made)))
    at = table_size
    for (retail, build, steps), e in zip(made, encoded):
        body += hashlib.sha256(retail).digest() + struct.pack('<II', len(retail), len(build))
        body += hashlib.sha256(blank(build)).digest() + struct.pack('<II', at, len(e))
        body += retail_name(retail).encode('ascii').ljust(32, b'\0')[:32]
        at += len(e)
    for e in encoded:
        body += e
    assert len(body) == pool_at
    body += pool
    body = bytes(body)
    # (checked: each build made from its retail XBE, as the installer will)
    at = table_size
    for (retail, build, steps), e in zip(made, encoded):
        assert apply(body, (at, len(e)), retail) == blank(build), 'a build not made back'
        at += len(e)
    packed = zlib.compress(body, 9)
    manifest = 'name: %s\nrelease: %d\nnetwork: %d\ndate: %s\nnotes: %s\nmakes: %s\n' % (
        name, args.release, args.network, datetime.date.today().isoformat(), args.notes,
        ', '.join(retail_name(r) for r, _, _ in made))
    manifest = manifest.encode('ascii')
    header = b'OXPK' + struct.pack('<III', FORMAT, len(packed), len(body)) + hashlib.sha256(packed).digest()
    header += name.encode('ascii').ljust(64, b'\0')[:64]
    header += struct.pack('<IIII', len(manifest), args.release, args.network, 0)
    assert len(header) == 128
    open(out_path, 'wb').write(header + manifest + packed)
    for (retail, build, steps), e in zip(made, encoded):
        counts = {}
        for s in steps:
            size = s[2] if s[0] in (COPY, POOL) else s[1]
            c = counts.setdefault(s[0], [0, 0])
            c[0] += 1
            c[1] += size
        names = {COPY: 'from the retail XBE', POOL: "OpenCE's own", ZERO: 'zeros', PICTURES: 'pictures',
                 HEADER: 'header'}
        print('%s -> %d bytes: %s' % (retail_name(retail), len(build), ', '.join(
            '%s %d bytes (%d runs)' % (names[k], v[1], v[0]) for k, v in sorted(counts.items()))))
    print('%s: %d bytes (body %d, its pool %d), release %d, not signed' % (
        out_path, 128 + len(manifest) + len(packed), len(body), len(pool), args.release))


if __name__ == '__main__':
    main()
