"""pack_fingerprint.py PACK_FOLDER...: each map pack's fingerprint as the installer computes it (packs.c packs_scan: a
CRC-32 of its maps' "name size checksum" lines, lower case, in name order; ui.map and maps that aren't Xbox Halo
multiplayer or campaign maps left out), its map count and size: the first columns of a packs_known.txt line."""
import os, struct, sys, zlib


def fingerprint(folder):
    maps_dir = next((os.path.join(folder, d) for d in os.listdir(folder) if d.lower() == 'maps'), folder)
    rows = []
    for name in os.listdir(maps_dir):
        path = os.path.join(maps_dir, name)
        if not name.lower().endswith('.map') or not os.path.isfile(path):
            continue
        with open(path, 'rb') as f:
            head = f.read(0x800)
        if len(head) < 0x800 or head[:4] != b'daeh' or struct.unpack_from('<I', head, 4)[0] != 5:
            continue
        if struct.unpack_from('<h', head, 0x60)[0] not in (0, 1):
            continue
        rows.append((name[:-4], os.path.getsize(path), struct.unpack_from('<I', head, 0x64)[0]))
    rows.sort(key=lambda r: r[0].lower())
    text = ''.join('%s %d %08x\n' % r for r in rows).lower().encode('latin-1')
    return zlib.crc32(text) & 0xFFFFFFFF, len(rows), sum(r[1] for r in rows)


if __name__ == '__main__':
    for folder in sys.argv[1:]:
        fp, n, size = fingerprint(folder)
        print('%08x  %3d maps  %5d MB  %s' % (fp, n, size >> 20, os.path.basename(os.path.normpath(folder))))
