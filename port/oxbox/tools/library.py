"""library.py: the map library's files (docs/map_library.md), made on the PC from Halo Map Porter's output.

    python3 library.py build OUT FOLDER... [--group NAME] [--previous CATALOG] [--base URL] [--pack]
    python3 library.py show CATALOG

build: each FOLDER is a map's folder (a .map and what goes with it: info.txt, preview.bin, bots.txt, showcase.txt,
a .h1ulink), or a folder of them (HMP's "Copy to Xbox maps\\Crossplay": each folder in it with a .map is one; its name
is their group unless --group says another). OUT gets a zip for each map (<folder>-<first 8 hex of its SHA-256>.zip:
the same files always make the same zip, so an unchanged map keeps its file and isn't uploaded again) and
catalog.oxlb, NOT signed: sign it on the owner's PC with oxsign.py (python3 oxsign.py sign library.oxkey
catalog.oxlb). --previous: the catalog now published, whose serial this one's goes past and whose items are kept
when they aren't made again (so a build can add one group at a time); the zips to upload are those it lists as new.
--base: where the zips are, when not beside the catalog (a URL ending in /). --pack: each FOLDER is a whole map pack's
folder (its maps' folders and its .shared file, as Halo Map Porter or tagpool --pack make it), one item "kind: pack"
that the Xbox puts in maps\\<folder>\\ as a pack of its own.

A folder's name is where the Xbox puts the map (maps\\Library\\<folder>\\, the library's maps together), and the map's
name in map sharing ("+Library/<folder>", which the game also finds as a copy put in maps\\<folder>\\ by hand), so it
must be the name the map has everywhere: Halo Map Porter's. At most 22 characters (the game's 30 for a pack's and its
map's together, "Library/" taking 8), and only what FATX allows.
"""
import hashlib, io, os, struct, sys, time, unicodedata, zipfile

MAGIC = b'OXLB'
FORMAT = 1
MAXIMUM_FOLDER = 22   # (the Xbox's maps\Library\<folder>: the game's 30 for "Library/<folder>")
MAXIMUM_NAME = 30     # (the game's, for a pack's name and its map's together: "<pack>/<map>")
MAXIMUM_ABOUT = 300
FATX_BAD = set('<>:"/\\|?*')
STORED = ('.map', '.zip')   # (Xbox maps are zlib inside already: 98% of their size when packed again)


def ascii_text(text):
    """the installer's font is ASCII: accents dropped, anything else as '?'"""
    text = unicodedata.normalize('NFKD', text)
    return ''.join(c if 32 <= ord(c) < 127 else '' if unicodedata.combining(c) else '?' for c in text)


def read_info(path):
    """info.txt: line 1 the name, then the description (UTF-8, UTF-16 with a BOM, or ANSI; "|n" breaks a line)"""
    data = open(path, 'rb').read()
    if data[:2] in (b'\xff\xfe', b'\xfe\xff'):
        text = data.decode('utf-16')
    else:
        try:
            text = data.decode('utf-8-sig')
        except UnicodeDecodeError:
            text = data.decode('cp1252', 'replace')
    lines = [line.strip() for line in text.replace('|n', '\n').splitlines()]
    lines = [line for line in lines if line]
    # (the description stops at Halo Map Porter's sections, "[Vehicles]"; "Version:" is a line of its own)
    about, version = [], ''
    for line in lines[1:]:
        if line.startswith('['):
            break
        if line.lower().startswith('version:'):
            version = line.split(':', 1)[1].strip()
        else:
            about.append(line)
    return (lines[0] if lines else ''), about, version


def map_folders(folder):
    """a map's folder itself, or the folders in it that are maps'"""
    files = os.listdir(folder)
    if any(f.lower().endswith('.map') and os.path.isfile(os.path.join(folder, f)) for f in files):
        return [folder]
    return sorted(os.path.join(folder, f) for f in files if os.path.isdir(os.path.join(folder, f)) and
                  any(g.lower().endswith('.map') for g in os.listdir(os.path.join(folder, f))))


