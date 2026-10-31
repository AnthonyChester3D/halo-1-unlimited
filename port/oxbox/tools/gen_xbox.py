"""Writes build.ninja for the OpenCE-on-Xbox proof of concept (game code + our Xbox layer, linked against the
retail Halo XBE's libraries). Run from the repository's root: python3 -I port/oxbox/tools/gen_xbox.py > build-xbox/build.ninja
(port/oxbox/build.sh does)"""
import json, os
from pathlib import Path

cfg = json.loads(Path('port/linux/port.json').read_text())
game = cfg['game']
excl = game['exclude']
srcs = sorted(p for p in Path(game['root']).rglob('*.c')
              if not any(p.as_posix() == e or (e.endswith('/') and p.as_posix().startswith(e)) for e in excl))
# the port's game units, without the PC-only ones: Custom Edition maps (6.7 MB of tables), the tag validator
# (3 MB) and the PC version's map sharing; port/oxbox/oxbox_port_game.c answers their hooks as "not here"
PC_ONLY = ('custom_edition_', 'tag_validate', 'map_share', 'render_interpolation', 'menu_functions', 'menu_tags',
           # (test build 14: OpenCE's PC extras, all switched off on the Xbox: port/oxbox/oxbox_port_game.c)
           'first_person_legs', 'object_mesh', 'shield_glow', 'shield_color', 'view_fov', 'loose_sounds',
           'network_votekick', 'network_voice', 'hud_hires_tags', 'object_bounds_cache')
port_game = sorted(p for p in Path(cfg['game_sources']).glob('*.c') if not p.name.startswith(PC_ONLY))
B = 'build-xbox'
X = 'port/oxbox'

abi = ['--target=i686-linux-gnu', '-m32', '-fms-extensions', '-fshort-wchar', '-malign-double', '-fcommon', '-fno-pic',
       '-fno-strict-aliasing', '-fwrapv', '-fno-delete-null-pointer-checks', '-freg-struct-return',
       '-fno-omit-frame-pointer', '-ffp-contract=off', '-O2',
       # the Xbox's CPU: a 733 MHz Pentium III (Coppermine): MMX and SSE, no SSE2
       '-march=pentium3', '-mno-sse2',
       # Xbox code (MSVC, the libraries, the kernel) keeps the stack 4-byte aligned, not 16
       '-mstack-alignment=4',
       '-ffunction-sections', '-fdata-sections', '-fno-stack-protector', '-fno-asynchronous-unwind-tables',
       '-DHALO_RELEASE', '-DHALO_XBOX_TARGET', '-DOXBOX_STRIP', '-DOXBOX_BUILD_NUMBER=63', '-D__thread='] + os.environ.get('OXBOX_EXTRA', '').split()
abi += [f'-fno-builtin-{n}' for n in ('wcslen', 'wcsnlen', 'wcschr', 'wcsrchr', 'wcscmp', 'wcsncmp', 'wcscpy', 'wcsncpy',
                                      'wcscat', 'wcsncat', 'wmemchr', 'wmemcmp', 'wmemcpy', 'wmemmove', 'wmemset')]
gflags = ['-std=gnu89', '-D__STRICT_ANSI__', '-w', '-Wno-error=incompatible-pointer-types',
          '-Wno-error=incompatible-function-pointer-types', '-Wno-error=int-conversion',
          '-Wno-error=implicit-function-declaration', '-Wno-error=implicit-int', '-Wno-error=return-type']
inc = ['-include port/linux/include/halo_linux_prefix.h', f'-include {X}/include/halo_msvc_semantics.h', '-Iport/include',
       '-Iport/linux/include', '-iquote port/linux/game'] + [f'-D{d}' for d in game['defines']] + \
      [f'-I"{d}"' for d in game['include_dirs']] + ['-idirafter port/include/xdk']
game_cflags = ' '.join(abi + gflags + inc)
# our Xbox layer and the port units it reuses: the game's ABI and the XDK declarations
plat_cflags = ' '.join(abi + ['-std=gnu11', '-D_GNU_SOURCE', '-DHALO_LINUX_PLATFORM_LAYER', '-w', '-include port/linux/include/halo_linux_prefix.h',
                              f'-include {X}/include/halo_msvc_semantics.h', f'-I{X}/include', '-Iport/include', '-Iport/linux/include', '-Isource', '-Isource/cseries',
                              '-Iport/third_party/zlib', '-idirafter port/include/xdk'])
