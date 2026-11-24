"""perf_profile.py LOG ELF [WINDOW...]: a dense lab build's (OXBOX_DENSE_PERF) "profile N s:" lines, symbolized: our
code by the ELF's symbol table (llvm-nm -n -S: an address is named only inside a function's size), retail's libraries
by 4 KB region (port/oxbox/retail_syms.txt names too few of them; its nearest name is shown as a hint); per window, the
functions by share of the samples (waiting = the game's thread blocked, e.g. for the graphics chip)"""
import os, re, subprocess, sys, bisect, collections
log, elf = sys.argv[1], sys.argv[2]
want = set(int(w) for w in sys.argv[3:])
syms = []
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'retail_syms.txt')):
    p = line.split()
    if len(p) >= 2 and not line.startswith('#'):
        try: syms.append((int(p[1], 16), p[0]))
        except ValueError: pass
syms.sort(); keys = [a for a, _ in syms]
windows = []
for line in open(log, errors='replace'):
    m = re.search(r'profile (\d+) s: (\d+) samples, waiting (\d+), ready (\d+) \((\d+) not in code\), other (\d+); top:(.*)', line)
    if m:
        windows.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)),
                        [(int(a, 16), int(c)) for a, c in re.findall(r' ([0-9a-f]+):(\d+)', m.group(7))]))
addrs = sorted({a for w in windows for a, _ in w[4]})
# our code by the ELF's symbol table (llvm-nm -n -S: functions with sizes; the ELF has no debug lines)
funcs = []
for line in subprocess.run(['llvm-nm', '-n', '-S', elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4 and p[2] in 'Tt':
        funcs.append((int(p[0], 16), int(p[1], 16), p[3]))
fkeys = [f[0] for f in funcs]
name = {}
for a in addrs:
    i = bisect.bisect_right(fkeys, a) - 1
    if i >= 0 and a < funcs[i][0] + max(funcs[i][1], 1):
        name[a] = funcs[i][2]
    else:
        # (retail's libraries: retail_syms.txt names too few to trust; by 4 KB region, the nearest name as a hint)
        j = bisect.bisect_right(keys, a) - 1
        name[a] = 'retail library %05x..' % (a & ~0xfff) + (' (after %s)' % syms[j][1] if j >= 0 else '')
for t, samples, waiting, ready, top in windows:
    if want and t not in want: continue
    agg = collections.Counter()
    for a, c in top: agg[re.sub(r'\+[0-9a-f]+$', '', name[a])] += c
    shown = sum(c for _, c in top)
    print('== %d s: %d samples, waiting %.0f%%, top 48 addresses %.0f%% of the samples' % (t, samples, 100.0 * waiting / samples, 100.0 * shown / samples))
    for fn, c in agg.most_common(14):
        print('   %5.1f%%  %s' % (100.0 * c / samples, fn))