def folder_problem(name):
    if len(name) > MAXIMUM_FOLDER:
        return 'its name is longer than %d characters (the Xbox\'s Library pack\'s limit): rename it in HMP\'s output' % \
            MAXIMUM_FOLDER
    if any(c in FATX_BAD or not 32 <= ord(c) < 127 for c in name) or name != name.strip() or name.endswith('.'):
        return 'its name has characters an Xbox folder can\'t have'
    if '.hmp_' in name.lower():
        return 'its name has ".hmp_" in it (the game skips such folders)'
    return None


def pack_problem(folder):
    """a whole pack's folder: its name, and each of its maps' names with it, within the game's limit"""
    name = os.path.basename(os.path.normpath(folder))
    if len(name) > MAXIMUM_NAME - 2:
        return 'its name is longer than %d characters' % (MAXIMUM_NAME - 2)
    if any(c in FATX_BAD or not 32 <= ord(c) < 127 for c in name) or name != name.strip() or name.endswith('.'):
        return 'its name has characters an Xbox folder can\'t have'
    if '.hmp_' in name.lower() or name.lower() == 'library':
        return 'its name can\'t be "%s"' % name
    maps = [m for m in map_folders(folder) if m != folder]
    if not maps:
        return 'no map\'s folder in it (a pack\'s maps are each in a folder of their own)'
    for m in maps:
        leaf = os.path.basename(m)
        if len(name) + 1 + len(leaf) > MAXIMUM_NAME:
            return '"%s/%s" is longer than the game\'s %d characters' % (name, leaf, MAXIMUM_NAME)
        if any(c in FATX_BAD or not 32 <= ord(c) < 127 for c in leaf):
            return '%s: its name has characters an Xbox folder can\'t have' % leaf
    return None


def make_zip(folder):
    """the folder's files as a zip, the same bytes every time: (zip bytes, unpacked size, file count)"""
    names = []
    for root, dirs, files in os.walk(folder):
        dirs.sort()
        for f in sorted(files):
            names.append(os.path.relpath(os.path.join(root, f), folder).replace(os.sep, '/'))
    out = io.BytesIO()
    unpacked = 0
    with zipfile.ZipFile(out, 'w') as z:
        for name in names:
            if name.count('/') > 1 or len(name) > 42 * 2:
                sys.exit('library: %s: %s is too deep or too long a name' % (folder, name))
            data = open(os.path.join(folder, name), 'rb').read()
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system = 0
            info.external_attr = 0
            info.compress_type = zipfile.ZIP_STORED if name.lower().endswith(STORED) else zipfile.ZIP_DEFLATED
            z.writestr(info, data, compresslevel=9)
            unpacked += len(data)
    return out.getvalue(), unpacked, len(names)


def read_catalog(path):
    """(serial, items: a dict of each item's fields, in order)"""
    return parse_catalog(open(path, 'rb').read(), path)


def parse_catalog(data, path='the catalog'):
    if data[:4] != MAGIC:
        sys.exit('library: %s isn\'t a catalog' % path)
    fmt, size = struct.unpack_from('<II', data, 4)
    text = data[12:12 + size].decode('ascii')
    serial, items, item = 0, [], None
    for line in text.split('\n'):
        key, _, value = line.partition(':')
        key, value = key.strip(), value.strip()
        if not key:
            item = None
            continue
        if key == 'serial':
            serial = int(value)
        elif key == 'item':
            item = {'item': value}
            items.append(item)
        elif item is not None:
            item[key] = value
    return serial, items


def catalog_bytes(serial, items):
    lines = ['library: Halo 1 Unlimited map library', 'format: %d' % FORMAT, 'serial: %d' % serial,
             'date: %s' % time.strftime('%Y-%m-%d', time.gmtime()), '']
    for item in items:
        lines.append('item: %s' % item['item'])
        for key in ('kind', 'name', 'group', 'about', 'version', 'file', 'size', 'unpacked', 'files', 'sha256'):
            if item.get(key):
                lines.append('%s: %s' % (key, item[key]))
        lines.append('')
    text = '\n'.join(lines).encode('ascii')
    return MAGIC + struct.pack('<II', FORMAT, len(text)) + text


def write_catalog(path, serial, items):
    with open(path, 'wb') as f:
        f.write(catalog_bytes(serial, items))


def item_key(item):
    """an item is its folder in its group: the same folder in two groups is two versions of a map, one or the other on
    an Xbox"""
    return item['item'].lower(), item.get('group', '').lower()


