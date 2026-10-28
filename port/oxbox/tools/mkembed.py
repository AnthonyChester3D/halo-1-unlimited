"""mkembed.py: a file as a C array, for the installer (port/oxbox/installer/nxdk.mk: its THIRD_PARTY.txt, which it
writes beside itself, as notices.h). The output is rewritten only when it changes.

Usage: python3 port/oxbox/tools/mkembed.py IN OUT NAME"""
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    source, out, name = sys.argv[1:]
    data = open(source, 'rb').read()
    lines = ['/* %s, made from %s by port/oxbox/tools/mkembed.py: not to be edited */' % (out.replace('\\', '/').split('/')[-1],
             source.replace('\\', '/').split('/')[-1]),
             'static unsigned char const %s[%d] = {' % (name, len(data))]
    for at in range(0, len(data), 24):
        lines.append('\t' + ', '.join('%d' % b for b in data[at:at + 24]) + ',')
    lines.append('};')
    text = '\n'.join(lines) + '\n'
    try:
        if open(out).read() == text:
            return
    except OSError:
        pass
    open(out, 'w').write(text)


if __name__ == '__main__':
    main()
