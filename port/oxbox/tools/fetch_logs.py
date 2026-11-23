#!/usr/bin/env python3
"""fetch_logs.py: the logs players sent with "Send logs" (docs/log_uploads.md), fetched from the upload Worker.

    fetch_logs.py list                      what's there (newest last)
    fetch_logs.py fetch [OUT_DIR]           each bundle not yet in OUT_DIR (default ~/h1u-lab/uploads), unpacked into
                                            OUT_DIR/<name>/ (its files, and about.txt)
    fetch_logs.py delete NAME               one bundle taken off the Worker (they go by themselves after 30 days)
    fetch_logs.py unpack BUNDLE DIR         a bundle's files into DIR
    fetch_logs.py pack OUT [--code C] [--source 1|2] NAME=PATH...   a bundle, as a console makes it (for testing)

The Worker's address and read key: ~/.config/h1u/logs.conf, lines "url = https://..." and "key = ..." (or the
environment's H1U_LOGS_URL and H1U_LOGS_KEY)."""
import json
import os
import random
import struct
import sys
import urllib.request
import zlib

HEADER = 0x40
ENTRY = 80
ALPHABET = '23456789ABCDEFGHJKLMNPQRSTUVWXYZ'
SOURCES = {1: 'the game', 2: 'the installer'}


def config():
    url, key = os.environ.get('H1U_LOGS_URL', ''), os.environ.get('H1U_LOGS_KEY', '')
    path = os.path.expanduser('~/.config/h1u/logs.conf')
    if os.path.exists(path):
        for line in open(path):
            name, _, value = line.partition('=')
            if name.strip() == 'url' and not url:
                url = value.strip()
            elif name.strip() == 'key' and not key:
                key = value.strip()
    if not url or not key:
        sys.exit('no Worker address or read key: see ~/.config/h1u/logs.conf in this file\'s notes')
    return url.rstrip('/'), key


def request(method, path, url, key):
    r = urllib.request.Request(url + path, method=method,
                               headers={'Authorization': 'Bearer ' + key, 'User-Agent': 'h1u-fetch-logs/1'})
    with urllib.request.urlopen(r, timeout=60) as answer:
        return answer.read()


def unpack(data, out_dir):
    """a bundle's files into out_dir, and about.txt: what it is"""
    if data[:4] != b'OXLG' or struct.unpack_from('<I', data, 4)[0] != 1:
        raise ValueError('not an OXLG bundle')
    code = data[8:16].decode('ascii')
    source, build, installer, count, size, crc = struct.unpack_from('<IIIIII', data, 0x10)
    if size != len(data) or zlib.crc32(data[HEADER:]) & 0xFFFFFFFF != crc:
        raise ValueError('damaged bundle')
    os.makedirs(out_dir, exist_ok=True)
    about = ['code %s, from %s, game build %u, installer %u, %d files' % (code, SOURCES.get(source, source), build,
                                                                         installer, count)]
    at = HEADER
    for _ in range(count):
        name = data[at:at + 64].split(b'\0')[0].decode('utf-8', 'replace')
        whole, start, kept, packed = struct.unpack_from('<IIII', data, at + 64)
        body = zlib.decompress(data[at + ENTRY:at + ENTRY + packed])
        assert len(body) == kept, name
        at += ENTRY + packed
        safe = [p for p in name.replace('\\', '/').split('/') if p not in ('', '.', '..')]
        path = os.path.join(out_dir, *safe)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'wb').write(body)
        about.append('%s: %u bytes%s' % (name, whole, ' (its last %u)' % kept if start else ''))
    open(os.path.join(out_dir, 'about.txt'), 'w').write('\n'.join(about) + '\n')
    return about


def pack(out, files, code=None, source=2, build=0, installer=0):
    """a bundle of (name, bytes, whole size) as a console makes it"""
    code = code or ''.join(random.choice(ALPHABET) for _ in range(8))
    body = bytearray()
    for name, kept, whole in files:
        z = zlib.compress(kept, 6)
        body += name.encode()[:63].ljust(64, b'\0') + struct.pack('<IIII', whole, whole - len(kept), len(kept), len(z)) + z
    head = bytearray(b'OXLG' + struct.pack('<I', 1) + code.encode() + struct.pack('<IIII', source, build, installer,
                                                                               len(files)))
    head += struct.pack('<II', HEADER + len(body), zlib.crc32(body) & 0xFFFFFFFF)
    head += bytes(HEADER - len(head))
    open(out, 'wb').write(head + body)
    return code


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if args[0] == 'unpack' and len(args) == 3:
        print('\n'.join(unpack(open(args[1], 'rb').read(), args[2])))
        return
    if args[0] == 'pack' and len(args) >= 3:
        out, rest, code, source = args[1], args[2:], None, 2
        files = []
        while rest:
            a = rest.pop(0)
            if a == '--code':
                code = rest.pop(0)
            elif a == '--source':
                source = int(rest.pop(0))
            else:
                name, _, path = a.partition('=')
                data = open(path, 'rb').read()
                files.append((name, data, len(data)))
        print(pack(out, files, code, source))
        return
    url, key = config()
    if args[0] == 'list':
        for b in json.loads(request('GET', '/v1/logs', url, key)):
            print('%-25s %s from %s, build %s, installer %s, %s KB, %s' % (b['name'], b.get('code'), SOURCES.get(
                b.get('source'), '?'), b.get('build'), b.get('installer'), b.get('size', 0) // 1024, b.get('country')))
    elif args[0] == 'fetch':
        out = os.path.expanduser(args[1] if len(args) > 1 else '~/h1u-lab/uploads')
        os.makedirs(out, exist_ok=True)
        for b in json.loads(request('GET', '/v1/logs', url, key)):
            if os.path.exists(os.path.join(out, b['name'])):
                continue
            data = request('GET', '/v1/logs/' + b['name'], url, key)
            open(os.path.join(out, b['name'] + '.oxlg'), 'wb').write(data)
            print('%s: %s' % (b['name'], unpack(data, os.path.join(out, b['name']))[0]))
    elif args[0] == 'delete' and len(args) == 2:
        print(request('DELETE', '/v1/logs/' + args[1], url, key).decode())
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
