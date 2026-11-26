"""perf_profile.py LOG|profile.bin ELF [WINDOW...] [--from S] [--to S] [--top N]: a dense lab build's (OXBOX_DENSE_PERF)
sampling profile, symbolized: our code by the ELF's symbol table (llvm-nm -n -S: an address is named only inside a
function's size), retail's libraries by 4 KB region (port/oxbox/retail_syms.txt names too few of them; its nearest name
is shown as a hint); the functions by share of the samples (waiting = the game's thread blocked, e.g. for the graphics
chip).

LOG: its "profile N s:" lines (each window's most taken addresses only); WINDOW picks windows by their N.
profile.bin (v1.183's sampler, oxbox_perf.c sampler_save): every window's whole table of 16-byte buckets; --from/--to
(session seconds at the windows' ends) add up the windows in that range, else each window is shown alone; --part N
keeps the Nth run of windows (the session's clock starts again at each map load, so a part is about a map).
v2 (v1.211's dense sweep build): each window also says the sweep run it was in (key, run, bots, measuring) and the
boot, and the file keeps every boot's; --runs adds up each sweep run's measured windows and shows its top functions
(--top N each), --csv FILE writes them as rows (key, run, bots, samples, waiting %, then function:percent pairs)."""
import os, re, struct, subprocess, sys, bisect, collections

args, opts = [], {'--from': None, '--to': None, '--top': '24', '--part': None, '--csv': None}
runs_mode = '--runs' in sys.argv
sys.argv = [a for a in sys.argv if a != '--runs']
it = iter(sys.argv[1:])
for a in it:
    if a in opts: opts[a] = next(it)
    else: args.append(a)
source, elf = args[0], args[1]
want = set(int(w) for w in args[2:])
top_n = int(opts['--top'])

syms = []
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'retail_syms.txt')):
    p = line.split()
    if len(p) >= 2 and not line.startswith('#'):
        try: syms.append((int(p[1], 16), p[0]))
        except ValueError: pass
syms.sort(); keys = [a for a, _ in syms]
# our code by the ELF's symbol table (llvm-nm -n -S: functions with sizes; the ELF has no debug lines)
funcs = []
for line in subprocess.run(['llvm-nm', '-n', '-S', elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4 and p[2] in 'Tt':
        funcs.append((int(p[0], 16), int(p[1], 16), p[3]))
fkeys = [f[0] for f in funcs]


def name_of(a, bucket=16 if source.endswith('.bin') else 1):
    i = bisect.bisect_right(fkeys, a) - 1
    # (a 16-byte bucket can hold the next function's start: both named, as which one the samples were in is unknown;
    # e.g. the frame-rate wait's calls of cache_files_precache_in_progress, whose first bytes share shared_slot_kept's
    # last bucket)
    later = funcs[i + 1][2] if i + 1 < len(funcs) and funcs[i + 1][0] < a + bucket else None
    if i >= 0 and a < funcs[i][0] + max(funcs[i][1], 1):
        return funcs[i][2] + (' / ' + later if later else '')
    if later:
        return later
    # (retail's libraries: retail_syms.txt names too few to trust; by 4 KB region, the nearest name as a hint)
    j = bisect.bisect_right(keys, a) - 1
    return 'retail library %05x..' % (a & ~0xfff) + (' (after %s)' % syms[j][1] if j >= 0 else '')


# windows: (end seconds, samples, waiting, ready, other, no frame, table full, singles, [(address, count)],
# (key, run, bots, measuring, boot) or None)
windows = []
if source.endswith('.bin'):
    b = open(source, 'rb').read()
    i = 0
    while i + 48 <= len(b):
        h = struct.unpack_from('<12I', b, i)
        if h[0] != 0x4650584f or h[1] not in (1, 2):
            print('profile.bin: no window header at byte %d' % i); break
        i += 48
        run = None
        if h[1] == 2:
            if i + 16 + 48 > len(b): break
            r, bots, measuring, boot = struct.unpack_from('<iIII', b, i)
            key = b[i + 16:i + 64].split(b'\0')[0].decode('latin1')
            run = (key, r, bots, measuring, boot)
            i += 64
        n = h[11]
        if i + n * 6 > len(b):
            print('profile.bin: window %d cut short' % h[2]); break
        entries = [struct.unpack_from('<IH', b, i + k * 6) for k in range(n)]
        i += n * 6
        windows.append((h[3] / 1000.0, h[4], h[5], h[6], h[7], h[8], h[9], h[10], entries, run))
else:
    for line in open(source, errors='replace'):
        m = re.search(r'profile (\d+) s: (\d+) samples, waiting (\d+), ready (\d+) \((\d+) not in code\), other (\d+); top:(.*)', line)
        if m:
            windows.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(6)),
                            int(m.group(5)), 0, 0, [(int(a, 16), int(c)) for a, c in re.findall(r' ([0-9a-f]+):(\d+)', m.group(7))], None))


