"""oxsign.py: signing Halo 1 Unlimited's packages, on the project owner's PC (port/oxbox/installer/README.md).

The installer and the game trust the public keys in port/oxbox/installer/keys.txt: they install an update only if it
carries a signature by one of them. The secret keys stay on the owner's PC (and a backup offline), protected by a
passphrase, and never go in the repository. Plain Python 3, nothing to install.

    python3 oxsign.py keygen NAME              a new key: NAME.oxkey (secret) and NAME.pub (its public line)
    python3 oxsign.py sign KEY.oxkey FILE...   signs packages (.oxpk), replacing that key's earlier signature
    python3 oxsign.py verify FILE... [--keys keys.txt]   checks packages and their signatures
    python3 oxsign.py show FILE...             a package's manifest

Make two keys: "main" to sign releases, and "backup", kept offline, for if main is ever lost or leaked (an update
signed with backup can then trust a new main). Signatures are Ed25519 (RFC 8032), the same the installer checks with
Monocypher; signing here isn't constant-time, which only matters on a machine someone else can measure.
"""
import getpass, hashlib, hmac, os, struct, sys

# ---- Ed25519 (RFC 8032), in plain Python ----

P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def _inverse(x):
    return pow(x, P - 2, P)


def _add(a, b):
    """points in extended coordinates (X, Y, Z, T)"""
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    pa = (y1 - x1) * (y2 - x2) % P
    pb = (y1 + x1) * (y2 + x2) % P
    pc = t1 * 2 * D * t2 % P
    pd = z1 * 2 * z2 % P
    e, f, g, h = pb - pa, pd - pc, pd + pc, pb + pa
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _multiply(s, point):
    result = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            result = _add(result, point)
        point = _add(point, point)
        s >>= 1
    return result


def _same(a, b):
    return (a[0] * b[2] - b[0] * a[2]) % P == 0 and (a[1] * b[2] - b[1] * a[2]) % P == 0


def _recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * _inverse(D * y * y + 1) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * _inverse(5) % P
_GX = _recover_x(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % P)


def _compress(point):
    z = _inverse(point[2])
    x, y = point[0] * z % P, point[1] * z % P
    return int.to_bytes(y | ((x & 1) << 255), 32, 'little')


def _decompress(data):
    if len(data) != 32:
        return None
    y = int.from_bytes(data, 'little')
    sign, y = y >> 255, y & ((1 << 255) - 1)
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % P)


def _hash_int(*parts):
    return int.from_bytes(hashlib.sha512(b''.join(parts)).digest(), 'little')


def _expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], 'little')
    a = (a & ((1 << 254) - 8)) | (1 << 254)
    return a, h[32:]


def ed25519_public(seed):
    return _compress(_multiply(_expand(seed)[0], _G))


def ed25519_sign(seed, message):
    a, prefix = _expand(seed)
    public = _compress(_multiply(a, _G))
    r = _hash_int(prefix, message) % L
    big_r = _compress(_multiply(r, _G))
    s = (r + _hash_int(big_r, public, message) % L * a) % L
    return big_r + int.to_bytes(s, 32, 'little')


def ed25519_verify(public, message, signature):
    if len(public) != 32 or len(signature) != 64:
        return False
    a, r = _decompress(public), _decompress(signature[:32])
    s = int.from_bytes(signature[32:], 'little')
    if a is None or r is None or s >= L:
        return False
    h = _hash_int(signature[:32], public, message) % L
    return _same(_multiply(s, _G), _add(r, _multiply(h, a)))


# ---- keys ----

SCRYPT = (17, 8, 1)   # N = 2^17, r, p: half a second or so on a PC


def key_id(public):
    """a key's short name: the start of its SHA-256"""
    return hashlib.sha256(public).digest()[:8]


def _derive(passphrase, salt, n_log, r, p):
    return hashlib.scrypt(passphrase.encode('utf-8'), salt=salt, n=2 ** n_log, r=r, p=p, maxmem=2 ** 29, dklen=64)