zlib_cflags = ' '.join(abi + ['-std=gnu11', '-DZ_PREFIX', '-Dz_errmsg=z_port_errmsg', '-w'])
musl_cflags = ' '.join(abi + ['-std=gnu11', '-w', '-Iport/third_party/musl-math/include',
                              '-include port/third_party/musl-math/include/libm.h'])
# (the online branch's own network stack: lwIP, nxdk's network card driver and our glue in port/oxbox/net; only the
# online test builds reference it: --gc-sections leaves it out of the others)
net_cflags = ' '.join(abi + ['-std=gnu11', '-w', '-Os', '-Wno-error=int-conversion', '-Wno-error=incompatible-pointer-types',
                             f'-I{X}/net', '-Iport/third_party/nxdk_net', '-Iport/third_party/lwip/src/include',
                             '-Iport/third_party/monocypher'])
# (internet play, the online test builds with OXBOX_NET_P2P alone: OpenCE's p2p layer as our fork has it
# (port/linux/src/p2p*.c), Monocypher and KCP, compiled beside our platform.h and pthread.h (port/oxbox/net/p2p); its
# platform calls are port/oxbox/net/oxbox_p2p_posix.c's)
P2P = '-DOXBOX_NET_P2P' in os.environ.get('OXBOX_EXTRA', '').split()
p2p_cflags = ' '.join(abi + ['-std=gnu11', '-w', '-Os', '-D_GNU_SOURCE', '-DHALO_LINUX_PLATFORM_LAYER', f'-I{X}/net/p2p',
                             '-include port/linux/include/halo_linux_prefix.h', f'-include {X}/include/halo_msvc_semantics.h',
                             f'-I{X}/include', '-Iport/include', '-Iport/linux/include', '-Iport/linux/src',
                             '-Iport/third_party/kcp', '-Iport/third_party/monocypher', '-idirafter port/include/xdk'])

print(f'game_cflags = {game_cflags}\nplat_cflags = {plat_cflags}\nzlib_cflags = {zlib_cflags}\nmusl_cflags = {musl_cflags}')
# (the update check, port/oxbox/net/oxbox_update.c, and Mbed TLS under it: link.ld puts both in the XBE section OXUPD)
TLS_FLAGS = f"-Iport/third_party/mbedtls/include -I{X}/net '-DMBEDTLS_CONFIG_FILE=\"oxbox_tls_config.h\"'"
tls_cflags = ' '.join(abi + ['-std=gnu11', '-w', '-Os', TLS_FLAGS])
print(f'net_cflags = {net_cflags}\np2p_cflags = {p2p_cflags}\ntls_cflags = {tls_cflags}')
for r in ('game', 'plat', 'zlib', 'musl', 'net', 'p2p', 'tls'):
    print(f'rule cc_{r}\n  command = clang -MMD -MF $out.d ${r}_cflags -c $in -o $out 2> $out.err || (cat $out.err; exit 1)'
          f'\n  depfile = $out.d\n  deps = gcc\n  description = CC $in')
print('rule copy\n  command = cp $in $out')

objs = []
def esc(p): return str(p).replace(' ', '$ ').replace(':', '$:')
def obj(s, sub=''):
    return Path(B) / 'obj' / sub / Path(str(Path(s).with_suffix('.o')).replace(' ', '_').replace('../', ''))
def add(s, rule, sub=''):
    o = obj(s, sub); objs.append(o); print(f'build {esc(o)}: cc_{rule} {esc(s)}')