def zip_name(folder_name, digest):
    """(GitHub renames an uploaded file with other characters: letters, digits, '-', '.', '_' between them)"""
    safe = ''.join(c if c.isalnum() or c in '-.' else '_' for c in folder_name)
    while '__' in safe:
        safe = safe.replace('__', '_')
    return '%s-%s.zip' % (safe.strip('_.-') or 'map', digest[:8])


def make_item(folder, group, base='', pack=False):
    """a map's folder (pack: a whole pack's) as an item: (its catalog fields, its zip's bytes), or (None, why not)"""
    name = os.path.basename(os.path.normpath(folder))
    problem = pack_problem(folder) if pack else folder_problem(name)
    if problem:
        return None, problem
    data, unpacked, count = make_zip(folder)
    digest = hashlib.sha256(data).hexdigest()
    title, about, version = ('', [], '')
    info = os.path.join(folder, 'info.txt')
    if os.path.isfile(info):
        title, about, version = read_info(info)
    about = ascii_text('  '.join(about))
    if len(about) > MAXIMUM_ABOUT:
        about = about[:MAXIMUM_ABOUT - 3].rsplit(' ', 1)[0] + '...'
    item = {'item': name, 'name': ascii_text(title)[:48] or name, 'about': about, 'group': ascii_text(group or '')[:24],
            'file': base + zip_name(name, digest), 'size': str(len(data)), 'unpacked': str(unpacked),
            'files': str(count), 'version': ascii_text(version)[:12], 'sha256': digest}
    if pack:
        item['kind'] = 'pack'
    return item, data


def sort_items(items):
    items.sort(key=lambda item: (item.get('group', '').lower(), item['name'].lower(), item['item'].lower()))


def build(out, folders, group=None, previous=None, base=None, pack=False):
    os.makedirs(out, exist_ok=True)
    serial, kept = read_catalog(previous) if previous else (0, [])
    published = {item['file'] for item in kept}
    seen = set()
    made, new = [], []
    for given in folders:
        for folder in [given] if pack else map_folders(given):
            in_group = group or (os.path.basename(os.path.normpath(given)) if folder != given else '')
            name = os.path.basename(os.path.normpath(folder))
            if (name.lower(), in_group.lower()) in seen:
                print('  skipped %s: a second folder of that name in the group' % name)
                continue
            seen.add((name.lower(), in_group.lower()))
            item, data = make_item(folder, in_group, base or '', pack)
            if item is None:
                print('  skipped %s: %s' % (name, data))
                continue
            with open(os.path.join(out, item['file'][len(base or ''):]), 'wb') as f:
                f.write(data)
            made.append(item)
            if item['file'] not in published:
                new.append(item['file'])
    names = {item_key(item) for item in made}
    items = [item for item in kept if item_key(item) not in names] + made
    sort_items(items)
    write_catalog(os.path.join(out, 'catalog.oxlb'), serial + 1, items)
    print('%s: %d items (%d made now), serial %d, not signed' % (os.path.join(out, 'catalog.oxlb'), len(items),
                                                                 len(made), serial + 1))
    print('new files to upload (%d): %s' % (len(new), ', '.join(new) if new else 'none'))


def show(path):
    serial, items = read_catalog(path)
    data = open(path, 'rb').read()
    size = struct.unpack_from('<I', data, 8)[0]
    print('%s: serial %d, %d items, %s' % (path, serial, len(items),
                                          'signed' if data[12 + size:12 + size + 4] == b'OXSG' else 'not signed'))
    for item in items:
        print('  %-24s %-30s %7.1f MB  %s%s' % (item.get('group', ''), item['item'], int(item['size']) / 1e6,
                                               item['file'], '  (pack)' if item.get('kind') == 'pack' else ''))


def main():
    args = sys.argv[1:]
    options = {}
    for flag in ('--group', '--previous', '--base'):
        if flag in args:
            at = args.index(flag)
            options[flag[2:]] = args[at + 1]
            del args[at:at + 2]
    if '--pack' in args:
        args.remove('--pack')
        options['pack'] = True
    if len(args) >= 3 and args[0] == 'build':
        build(args[1], args[2:], **options)
    elif len(args) == 2 and args[0] == 'show':
        show(args[1])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