def keygen(name, folder='.'):
    if not name.replace('-', '').replace('_', '').isalnum() or len(name) > 24:
        sys.exit('oxsign: a key name is letters, digits, - and _ (up to 24)')
    secret_path, public_path = os.path.join(folder, name + '.oxkey'), os.path.join(folder, name + '.pub')
    for path in (secret_path, public_path):
        if os.path.exists(path):
            sys.exit('oxsign: %s is already there: not replacing a key' % path)
    print('A passphrase protects the secret key file (if someone copies the file, they still can\'t sign with it).')
    passphrase = getpass.getpass('Passphrase for %s: ' % name)
    if passphrase != getpass.getpass('Again: '):
        sys.exit('oxsign: the passphrases differ: no key made')
    if not passphrase:
        print('(no passphrase: anyone with the file can sign as this key)')
    seed = os.urandom(32)
    public = ed25519_public(seed)
    salt = os.urandom(16)
    derived = _derive(passphrase, salt, *SCRYPT)
    sealed = bytes(a ^ b for a, b in zip(seed, derived[:32]))
    check = hmac.new(derived[32:], sealed + public, hashlib.sha256).digest()[:16]
    with open(secret_path, 'x') as f:
        f.write('Halo 1 Unlimited signing key "%s": SECRET. Never put it on GitHub; keep a copy offline.\n' % name)
        f.write('name %s\nid %s\npublic %s\nscrypt %d %d %d %s\nsealed %s\ncheck %s\n' % (
            name, key_id(public).hex(), public.hex(), SCRYPT[0], SCRYPT[1], SCRYPT[2], salt.hex(), sealed.hex(),
            check.hex()))
    line = '%s %s %s' % (name, key_id(public).hex(), public.hex())
    with open(public_path, 'x') as f:
        f.write(line + '\n')
    print('%s: the secret key (keep it private, and back it up offline)' % secret_path)
    print('%s: its public line, for port/oxbox/installer/keys.txt (not secret):' % public_path)
    print('    ' + line)


def load_key(path):
    fields = {}
    for text in open(path, encoding='ascii'):
        parts = text.split()
        if len(parts) >= 2 and parts[0] in ('name', 'id', 'public', 'scrypt', 'sealed', 'check'):
            fields[parts[0]] = parts[1:]
    try:
        name = fields['name'][0]
        public = bytes.fromhex(fields['public'][0])
        n_log, r, p = (int(v) for v in fields['scrypt'][:3])
        salt = bytes.fromhex(fields['scrypt'][3])
        sealed = bytes.fromhex(fields['sealed'][0])
        check = bytes.fromhex(fields['check'][0])
    except (KeyError, IndexError, ValueError):
        sys.exit('oxsign: %s isn\'t a signing key file' % path)
    passphrase = getpass.getpass('Passphrase for %s: ' % name)
    derived = _derive(passphrase, salt, n_log, r, p)
    if not hmac.compare_digest(hmac.new(derived[32:], sealed + public, hashlib.sha256).digest()[:16], check):
        sys.exit('oxsign: wrong passphrase')
    seed = bytes(a ^ b for a, b in zip(sealed, derived[:32]))
    if ed25519_public(seed) != public:
        sys.exit('oxsign: %s is damaged' % path)
    return name, seed, public


def load_trusted(path):
    """keys.txt: "name id public" lines (# comments)"""
    keys = []
    for number, text in enumerate(open(path, encoding='ascii'), 1):
        text = text.split('#', 1)[0].strip()
        if not text:
            continue
        parts = text.split()
        if len(parts) != 3 or len(parts[2]) != 64 or key_id(bytes.fromhex(parts[2])).hex() != parts[1]:
            sys.exit('oxsign: %s line %d isn\'t "name id public"' % (path, number))
        keys.append((parts[0], bytes.fromhex(parts[1]), bytes.fromhex(parts[2])))
    return keys


