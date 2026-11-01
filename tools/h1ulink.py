#!/usr/bin/env python3
# tools/h1ulink.py: the reference writer of a link file (.h1ulink) between a Custom Edition map and its Xbox port, the
# file Halo 1 Unlimited reads to play the two together (a dedicated server and its PCs on the Custom Edition map, its
# Xboxes on the port: docs/linked_maps.md).
#
# It writes what a link needs to find and translate: both files' hashes (map sharing's: port/linux/game/
# map_share_files.c), both scenarios' paths, every tag of both maps with its tag ID in each, and the placed objects'
# counts. It checks NO gameplay: that's Halo Map Porter's pair checker's job (its "check" lines). A link this writes
# says "check reference-only" and is for testing.
#
#   python3 -I tools/h1ulink.py CE_MAP XBOX_MAP [-o OUT] [--xbox-maps DIR]
#
# CE_MAP: a Custom Edition cache file (version 609); XBOX_MAP: an Xbox one (version 5, compressed or not), normally a
# folder map (maps\<folder>\<file>.map); --xbox-maps names the Xbox maps folder it's under, so the link names it as
# the Xbox does (default: the .map's parent's parent). OUT defaults to the Xbox map's folder, <its name>.h1ulink.
import argparse
import hashlib
import os
import struct
import sys
import zlib

PIECE_BYTES = 0x40000
HASH_LABEL = b'OpenCE map 1'
HEADER_SIZE = 0x800
# (the scenario's placement blocks: offsets in scnr, a tag block's count first)
PLACEMENTS = (('scenery', 0x210), ('biped', 0x228), ('vehicle', 0x240), ('equipment', 0x258), ('weapon', 0x270),
              ('machine', 0x294), ('control', 0x2AC), ('light_fixture', 0x2C4), ('sound_scenery', 0x2DC))
PALETTES = (('scenery', 0x21C), ('biped', 0x234), ('vehicle', 0x24C), ('equipment', 0x264), ('weapon', 0x27C),
            ('machine', 0x2A0), ('control', 0x2B8), ('light_fixture', 0x2D0), ('sound_scenery', 0x2E8))


def file_hash(path):
    """map sharing's hash: BLAKE2b-256 of the label, the size (4 bytes, little-endian) and each 256 KiB piece's
    BLAKE2b-256"""
    size = os.path.getsize(path)
    pieces = []
    with open(path, 'rb') as f:
        while True:
            piece = f.read(PIECE_BYTES)
            if not piece:
                break
            pieces.append(hashlib.blake2b(piece, digest_size=32).digest())
    data = HASH_LABEL + struct.pack('<I', size & 0xFFFFFFFF) + b''.join(pieces)
    return size, hashlib.blake2b(data, digest_size=32).hexdigest()


class CacheFile:
    def __init__(self, path):
        with open(path, 'rb') as f:
            raw = f.read()
        if len(raw) < HEADER_SIZE or raw[0:4] not in (b'head', b'daeh'):
            raise SystemExit('%s: not a cache file' % path)
        self.version, self.file_length = struct.unpack_from('<II', raw, 4)
        self.tag_data_offset, self.tag_data_size = struct.unpack_from('<II', raw, 0x10)
        self.name = raw[0x20:0x40].split(b'\0')[0].decode('latin-1')
        self.build = raw[0x40:0x60].split(b'\0')[0].decode('latin-1')
        self.scenario_type = struct.unpack_from('<H', raw, 0x60)[0]
        if self.version == 5 and len(raw) < self.file_length:
            # (an Xbox map: everything past the header compressed)
            raw = raw[:HEADER_SIZE] + zlib.decompress(raw[HEADER_SIZE:])
        elif self.version not in (5, 609, 7):
            raise SystemExit('%s: a cache file of version %d (Xbox 5, Custom Edition 609)' % (path, self.version))
        self.raw = raw
        self.tags = []
        self.read_tags()

    def read_tags(self):
        raw, base_offset = self.raw, self.tag_data_offset
        tag_array_address, self.scenario_id = struct.unpack_from('<II', raw, base_offset)
        tag_count = struct.unpack_from('<I', raw, base_offset + 0x0C)[0]
        if not 0 < tag_count < 65536:
            raise SystemExit('the tag count (%d) is not a map\'s' % tag_count)
        # (the address the tag data is loaded at: the Xbox's 0x803A6000, Custom Edition's 0x40440000; the tag array is
        # where its address says)
        base = 0x803A6000 if self.version == 5 else 0x40440000
        array = base_offset + (tag_array_address - base)
        if not base_offset <= array < base_offset + self.tag_data_size:
            raise SystemExit('the tag array was not found (its address %08x)' % tag_array_address)
        self.base = base
        for index in range(tag_count):
            entry = array + index * 32
            c0, c1, c2, tag_id, name_address, data_address = struct.unpack_from('<6I', raw, entry)
            name_offset = base_offset + (name_address - base)
            end = raw.index(b'\0', name_offset)
            name = raw[name_offset:end].decode('latin-1')
            group = struct.pack('>I', c0).decode('latin-1')
            self.tags.append((group, name, tag_id, data_address))

    def scenario(self):
        for group, name, tag_id, data_address in self.tags:
            if tag_id == self.scenario_id:
                return name, self.tag_data_offset + (data_address - self.base)
        raise SystemExit('the scenario tag was not found')

    def block_count(self, scenario_offset, field):
        return struct.unpack_from('<I', self.raw, scenario_offset + field)[0]


