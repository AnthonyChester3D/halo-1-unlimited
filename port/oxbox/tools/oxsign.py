"""oxsign.py: signing Halo 1 Unlimited's packages, on the project owner's PC (port/oxbox/installer/README.md).

The installer and the game trust the public keys in port/oxbox/installer/keys.txt: they install an update only if it
carries a signature by one of them. The secret keys stay on the owner's PC (and a backup offline), protected by a
passphrase, and never go in the repository. Plain Python 3, nothing to install.

    python3 oxsign.py keygen NAME              a new key: NAME.oxkey (secret) and NAME.pub (its public line)
    python3 oxsign.py sign KEY.oxkey FILE...   signs packages (.oxpk), replacing that key's earlier signature
    python3 oxsign.py verify FILE... [--keys keys.txt]   checks packages and their signatures
    python3 oxsign.py show FILE...             a package's manifest

Signing in one go, on Windows (once: setup, with the key you sign with):

    py oxsign.py setup KEY.oxkey               double-clicking a .oxpk then shows it and its signatures and asks
                                               "Sign it with KEY?" (press Y); sign.cmd beside this does the same for
                                               packages dropped on it. The passphrase is asked the first time, and
                                               can be remembered on this PC (Windows keeps it encrypted to your login)
    py oxsign.py open FILE...                  what a double-click runs
    py oxsign.py forget                        forget the remembered passphrase (it's asked again next time)
    py oxsign.py unsetup                       undo setup (and forget)

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


class KeyFile:
    """a signing key file (keygen's): its name and public key, and the sealed secret"""
    def __init__(self, path):
        fields = {}
        try:
            lines = open(path, encoding='ascii').read().splitlines()
        except (OSError, UnicodeDecodeError):
            sys.exit('oxsign: can\'t read %s' % path)
        for text in lines:
            parts = text.split()
            if len(parts) >= 2 and parts[0] in ('name', 'id', 'public', 'scrypt', 'sealed', 'check'):
                fields[parts[0]] = parts[1:]
        try:
            self.name = fields['name'][0]
            self.public = bytes.fromhex(fields['public'][0])
            self.scrypt = tuple(int(v) for v in fields['scrypt'][:3])
            self.salt = bytes.fromhex(fields['scrypt'][3])
            self.sealed = bytes.fromhex(fields['sealed'][0])
            self.check = bytes.fromhex(fields['check'][0])
        except (KeyError, IndexError, ValueError):
            sys.exit('oxsign: %s isn\'t a signing key file' % path)
        self.path = path

    def unseal(self, passphrase):
        """the secret seed, or None for a wrong passphrase"""
        derived = _derive(passphrase, self.salt, *self.scrypt)
        if not hmac.compare_digest(hmac.new(derived[32:], self.sealed + self.public, hashlib.sha256).digest()[:16],
                                   self.check):
            return None
        seed = bytes(a ^ b for a, b in zip(self.sealed, derived[:32]))
        if ed25519_public(seed) != self.public:
            sys.exit('oxsign: %s is damaged' % self.path)
        return seed


def load_key(path):
    key = KeyFile(path)
    seed = key.unseal(getpass.getpass('Passphrase for %s: ' % key.name))
    if seed is None:
        sys.exit('oxsign: wrong passphrase')
    return key.name, seed, key.public


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


# ---- signing in one go (Windows: setup, then a double-click on a package) ----

PROG_ID = 'Halo1Unlimited.Package'
DPAPI_ENTROPY = b'Halo 1 Unlimited oxsign remembered key'
HERE = os.path.dirname(os.path.abspath(__file__))
TRUSTED_KEYS = os.path.join(HERE, '..', 'installer', 'keys.txt')


def _settings_folder():
    if sys.platform == 'win32':
        base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
        return os.path.join(base, 'Halo1Unlimited')
    return os.path.join(os.path.expanduser('~'), '.config', 'halo1unlimited')


def _settings_path():
    return os.path.join(_settings_folder(), 'oxsign.txt')


def _setting_key():
    """the key file setup chose, or None"""
    try:
        for text in open(_settings_path(), encoding='utf-8'):
            if text.startswith('key '):
                return text[4:].strip()
    except OSError:
        pass
    return None


def _remembered_path(key):
    return os.path.join(_settings_folder(), '%s-%s.remembered' % (key.name, key_id(key.public).hex()))


def _dpapi(data, protect):
    """Windows' DPAPI (CryptProtectData / CryptUnprotectData): data only this Windows login can read back"""
    import ctypes
    from ctypes import wintypes

    class Blob(ctypes.Structure):
        _fields_ = [('cbData', wintypes.DWORD), ('pbData', ctypes.POINTER(ctypes.c_ubyte))]

    def blob(b):
        buffer = (ctypes.c_ubyte * max(1, len(b))).from_buffer_copy(b or b'\0')
        return Blob(len(b), ctypes.cast(buffer, ctypes.POINTER(ctypes.c_ubyte))), buffer

    crypt32, kernel32 = ctypes.windll.crypt32, ctypes.windll.kernel32
    function = crypt32.CryptProtectData if protect else crypt32.CryptUnprotectData
    function.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p, ctypes.POINTER(Blob), ctypes.c_void_p,
                         ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(Blob)]
    function.restype = wintypes.BOOL
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    kernel32.LocalFree.restype = ctypes.c_void_p
    source, keep_source = blob(data)
    entropy, keep_entropy = blob(DPAPI_ENTROPY)
    result = Blob()
    description = 'Halo 1 Unlimited signing key' if protect else None
    # (1: CRYPTPROTECT_UI_FORBIDDEN)
    if not function(ctypes.byref(source), description, ctypes.byref(entropy), None, None, 1, ctypes.byref(result)):
        raise OSError('DPAPI: Windows error %d' % ctypes.GetLastError())
    try:
        return ctypes.string_at(result.pbData, result.cbData)
    finally:
        kernel32.LocalFree(ctypes.cast(result.pbData, ctypes.c_void_p))


