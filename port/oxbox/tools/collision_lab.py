"""collision_lab.py LOG ELF [--top N]: a lab build's (-DOXBOX_COLLISION_PROFILE, port/oxbox/oxbox_collision_lab.c)
collision profile, per perf sweep run: its "collisions 10 s:" windows added up (ticks, wall cycles, cycles in the
collision tests by test) and the "collision caller:" lines named (the caller and its caller, by the ELF's symbol
table) and grouped by caller function, per tick. A window counts toward the sweep run whose line came before it; one
across a map load (negative ticks: the game's count starts again) is left out."""
import bisect, collections, re, subprocess, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 25
if '--top' in sys.argv: args.remove(str(top))
log, elf = args[0], args[1]

funcs = []
for line in subprocess.run(['llvm-nm', '-n', elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'Tt': funcs.append((int(p[0], 16), p[2]))
funcs.sort(); keys = [a for a, _ in funcs]
def name(address):
    i = bisect.bisect_right(keys, address) - 1
    return funcs[i][1] if i >= 0 else '?'

runs = collections.OrderedDict(); run = None; skip = False
head = re.compile(r'collisions 10 s: (-?\d+) ticks, wall (\d+) Kc, collision (\d+) Kc \((\d+) lost\); by test:(.*)')
caller = re.compile(r'collision caller: (\w+) ([0-9a-f]+) ([0-9a-f]+): (\d+) calls, (\d+) Kc')
sweep = re.compile(r'perf sweep: map \d+ of \d+, (.*?) \(.*\), run (\d): (\d+) bots')
for line in open(log, encoding='latin1'):
    m = sweep.search(line)
    if m:
        run = '%s run %s (%s bots)' % m.groups()
        runs.setdefault(run, {'ticks': 0, 'wall': 0, 'coll': 0, 'kinds': collections.Counter(),
                              'kind_calls': collections.Counter(), 'callers': collections.Counter(),
                              'calls': collections.Counter(), 'windows': 0})
        continue
    if run is None: continue
    r = runs[run]
    m = head.search(line)
    if m:
        skip = int(m.group(1)) < 0      # (a window across a map load: the game's tick count starts again)
        if skip: continue
        r['windows'] += 1; r['ticks'] += int(m.group(1)); r['wall'] += int(m.group(2)); r['coll'] += int(m.group(3))
        for k, c, kc in re.findall(r' (\w+) (\d+)/(\d+) Kc', m.group(5)):
            r['kinds'][k] += int(kc); r['kind_calls'][k] += int(c)
        continue
    m = caller.search(line)
    if m and not skip:
        kind, a0, a1, calls, kc = m.groups()
        key = '%-18s %s <- %s' % (kind, name(int(a0, 16)), name(int(a1, 16)))
        r['callers'][key] += int(kc); r['calls'][key] += int(calls)

for run, r in runs.items():
    if not r['ticks']: continue
    t = float(r['ticks'])
    print('== %s: %d windows, %d ticks; collision %.0f Kc a tick = %.1f%% of the wall time' % (
        run, r['windows'], r['ticks'], r['coll'] / t, 100.0 * r['coll'] / max(r['wall'], 1)))
    print('   by test (Kc a tick, calls a tick): ' + ', '.join('%s %.0f/%.1f' % (k, v / t, r['kind_calls'][k] / t)
                                                              for k, v in r['kinds'].most_common()))
    shown = sum(r['callers'].values())
    print('   callers shown cover %.0f%% of the collision cycles' % (100.0 * shown / max(r['coll'], 1)))
    for key, kc in r['callers'].most_common(top):
        print('   %7.1f Kc %7.1f calls a tick  %s' % (kc / t, r['calls'][key] / t, key))