# ---- packages (format 2: port/oxbox/tools/mkpackage.py) ----

SIGNED_PREFIX = b'Halo 1 Unlimited package, signed\n'
HEADER = 128


class Package:
    def __init__(self, path):
        self.path = path
        data = open(path, 'rb').read()
        if data[:4] != b'OXPK':
            sys.exit('oxsign: %s isn\'t a package' % path)
        fmt, packed, size = struct.unpack_from('<III', data, 4)
        if fmt != 2:
            sys.exit('oxsign: %s is a format %d package: only format 2 packages carry signatures' % (path, fmt))
        manifest_size, = struct.unpack_from('<I', data, 112)
        self.header = data[:HEADER]
        self.manifest = data[HEADER:HEADER + manifest_size]
        body = data[HEADER + manifest_size:HEADER + manifest_size + packed]
        if len(body) != packed or hashlib.sha256(body).digest() != data[16:48]:
            sys.exit('oxsign: %s is damaged (its contents don\'t match its SHA-256)' % path)
        self.body = body
        rest = data[HEADER + manifest_size + packed:]
        self.signatures = []
        if rest:
            if rest[:4] != b'OXSG':
                sys.exit('oxsign: %s has something after its contents that isn\'t signatures' % path)
            count, = struct.unpack_from('<I', rest, 4)
            if len(rest) != 8 + count * 72:
                sys.exit('oxsign: %s\'s signatures don\'t read' % path)
            for k in range(count):
                at = 8 + k * 72
                self.signatures.append((rest[at:at + 8], rest[at + 8:at + 72]))

    def signed_part(self):
        return SIGNED_PREFIX + self.header + self.manifest

    def save(self):
        out = self.header + self.manifest + self.body
        if self.signatures:
            out += b'OXSG' + struct.pack('<I', len(self.signatures))
            for kid, signature in self.signatures:
                out += kid + signature
        temp = self.path + '.new'
        with open(temp, 'wb') as f:
            f.write(out)
        os.replace(temp, self.path)


def sign(key_path, paths):
    name, seed, public = load_key(key_path)
    kid = key_id(public)
    for path in paths:
        package = Package(path)
        signature = ed25519_sign(seed, package.signed_part())
        assert ed25519_verify(public, package.signed_part(), signature)
        package.signatures = [s for s in package.signatures if s[0] != kid] + [(kid, signature)]
        package.save()
        print('%s: signed with %s (%s)' % (path, name, kid.hex()))


def verify(paths, keys_path):
    trusted = load_trusted(keys_path) if keys_path else []
    bad = False
    for path in paths:
        package = Package(path)
        print('%s: contents match their SHA-256' % path)
        if not package.signatures:
            print('  not signed')
        for kid, signature in package.signatures:
            known = [k for k in trusted if k[1] == kid]
            if not known:
                print('  signature by key %s: not a key in %s' % (kid.hex(), keys_path or '(no keys given)'))
            elif ed25519_verify(known[0][2], package.signed_part(), signature):
                print('  signature by %s (%s): good' % (known[0][0], kid.hex()))
            else:
                print('  signature by %s (%s): BAD (changed after it was signed)' % (known[0][0], kid.hex()))
                bad = True
    sys.exit(1 if bad else 0)


def show(paths):
    for path in paths:
        package = Package(path)
        print('%s:' % path)
        print(package.manifest.decode('utf-8', 'replace').rstrip())


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == 'keygen':
        keygen(args[1], args[2] if len(args) > 2 else '.')
    elif len(args) >= 3 and args[0] == 'sign':
        sign(args[1], args[2:])
    elif len(args) >= 2 and args[0] == 'verify':
        keys = None
        if '--keys' in args:
            k = args.index('--keys')
            keys = args[k + 1]
            args = args[:k] + args[k + 2:]
        verify(args[1:], keys)
    elif len(args) >= 2 and args[0] == 'show':
        show(args[1:])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