# compiled for size, but for the per-frame drawing, collision, physics and math, which stay -O2 (speed)
# (test build 18: debug and profiling units in those folders, never run in a game, compiled for size too)
COLD = ('render_debug', 'rasterizer_xbox_profile', 'profile', 'collision_usage', 'rasterizer_debug', 'collision_debug')
HOT = ('rasterizer', 'render', 'math', 'physics', 'models', 'structures', 'effects', 'memory', 'cseries', 'objects')
# (test build 33's experiment: OXBOX_HOT_EXTRA="units devices items ai interface" compiles those folders -O2 too;
# that build says so, in the menus' build number and in its perf logs' build line)
HOT_EXTRA = tuple(os.environ.get('OXBOX_HOT_EXTRA', '').split())
HOT += HOT_EXTRA
VARIANT = ['-DOXBOX_BUILD_O2'] if HOT_EXTRA else []
VARIANT_UNITS = ('ui_widget_game_data_input_functions', 'oxbox_perf')
for s in srcs + port_game + [Path('port/third_party/stb_vorbis/stb_vorbis.c')]:
    add(s, 'game')
    parts = Path(s).parts
    flags = []
    if not (len(parts) > 1 and parts[0] == 'source' and parts[1] in HOT) or Path(s).stem in COLD:
        flags.append('-Os')
    if Path(s).stem in VARIANT_UNITS:
        flags += VARIANT
    if flags:
        print('  game_cflags = $game_cflags ' + ' '.join(flags))
# port units reused as they are, compiled beside our platform.h (they include "platform.h")
for name in ('halo_linker_common.c', 'msvc_wide.c'):
    c = f'{B}/portcopy/{name}'
    print(f'build {c}: copy port/linux/src/{name}')
    o = obj(name, 'portcopy'); objs.append(o); print(f'build {esc(o)}: cc_plat {c}\n  plat_cflags = $plat_cflags -I{X}/include')
for name in ('adler32.c', 'crc32.c', 'inffast.c', 'inflate.c', 'inftrees.c', 'uncompr.c', 'zutil.c'):
    add(f'port/third_party/zlib/{name}', 'zlib', 'zlib')
for s in sorted(Path('port/third_party/musl-math/src').glob('*.c')):
    add(s, 'musl')
LWIP = 'port/third_party/lwip/src'
for s in (sorted(Path(LWIP, 'core').glob('*.c')) + sorted(Path(LWIP, 'core/ipv4').glob('*.c')) +
          sorted(Path(LWIP, 'api').glob('*.c')) + [Path(LWIP, 'netif/ethernet.c')] +
          sorted(Path('port/third_party/nxdk_net').glob('*.c')) + sorted(Path(X, 'net').glob('*.c'))):
    add(s, 'net', 'net')
    if s.name == 'oxbox_update.c':
        print('  net_cflags = $net_cflags ' + TLS_FLAGS)
if P2P:
    for s in sorted(Path('port/third_party/mbedtls/library').glob('*.c')):
        add(s, 'tls', 'mbedtls')
    # (copied beside nothing, as the port units above are: their own folder's platform.h is the PC's)
    for name in ('p2p.c', 'p2p_crypto.c', 'p2p_lobby.c', 'p2p_signal.c'):
        c = f'{B}/portcopy/{name}'
        print(f'build {c}: copy port/linux/src/{name}')
        add(c, 'p2p', 'p2p')
    for s in ([Path('port/third_party/kcp/ikcp.c'), Path('port/third_party/monocypher/monocypher.c'),
               Path('port/third_party/monocypher/monocypher-ed25519.c')] + sorted(Path(X, 'net/p2p').glob('*.c'))):
        add(s, 'p2p', 'p2p')
for s in sorted(Path(X).glob('*.c')):
    # (the perf log reads the game's structures: a game unit; link.ld puts it in its own section, OXPERF)
    add(s, 'game' if s.name == 'oxbox_perf.c' else 'plat', 'xbox')
    if s.stem in VARIANT_UNITS and VARIANT:
        print('  game_cflags = $game_cflags ' + ' '.join(VARIANT))
    if s.name in ('oxbox_libc.c', 'oxbox_stdio.c', 'oxbox_math.c'):
        # (the C library itself: no loop may become a call to the function it is)
        print('  plat_cflags = $plat_cflags -ffreestanding -fno-builtin')
print('build all: phony ' + ' '.join(esc(o) for o in objs))
print('default all')
Path(B, 'objects.txt').write_text('\n'.join(str(o) for o in objs))