def _remembered_seed(key):
    """the key's seed, if this PC remembers it (and it still matches the key file)"""
    if sys.platform != 'win32':
        return None
    try:
        seed = _dpapi(open(_remembered_path(key), 'rb').read(), False)
    except (OSError, ValueError):
        return None
    return seed if len(seed) == 32 and ed25519_public(seed) == key.public else None


def _unseal_for_signing(key):
    """the key's seed: remembered, else from its passphrase (and then, on Windows, maybe remembered)"""
    seed = _remembered_seed(key)
    if seed:
        return seed
    for attempt in range(3):
        seed = key.unseal(getpass.getpass('Passphrase for %s: ' % key.name))
        if seed:
            break
        print('Wrong passphrase.')
    else:
        sys.exit('oxsign: not signed')
    if sys.platform == 'win32':
        answer = input('Remember it on this PC, so it isn\'t asked again? Windows keeps it encrypted to your login. '
                       '[y/N] ').strip().lower()
        if answer.startswith('y'):
            try:
                os.makedirs(_settings_folder(), exist_ok=True)
                with open(_remembered_path(key), 'wb') as f:
                    f.write(_dpapi(seed, True))
                print('Remembered ("py oxsign.py forget" forgets it).')
            except OSError as e:
                print('Couldn\'t remember it (%s): it\'ll be asked next time.' % e)
    return seed


def _signatures_line(package, trusted):
    """the package's signatures, checked against keys.txt"""
    if not package.signatures:
        return 'not signed'
    out = []
    for kid, signature in package.signatures:
        known = [k for k in trusted if k[1] == kid]
        if not known:
            out.append('signed by a key keys.txt doesn\'t have (%s)' % kid.hex())
        elif ed25519_verify(known[0][2], package.signed_part(), signature):
            out.append('signed (%s): good' % known[0][0])
        else:
            out.append('signed (%s): BAD, changed after it was signed' % known[0][0])
    return ', '.join(out)


def open_packages(paths):
    """a double-click (or packages dropped on sign.cmd): show them, and sign them if asked"""
    try:
        trusted = load_trusted(TRUSTED_KEYS) if os.path.exists(TRUSTED_KEYS) else []
        packages = []
        for path in paths:
            package = Package(path)
            packages.append(package)
            fields = dict(line.split(': ', 1) for line in package.manifest.decode('utf-8', 'replace').splitlines()
                          if ': ' in line)
            print(os.path.basename(path))
            for name in ('name', 'release', 'network', 'date', 'notes', 'makes'):
                if fields.get(name):
                    print('  %s: %s' % (name, fields[name]))
            print('  %s' % _signatures_line(package, trusted))
        key_path = _setting_key()
        if not key_path:
            print('\nNo key set up to sign with ("py oxsign.py setup KEY.oxkey").')
        elif not os.path.exists(key_path):
            print('\nThe key set up to sign with isn\'t at %s any more (run setup again).' % key_path)
        else:
            key = KeyFile(key_path)
            what = 'it' if len(packages) == 1 else 'these %d packages' % len(packages)
            answer = input('\nSign %s with %s? [y/N] ' % (what, key.name)).strip().lower()
            if answer.startswith('y'):
                seed = _unseal_for_signing(key)
                kid = key_id(key.public)
                for package in packages:
                    signature = ed25519_sign(seed, package.signed_part())
                    assert ed25519_verify(key.public, package.signed_part(), signature)
                    package.signatures = [g for g in package.signatures if g[0] != kid] + [(kid, signature)]
                    package.save()
                    print('%s: %s' % (os.path.basename(package.path), _signatures_line(Package(package.path), trusted)))
            else:
                print('Not signed.')
    except SystemExit as e:
        if e.code not in (None, 0):
            print(e.code)
    except (KeyboardInterrupt, EOFError):
        print()
    except Exception as e:
        # (the window stays open to say what went wrong)
        print('oxsign: %s: %s' % (type(e).__name__, e))
    if sys.platform == 'win32':
        try:
            input('\nPress Enter to close.')
        except (KeyboardInterrupt, EOFError):
            pass


