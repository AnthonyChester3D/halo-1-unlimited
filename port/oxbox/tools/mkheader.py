"""mkheader.py: the ONLINE GAMES header (the Online Play screen's list, in place of SYSTEM LINK GAMES), made of the letters
of ui.map's SYSTEM LINK GAMES and SELECT PROFILE headers (512 x 64 DXT3, as the originals). Reads the two headers from
ui.map; writes online_games.png and online_games.dxt3 (32 KB: tools/mkxbe.py appends it to the online test builds).
Usage: python3 port/oxbox/tools/mkheader.py path/to/ui.map port/oxbox"""
import os, struct, sys, zlib
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxt3, dxt3enc

BASE = 0x803A6000
FOUND = 'ui\\shell\\main_menu\\multiplayer_type_select\\connected\\server_list\\header_found_games'
PROFILE = 'ui\\shell\\main_menu\\multiplayer_type_select\\connected\\4way_profile_select\\header_select_profile'

def map_bitmaps(path, names):
    raw = open(path, 'rb').read()
    data = raw[:2048] + zlib.decompress(raw[2048:])
    tag_offset = struct.unpack_from('<I', data, 16)[0]
    ptr = lambda p: p - BASE + tag_offset
    tag_array, _, _, count = struct.unpack_from('<IIII', data, tag_offset)
    found = {}
    for i in range(count):
        o = ptr(tag_array) + i * 32
        group = data[o:o + 4][::-1]
        name_ptr, data_ptr = struct.unpack_from('<II', data, o + 16)
        n = ptr(name_ptr)
        name = data[n:data.index(b'\0', n)].decode('latin1')
        if group == b'bitm' and name in names:
            t = ptr(data_ptr)
            n_bitmaps, bitmaps = struct.unpack_from('<II', data, t + 0x60)
            b = ptr(bitmaps)
            w, h = struct.unpack_from('<hh', data, b + 4)
            offset, size = struct.unpack_from('<II', data, b + 0x18)
            found[name] = np.asarray(Image.frombytes('RGBA', (w, h), dxt3.decode(data[offset:offset + size], w, h))).astype(np.float32)
    return found

# the letters' cores (columns, from the bright blue of their faces) in each header
FOUND_LETTERS = {'L': (174, 187), 'I': (191, 194), 'N': (200, 218), 'E': (113, 129), 'GAMES': (254, 366)}
FOUND_CORES = [(30, 49), (51, 90), (93, 110), (113, 129), (134, 157), (174, 187), (191, 194), (200, 218), (224, 241),
               (254, 273), (275, 296), (299, 323), (327, 343), (347, 366)]
PROFILE_LETTERS = {'O': (204, 224)}
PROFILE_CORES = [(30, 49), (53, 69), (74, 87), (90, 106), (110, 127), (129, 146), (161, 179), (183, 201), (204, 224),
                 (229, 245), (250, 253), (258, 271), (274, 291)]
# ONLINE GAMES: its pieces and the space before each (the originals' spacing: 3-5 between letters, 12-16 between words)
WORD = [('profile', 'O', 0), ('found', 'N', 4), ('found', 'L', 5), ('found', 'I', 3), ('found', 'N', 5),
        ('found', 'E', 4), ('found', 'GAMES', 13)]
LEFT = 30

def piece(image, cores, core):
    """a letter's columns with its glow: to halfway to the neighbours' cores (8 at the ends)"""
    x0, x1 = core
    before = [c for c in cores if c[1] < x0]
    after = [c for c in cores if c[0] > x1]
    left = (before[-1][1] + x0) // 2 + 1 if before else x0 - 8
    right = (x1 + after[0][0]) // 2 if after else x1 + 8
    return image[:, left:right + 1], x0 - left

def compose(found, profile):
    out = np.zeros((64, 512, 4), np.float32)
    x = LEFT
    for source, letter, space in WORD:
        image, cores, letters = (found, FOUND_CORES, FOUND_LETTERS) if source == 'found' else (profile, PROFILE_CORES, PROFILE_LETTERS)
        x += space
        core = letters[letter]
        sl, offset = piece(image, cores, core)
        for cx in range(sl.shape[1]):
            tx = x - offset + cx
            if 0 <= tx < 512:
                a = sl[:, cx, 3:4]
                o = out[:, tx]
                rgb = np.where(a > o[:, 3:4], sl[:, cx, :3], o[:, :3])
                out[:, tx, :3] = rgb
                out[:, tx, 3:4] = np.maximum(o[:, 3:4], a)
        x += core[1] - core[0] + 1
    return out.clip(0, 255).astype(np.uint8)

if __name__ == '__main__':
    folder = sys.argv[2] if len(sys.argv) > 2 else '.'
    bitmaps = map_bitmaps(sys.argv[1], {FOUND, PROFILE})
    image = compose(bitmaps[FOUND], bitmaps[PROFILE])
    Image.fromarray(image, 'RGBA').save(os.path.join(folder, 'online_games.png'))
    encoded = dxt3enc.encode(image)
    assert len(encoded) == 32768
    open(os.path.join(folder, 'online_games.dxt3'), 'wb').write(encoded)
    print('online_games.png, online_games.dxt3 (32768 bytes)')
