"""mkkeys.py keys.txt keys.h: the public keys the installer trusts, as C (port/oxbox/installer/nxdk.mk runs it).
keys.txt has a "name id public" line for each (port/oxbox/tools/oxsign.py keygen prints it); keys.h is written only
when it changes."""
import hashlib, os, sys


def main():
    source, out = sys.argv[1], sys.argv[2]
    keys = []
    for number, text in enumerate(open(source, encoding='ascii'), 1):
        text = text.split('#', 1)[0].strip()
        if not text:
            continue
        parts = text.split()
        ok = len(parts) == 3 and len(parts[1]) == 16 and len(parts[2]) == 64 and parts[0].replace('-', '').replace(
            '_', '').isalnum() and len(parts[0]) <= 24
        if not ok or hashlib.sha256(bytes.fromhex(parts[2])).digest()[:8].hex() != parts[1]:
            sys.exit('mkkeys: %s line %d isn\'t "name id public" (as oxsign.py keygen prints it)' % (source, number))
        keys.append(parts)
    hexes = lambda h: ', '.join('0x' + h[i:i + 2] for i in range(0, len(h), 2))
    lines = ['/* keys.h: the public keys the installer trusts, made by port/oxbox/tools/mkkeys.py from %s */' %
             os.path.basename(source), 'static struct package_key const trusted_keys[] = {']
    for name, kid, public in keys:
        lines.append('\t{"%s", {%s},\n\t\t{%s}},' % (name, hexes(kid), hexes(public)))
    if not keys:
        lines.append('\t{"", {0}, {0}},')
    lines += ['};', 'static int const trusted_key_count = %d;' % len(keys), '']
    text = '\n'.join(lines)
    if not os.path.exists(out) or open(out).read() != text:
        open(out, 'w').write(text)
    print('%s: %d trusted key%s%s' % (out, len(keys), '' if len(keys) == 1 else 's',
                                       (' (' + ', '.join(k[0] for k in keys) + ')') if keys else ''))


if __name__ == '__main__':
    main()
