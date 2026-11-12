"""gen_packs_known.py: port/oxbox/installer/packs_known.txt made packs_known.h (the installer's table of known packs:
packs.c packs_known)."""
import os
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'installer')
rows = []
for line in open(os.path.join(HERE, 'packs_known.txt'), encoding='utf-8'):
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    parts = line.split(None, 5)
    fp, maps, before, after, seconds = parts[:5]
    name = parts[5] if len(parts) > 5 else ''
    name = name.split(' (')[0].replace('\\', '\\\\').replace('"', '\\"')
    rows.append('\t{0x%sul, "%s", %d, %dul, %dul, %dul},' % (fp.lower(), name, int(maps), int(before), int(after), int(seconds)))
with open(os.path.join(HERE, 'packs_known.h'), 'w') as out:
    out.write('/* packs_known.h: made by tools/gen_packs_known.py from packs_known.txt (edit that) */\n')
    out.write('static struct pack_known const known_packs[] = {\n%s\n};\n' % '\n'.join(rows or ['\t{0, "", 0, 0, 0, 0},']))
print('%d packs' % len(rows))