def _register_windows():
    """double-clicking a .oxpk runs "oxsign.py open" (this Windows user's file types: no administrator needed)"""
    import ctypes, winreg
    command = '"%s" "%s" open "%%1"' % (sys.executable, os.path.abspath(__file__))
    classes = 'Software\\Classes\\'
    winreg.SetValue(winreg.HKEY_CURRENT_USER, classes + '.oxpk', winreg.REG_SZ, PROG_ID)
    winreg.SetValue(winreg.HKEY_CURRENT_USER, classes + PROG_ID, winreg.REG_SZ, 'Halo 1 Unlimited package')
    winreg.SetValue(winreg.HKEY_CURRENT_USER, classes + PROG_ID + '\\shell\\open', winreg.REG_SZ,
                    'Check or sign (Halo 1 Unlimited)')
    winreg.SetValue(winreg.HKEY_CURRENT_USER, classes + PROG_ID + '\\shell\\open\\command', winreg.REG_SZ, command)
    # (SHCNE_ASSOCCHANGED: Explorer reads the file types again)
    ctypes.windll.shell32.SHChangeNotify(0x08000000, 0, None, None)
    # an "Open with" choice someone made for .oxpk wins over this
    try:
        winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                       'Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.oxpk\\UserChoice').Close()
        print('Note: Windows has another program chosen for .oxpk files. Right-click a package > Open with > '
              'Choose another app, and pick "Check or sign (Halo 1 Unlimited)" / Python, or use sign.cmd.')
    except OSError:
        pass


def _unregister_windows():
    import ctypes, winreg
    classes = 'Software\\Classes\\'
    for sub in ('\\shell\\open\\command', '\\shell\\open', '\\shell', ''):
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, classes + PROG_ID + sub)
        except OSError:
            pass
    try:
        if winreg.QueryValue(winreg.HKEY_CURRENT_USER, classes + '.oxpk') == PROG_ID:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, classes + '.oxpk')
    except OSError:
        pass
    ctypes.windll.shell32.SHChangeNotify(0x08000000, 0, None, None)


def setup(key_path):
    key_path = os.path.abspath(key_path)
    key = KeyFile(key_path)
    os.makedirs(_settings_folder(), exist_ok=True)
    with open(_settings_path(), 'w', encoding='utf-8') as f:
        f.write('# oxsign.py setup: the key a double-click on a package (oxsign.py open) signs with\n')
        f.write('key %s\n' % key_path)
    print('Signing with %s (%s), from %s' % (key.name, key_id(key.public).hex(), key_path))
    if sys.platform == 'win32':
        try:
            _register_windows()
            print('Double-click a .oxpk to check it and sign it (or drop packages on sign.cmd).')
        except OSError as e:
            print('Couldn\'t set up double-clicking (%s): drop packages on sign.cmd instead.' % e)
    else:
        print('"oxsign.py open FILE..." checks packages and signs them with it.')
    if sys.platform == 'win32':
        print('(A double-click runs this copy of oxsign.py: if you move the repository, run setup again.)')


def forget():
    removed = 0
    folder = _settings_folder()
    if os.path.isdir(folder):
        for name in os.listdir(folder):
            if name.endswith('.remembered'):
                os.remove(os.path.join(folder, name))
                removed += 1
    print('Forgot %d remembered key%s.' % (removed, '' if removed == 1 else 's') if removed else
          'Nothing was remembered.')


def unsetup():
    forget()
    try:
        os.remove(_settings_path())
    except OSError:
        pass
    if sys.platform == 'win32':
        _unregister_windows()
    print('Setup undone: a double-click on a package doesn\'t sign any more.')


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
    elif len(args) == 2 and args[0] == 'setup':
        setup(args[1])
    elif len(args) >= 2 and args[0] == 'open':
        open_packages(args[1:])
    elif args == ['forget']:
        forget()
    elif args == ['unsetup']:
        unsetup()
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
