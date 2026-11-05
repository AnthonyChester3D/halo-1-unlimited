#!/usr/bin/env python3
"""shared_pass.py: Xbox maps' texture and sound data moved into one shared file (docs/shared_maps.md), a reference
for Halo Map Porter's own pass, and the test maker for the game's side.

Usage: python3 port/oxbox/tools/shared_pass.py NAME OUT_DIR MAP [MAP...]

Writes OUT_DIR/NAME.shared and, for each MAP (a folder map's .map: maps\\<folder>\\<file>.map), OUT_DIR/<folder>/<file>
with the other files of its folder copied beside it. Each bitmap's pixels (bitmap_data's pixels_offset and pixels_size) and each sound permutation's samples go
into the shared file once (the same bytes in two maps, or twice in one, are stored once), at an offset with the same
remainder modulo 512 as before (the game reads whole sectors); the map's offset to it gets its top bit set. The map
keeps the bytes' place, zeroed (they compress to nothing: Halo Map Porter's own pass leaves them out). The map's header
gets the shared reference at 0x700, its identity in map sharing (size, checksum) the map's before the pass.

Put NAME.shared beside the maps' folders (in the folder above them: a pack's, or maps\\ itself) or in a map's folder.
"""
import hashlib
import os
import shutil
import struct
import sys
import zlib

HEADER = 0x800
TAG_BASE = 0x803A6000
SHARED_REFERENCE_OFFSET = 0x700
BITMAP = 0x6269746D        # 'bitm'
SOUND = 0x736E6421         # 'snd!'


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def read_map(path):
    raw = open(path, 'rb').read()
    header = raw[:HEADER]
    if raw[HEADER:HEADER + 1] == b'\x78':
        body = zlib.decompress(raw[HEADER:])
    else:
        body = raw[HEADER:]
    data = bytearray(header + body)
    if data[0:4] != b'daeh' or u32(data, 4) != 5:
        sys.exit('%s: not an Xbox cache file' % path)
    return raw, data


def raw_blobs(data, path):
    """(offset field position, file offset, size) of every bitmap's pixel data and sound permutation's samples"""
    tag_offset, tag_size = u32(data, 0x10), u32(data, 0x14)

    def at(address):
        o = tag_offset + address - TAG_BASE
        if not tag_offset <= o < tag_offset + tag_size:
            raise ValueError('%08x is outside the tag data' % address)
        return o

    instances, count = u32(data, tag_offset), u32(data, tag_offset + 12)
    blobs = []
    for k in range(count):
        inst = at(instances) + k * 0x20
        group, base = u32(data, inst), u32(data, inst + 0x14)
        if not base:
            continue
        if group == BITMAP:
            # (each bitmap's own pixels: its pixels_offset is the file's, the tag's pixel data unused on the Xbox)
            b = at(base)
            for m in range(u32(data, b + 0x60)):
                e = at(u32(data, b + 0x64)) + m * 0x30
                blobs.append((e + 0x18, u32(data, e + 0x18), u32(data, e + 0x1C)))
        elif group == SOUND:
            s = at(base)
            for p in range(u32(data, s + 0x98)):
                pr = at(u32(data, s + 0x9C)) + p * 0x48
                for q in range(u32(data, pr + 0x3C)):
                    pm = at(u32(data, pr + 0x40)) + q * 0x7C
                    blobs.append((pm + 0x48, u32(data, pm + 0x48), u32(data, pm + 0x40)))
    good = [(f, o, n) for f, o, n in blobs if n and HEADER <= o and o + n <= len(data) and not o & 0x80000000 and
            not (tag_offset < o + n and o < tag_offset + tag_size)]
    if len(good) != len(blobs):
        print('  %s: %d of its %d texture and sound entries kept in it (empty, or not data in the file)' %
              (os.path.basename(path), len(blobs) - len(good), len(blobs)))
    return good


def write_cache_file(path, data):
    out = bytes(data[:HEADER]) + zlib.compress(bytes(data[HEADER:]), int(os.environ.get('SHARED_PASS_LEVEL', '9')))
    # (whole 2048-byte sectors, as Xbox maps are: the game copies them in whole sectors, unbuffered)
    out += bytes(-len(out) % 2048)
    open(path, 'wb').write(out)
    return len(out)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    name, out_dir, maps = sys.argv[1], sys.argv[2], sys.argv[3:]
    if len(name) > 30 or any(c in name for c in '\\/:*?"<>|'):
        sys.exit('the shared file name must be 1 to 30 characters, none of \\/:*?"<>|')
    os.makedirs(out_dir, exist_ok=True)
    shared = bytearray(HEADER)
    placed = {}                 # (hash, offset % 512) -> offset in the shared file
    pieces = []
    for path in maps:
        raw, data = read_map(path)
        original = bytes(data)
        moved = 0
        for field, offset, size in raw_blobs(data, path):
            blob = original[offset:offset + size]
            key = (hashlib.sha1(blob).digest(), offset % 512)
            if key not in placed:
                pad = (offset % 512 - len(shared) % 512) % 512
                shared += bytes(pad)
                placed[key] = len(shared)
                shared += blob
            struct.pack_into('<I', data, field, 0x80000000 | placed[key])
            data[offset:offset + size] = bytes(size)
            moved += size
        pieces.append((path, raw, data, moved))
    shared += bytes(-len(shared) % 2048)
    checksum = zlib.crc32(bytes(shared[HEADER:])) & 0xFFFFFFFF
    header = bytearray(HEADER)
    struct.pack_into('<4sII', header, 0, b'daeh', 5, len(shared))
    header[0x20:0x20 + len(name)] = name.encode()
    header[0x40:0x46] = b'shared'
    struct.pack_into('<hhI', header, 0x60, 0, 0, checksum)
    header[0x7FC:0x800] = b'toof'
    shared[:HEADER] = header
    shared_size = write_cache_file(os.path.join(out_dir, name + '.shared'), shared)
    total_before = total_after = 0
    for path, raw, data, moved in pieces:
        folder = os.path.basename(os.path.dirname(os.path.abspath(path)))
        dest_dir = os.path.join(out_dir, folder)
        os.makedirs(dest_dir, exist_ok=True)
        reference = struct.pack('<Il32sIlII', 0x73687264, 1, name.encode(), checksum, len(shared), len(raw),
                                u32(raw, 0x64))
        data[SHARED_REFERENCE_OFFSET:SHARED_REFERENCE_OFFSET + len(reference)] = reference
        size = write_cache_file(os.path.join(dest_dir, os.path.basename(path)), data)
        for other in os.listdir(os.path.dirname(os.path.abspath(path))):
            if not other.lower().endswith('.map'):
                shutil.copy(os.path.join(os.path.dirname(os.path.abspath(path)), other), dest_dir)
        print('%s/%s: %d KB -> %d KB (%d KB of textures and sounds in the shared file)' % (
            folder, os.path.basename(path), len(raw) >> 10, size >> 10, moved >> 10))
        total_before += len(raw)
        total_after += size
    print('%s.shared: %d KB (%d KB unpacked)' % (name, shared_size >> 10, len(shared) >> 10))
    print('in all: %d KB -> %d KB' % (total_before >> 10, (total_after + shared_size) >> 10))


if __name__ == '__main__':
    main()