if opts['--part'] is not None:
    parts, last = [[]], None
    for w in windows:
        if last is not None and w[0] < last:
            parts.append([])
        parts[-1].append(w); last = w[0]
    windows = parts[int(opts['--part']) - 1] if int(opts['--part']) <= len(parts) else []


def tally(ws):
    agg = collections.Counter()
    for w in ws:
        for a, c in w[8]:
            agg[re.sub(r'\+[0-9a-f]+$', '', name_of(a))] += c
    return agg


def show(title, ws):
    samples = sum(w[1] for w in ws) or 1
    agg = tally(ws)
    shown = sum(agg.values())
    waiting, no_frame, full, singles = (sum(w[k] for w in ws) for k in (2, 5, 6, 7))
    print('== %s: %d samples, waiting %.1f%%, no frame found %.1f%%, table full %.1f%%, buckets taken once %.1f%%, '
          'named below from %.0f%% of the samples' % (title, samples, 100.0 * waiting / samples, 100.0 * no_frame / samples,
                                                      100.0 * full / samples, 100.0 * singles / samples, 100.0 * shown / samples))
    for fn, c in agg.most_common(top_n):
        print('   %5.1f%%  %s' % (100.0 * c / samples, fn))


if runs_mode:
    # (each sweep run: the windows taken while it measured, in order; a run met again (another boot, or measured again)
    # is added to)
    groups = collections.OrderedDict()
    for w in windows:
        if w[9] and w[9][3] and w[9][1] >= 0:
            groups.setdefault((w[9][0], w[9][1], w[9][2]), []).append(w)
    out = open(opts['--csv'], 'w') if opts['--csv'] else None
    if out: out.write('key,run,bots,samples,waiting_pct,top\n')
    for (key, run, bots), ws in groups.items():
        show('%s run %d (%d bots), %d windows' % (key, run, bots, len(ws)), ws)
        if out:
            samples = sum(w[1] for w in ws) or 1
            agg = tally(ws)
            out.write('"%s",%d,%d,%d,%.1f,"%s"\n' % (key, run, bots, samples, 100.0 * sum(w[2] for w in ws) / samples,
                      ';'.join('%s:%.1f' % (fn, 100.0 * c / samples) for fn, c in agg.most_common(top_n))))
    print('%d sweep runs from %d windows (%d boots)' % (len(groups), len(windows),
          len(set(w[9][4] for w in windows if w[9]))))
elif opts['--from'] is not None or opts['--to'] is not None:
    lo = float(opts['--from'] or 0); hi = float(opts['--to'] or 1e9)
    ws = [w for w in windows if lo <= w[0] <= hi]
    show('windows ending %g-%g s (%d)' % (lo, hi, len(ws)), ws)
else:
    for w in windows:
        if want and int(w[0]) not in want: continue
        show('%g s' % w[0], [w])