def main():
    parser = argparse.ArgumentParser(description='the reference writer of a .h1ulink (no gameplay checks)')
    parser.add_argument('ce_map')
    parser.add_argument('xbox_map')
    parser.add_argument('-o', '--out')
    parser.add_argument('--xbox-maps', help='the Xbox maps folder the Xbox map is under (default: its parent\'s parent)')
    arguments = parser.parse_args()

    ce = CacheFile(arguments.ce_map)
    xbox = CacheFile(arguments.xbox_map)
    if ce.version != 609:
        raise SystemExit('%s is not a Custom Edition map (version %d)' % (arguments.ce_map, ce.version))
    if xbox.version != 5:
        raise SystemExit('%s is not an Xbox map (version %d)' % (arguments.xbox_map, xbox.version))
    xbox_maps = arguments.xbox_maps or os.path.dirname(os.path.dirname(os.path.abspath(arguments.xbox_map)))
    xbox_path = os.path.relpath(os.path.abspath(arguments.xbox_map), os.path.abspath(xbox_maps)).replace('/', '\\')
    ce_name = os.path.splitext(os.path.basename(arguments.ce_map))[0].lower()
    ce_size, ce_hash = file_hash(arguments.ce_map)
    xbox_size, xbox_hash = file_hash(arguments.xbox_map)
    ce_level, ce_scenario = ce.scenario()
    xbox_level, xbox_scenario = xbox.scenario()

    lines = ['h1u-link 1',
             'tool h1ulink.py reference (identity and tag table only: no gameplay checks)',
             '# the two files: the name each game knows the map by, its size in bytes and its hash',
             'ce   custom_maps\\%s   %d   %s' % (ce_name, ce_size, ce_hash),
             'xbox %s   %d   %s' % (xbox_path, xbox_size, xbox_hash),
             '# the scenario tag\'s path in each',
             'level ce   %s' % ce_level,
             'level xbox %s' % xbox_level,
             '# placed objects, per palette type: the count (the same in both, or no link)']
    placements_same = True
    for kind, field in PLACEMENTS:
        a, b = ce.block_count(ce_scenario, field), xbox.block_count(xbox_scenario, field)
        lines.append('placed %s %d' % (kind, a) + ('' if a == b else '   # Xbox map: %d' % b))
        placements_same &= a == b
    palettes_same = all(ce.block_count(ce_scenario, field) == xbox.block_count(xbox_scenario, field)
                        for kind, field in PALETTES)
    lines.append('# every tag in either map: group, full tag ID in the CE map, in the Xbox map, path (- where missing)')
    xbox_tags = {(group, name.lower()): tag_id for group, name, tag_id, address in xbox.tags}
    ce_keys = set()
    for group, name, tag_id, address in ce.tags:
        key = (group, name.lower())
        ce_keys.add(key)
        other = xbox_tags.get(key)
        lines.append('tag %s %08x %s %s' % (group, tag_id, '%08x' % other if other is not None else '-', name))
    for group, name, tag_id, address in xbox.tags:
        if (group, name.lower()) not in ce_keys:
            lines.append('tag %s - %08x %s' % (group, tag_id, name))
    lines.append('check reference-only (h1ulink.py: no gameplay compared)')
    lines.append('check placements %s' % ('same counts' if placements_same else 'DIFFER'))
    lines.append('check palettes %s' % ('same counts' if palettes_same else 'DIFFER'))

    out = arguments.out or os.path.join(os.path.dirname(os.path.abspath(arguments.xbox_map)),
                                        os.path.splitext(os.path.basename(arguments.xbox_map))[0] + '.h1ulink')
    with open(out, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    shared = sum(1 for group, name, tag_id, address in ce.tags if (group, name.lower()) in xbox_tags)
    print('%s: %d tags in the CE map, %d in the Xbox map, %d in both; placements %s' %
          (out, len(ce.tags), len(xbox.tags), shared, 'the same' if placements_same else 'DIFFER'))


if __name__ == '__main__':
    main()
