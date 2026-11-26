#!/usr/bin/env python3
# port/oxbox/tools/perf_sweep_report.py: the perf sweep's results (perf\sweep.csv from the Xbox: opence.ini
# perf_sweep = 1, port/oxbox/oxbox_perf.c) sorted into groups: perfect, a little work, a lot of work, probably never,
# and the maps that crashed or didn't load (not a frame-rate verdict).
#
#   python3 perf_sweep_report.py sweep.csv [more.csv ...] [--summary maps.csv] [--html report.html]
#   python3 perf_sweep_report.py sweep.csv --write-list sweep_list.txt
#   python3 perf_sweep_report.py sweep.csv --compare sweep_r70.csv [--compare-csv fill.csv]
#   python3 perf_sweep_report.py sweep.csv --compare sweep_p1.csv      (another build: opence.ini perf_sweep_pass = 1)
#   python3 perf_sweep_report.py --library-list sweep_list.txt --library library.json   (a pass of the map library's maps)
#   python3 perf_sweep_report.py sweep_p4.csv --library library.json [--library-out verdicts.txt]
#
# The map library (docs/map_library.md): --library-list writes perf\sweep_list.txt with every map in the library's
# library.json (its keys: +Library/<item>), for a pass of them alone (opence.ini perf_sweep_pass = 4, say), and
# --library sorts that pass's results into what to take out of the library (crashed, hung or didn't load, or under
# --remove-fps frames a second with no bots), what to look at (under --check-fps with 4 bots), what's fine, and what
# wasn't tested (not downloaded on that Xbox, or the pass hasn't reached it).
#
# The resolution scaling test: a pass with opence.ini res_scale = 70 (say) writes perf\sweep_r70.csv, and plays only
# perf\sweep_list.txt's maps when there's one (--write-list: the maps under the "perfect" line whose slowest run kept
# the graphics chip busy, --list-gpu percent or more). --compare then says, per map, how its frame rate and the
# graphics chip's time a frame changed with the pixels: the share of that time that scaled with them (1.0 = all of
# it: fill-limited), and whether the map reaches the "perfect" line at that scale.
#
# Several files (from several Xboxes, or older copies) are read together; a map's newest version counts, and its
# newest row for each run.
#
# v1.199, the master table: perf\sweep.csv is the one table (no pass), its rows' last column the build's sweep version
# (raised by a build that changes how fast maps run: the Xbox then measures the slow maps again). A map's latest
# measurement is its rows at the highest sweep version; the report shows it, and what its slowest run was at the
# version before ("was"), so a build's gain shows without a second table. The pass tables before v1.199 (sweep_p4.csv
# and so on) went into it once, on the Xbox, at sweep version 0. The thresholds below are the starting ones: change them here, or with --perfect, --little
# and --lot (the average frame rate of the map's slowest run), and the report changes without testing again.
import argparse
import csv
import html
import io
import json
import os
import re
import sys

GROUPS = [
    ('perfect', 'Perfect already'),
    ('little', 'A little work'),
    ('lot', 'A lot of work'),
    ('never', 'Probably never'),
    ('broken', 'Crashed, hung or didn\'t load (not a frame-rate verdict)'),
    ('partial', 'Not finished (runs left)'),
]
RUN_NAMES = {0: 'no bots', 1: '4 bots', 2: 'full bots', 3: 'campaign', 4: 'at 85%', 5: 'at 70%'}
RUN_SCALES = {4: 85, 5: 70}   # (the scaling runs: the map's slowest run again, its 3D view drawn smaller)
LARGE_MP_KB = 47 * 1024   # (a multiplayer map over 47 MiB unpacked goes in a campaign cache file)


def number(row, name, default=None):
    try:
        return float(row.get(name, '') or '')
    except ValueError:
        return default


def read_rows(paths):
    """every row, as a dict by its table's header (a table may hold more than one: sweep.csv kept from before v1.199
    has the old header, and its new rows one column more: sweep_version); sweep_version an int, 0 when there's none"""
    rows = []
    for path in paths:
        with open(path, encoding='utf-8', errors='replace', newline='') as f:
            lines = [line for line in f if line.strip() and not line.startswith('#')]
        header = None
        for fields in csv.reader(io.StringIO(''.join(lines))):
            if fields and fields[0] == 'status':
                header = fields
                continue
            if not header:
                continue
            row = dict(zip(header, fields))
            if 'sweep_version' not in header and len(fields) > len(header):
                row['sweep_version'] = fields[len(header)]
            if row.get('status') and row.get('key') and row.get('run', '').isdigit():
                row['run'] = int(row['run'])
                try:
                    row['sweep_version'] = int(row.get('sweep_version') or 0)
                except ValueError:
                    row['sweep_version'] = 0
                rows.append(row)
    return rows


def newer(row, than):
    """a higher sweep version, else the later row"""
    return (row['sweep_version'], row['utc']) >= (than['sweep_version'], than['utc'])


def maps_from(rows, before=False):
    """key -> (its newest identity's runs at its highest sweep version: run -> newest row); before: the runs at the
    sweep version below that one instead (the measurement before: empty when there's none)"""
    newest_identity = {}
    for row in rows:
        key = row['key'].lower()
        if key not in newest_identity or row['utc'] >= newest_identity[key][1]:
            newest_identity[key] = (row['identity'], row['utc'])
    versions = {}
    for row in rows:
        key = row['key'].lower()
        if row['identity'] == newest_identity[key][0]:
            versions.setdefault(key, set()).add(row['sweep_version'])
    maps = {}
    for row in rows:
        key = row['key'].lower()
        if row['identity'] != newest_identity[key][0]:
            continue
        ordered = sorted(versions[key], reverse=True)
        want = ordered[1] if before and len(ordered) > 1 else None if before else ordered[0]
        if want is None or row['sweep_version'] != want:
            continue
        runs = maps.setdefault(key, {})
        if row['run'] not in runs or newer(row, runs[row['run']]):
            runs[row['run']] = row
    return maps


def cause(row):
    """what the slowest run's time went on, in a few words"""
    if not row or number(row, 'frames') is None:
        return ''
    tick, draw, present = number(row, 'tick_ms', 0), number(row, 'draw_ms', 0), number(row, 'present_ms', 0)
    gpu = number(row, 'gpu_busy_pct', 0)
    tops = ', '.join('%s %s ms' % (row['top%d' % k].replace('_ms', ''), row['top%d_ms' % k])
                     for k in (1, 2) if row.get('top%d' % k))
    if gpu >= 90 and present >= max(tick, draw) * 0.5:
        where = 'the graphics chip (GPU %d%% busy, waiting %.1f ms a frame)' % (gpu, present)
    elif tick >= draw:
        where = 'game ticks (%.1f ms a frame)' % tick
    else:
        where = 'the CPU drawing (%.1f ms a frame)' % draw
    return where + ('; ' + tops if tops else '')


def fill_share(base, scaled, scale):
    """the share of the graphics chip's time a frame that shrank with the pixels (1.0: all of it), or None"""
    pixels = (scale / 100.0) ** 2
    bg, sg = gpu_ms(base), gpu_ms(scaled)
    if not bg or sg is None or pixels >= 1.0:
        return None
    return max(0.0, min(1.0, (bg - sg) / (bg * (1.0 - pixels))))


def scaling(runs, slowest, perfect):
    """the scaling runs (4: 85%, 5: 70%) against the slowest normal run: (fps at 85, fps at 70, share, verdict)"""
    scaled = [(RUN_SCALES[r], runs[r]) for r in (4, 5) if r in runs]
    if not scaled or not slowest:
        return '', '', None, ''
    fps = {sc: row['avg_fps'] if row['status'] in ('ok', 'fail_fps') else row['status'] for sc, row in scaled}
    shares = [fill_share(slowest, row, sc) for sc, row in scaled if row['status'] in ('ok', 'fail_fps')]
    shares = [x for x in shares if x is not None]
    share = sum(shares) / len(shares) if shares else None
    reached = [sc for sc, row in scaled if row['status'] == 'ok' and number(row, 'avg_fps', 0) >= perfect]
    kind = '' if share is None else 'fill-limited' if share >= 0.6 else 'partly fill-limited' if share >= 0.3 else \
        'not fill-limited'
    verdict = ('reaches %g fps at %d%%' % (perfect, max(reached)) if reached else
               'still under %g fps at %d%%' % (perfect, min(sc for sc, _ in scaled)))
    return fps.get(85, ''), fps.get(70, ''), share, verdict + (', ' + kind if kind else '')


def judge(runs, perfect, little, lot):
    runs = {run: row for run, row in runs.items() if run <= 3}   # (the verdict is the normal runs')
    needed = [3] if 3 in runs else [0, 1, 2]
    statuses = {run: row['status'] for run, row in runs.items()}
    first = runs.get(3) or runs.get(0)
    if any(statuses.get(r) in ('crashed', 'hung', 'bad_map', 'no_start') for r in needed):
        return 'broken', None
    if any(statuses.get(r) == 'left' for r in needed):
        return 'broken', None
    measured = [row for row in runs.values() if row['status'] in ('ok', 'fail_fps') and number(row, 'avg_fps') is not None]
    if first and first['status'] == 'fail_fps':
        return 'never', first
    if not measured:
        return 'partial', None
    slowest = min(measured, key=lambda row: number(row, 'avg_fps', 0))
    worst_fps = number(slowest, 'avg_fps', 0)
    if any(row['status'] == 'fail_fps' for row in measured):
        return ('lot' if first and number(first, 'avg_fps', 0) >= lot else 'never'), slowest
    missing = [r for r in needed if r not in runs]
    if missing and missing != [2]:
        return 'partial', slowest
    if worst_fps >= perfect and all(number(row, 'under25_pct', 0) < 2.0 for row in measured):
        return 'perfect', slowest
    if worst_fps >= little:
        return 'little', slowest
    if worst_fps >= lot:
        return 'lot', slowest
    return 'never', slowest


def summary_rows(maps, perfect, little, lot, before=None):
    out = []
    for key, runs in maps.items():
        group, slowest = judge(runs, perfect, little, lot)
        was = ''
        if before and key in before:
            # (the same run as the slowest one now: a map that passes its 4-bot run on a newer build runs its full-bot
            # one, which the older build skipped, so the two slowest runs needn't be the same run)
            same = before[key].get(slowest['run']) if slowest else None
            if same and same.get('avg_fps'):
                was = 'run %s %s fps on %s' % (same['run'], same['avg_fps'], same['build'])
            else:
                _, old = judge(before[key], perfect, little, lot)
                if old and old.get('avg_fps'):
                    was = 'run %s not measured then (its slowest: run %s %s fps on %s)' % (
                        slowest['run'] if slowest else '?', old['run'], old['avg_fps'], old['build'])
        any_row = next(iter(runs.values()))
        info = max(runs.values(), key=lambda row: row['utc'])
        encounters = max((number(row, 'encounters', 0) or 0) for row in runs.values())
        actors = max((number(row, 'actors_peak', 0) or 0) for row in runs.values())
        map_kb = number(info, 'map_kb', 0) or 0
        line = {
            'group': group,
            'name': info['name'] or any_row['key'],
            'key': info['key'],
            'path': info['path'],
            'origin': info['origin'],
            'type': info['type'],
            'file_mb': '%.1f' % ((number(info, 'file_kb', 0) or 0) / 1024),
            'map_mb': '%.1f' % (map_kb / 1024),
            'large': 'yes' if info['type'] == 'mp' and map_kb > LARGE_MP_KB else '',
            'ai': 'yes' if encounters > 0 or actors > 0 else 'no',
            'encounters': '%d' % encounters,
            'bsps': info.get('bsps', ''),
            'slowest_fps': slowest['avg_fps'] if slowest else '',
            'slowest_low1_fps': slowest['low1_fps'] if slowest else '',
            'cause': cause(slowest),
            'limits': ' '.join(sorted({row['limits'] for row in runs.values() if row.get('limits')})),
            'build': info['build'],
            'sweep_version': str(info['sweep_version']),
            'was': was,
        }
        fps85, fps70, share, verdict = scaling(runs, slowest, perfect)
        line.update({'fps_85': fps85, 'fps_70': fps70, 'fill_share': '%.2f' % share if share is not None else '',
                     'scaling': verdict})
        for run in (0, 1, 2, 3, 4, 5):
            row = runs.get(run)
            if row is None:
                line['run%d' % run] = ''
            elif row['status'] == 'ok':
                line['run%d' % run] = '%s fps (1%% low %s)' % (row['avg_fps'], row['low1_fps'])
            elif row['avg_fps']:
                line['run%d' % run] = '%s (%s fps)' % (row['status'], row['avg_fps'])
            else:
                line['run%d' % run] = row['status']
        out.append(line)
    order = {g: k for k, (g, _) in enumerate(GROUPS)}
    out.sort(key=lambda m: (order[m['group']], -(float(m['slowest_fps'] or 0)), m['name'].lower()))
    return out


def text_report(lines, args):
    print('Perf sweep: %d maps (thresholds: perfect >= %g fps and under 25 fps < 2%% of the time, a little work >= %g, '
          'a lot >= %g; the slowest run\'s average)' % (len(lines), args.perfect, args.little, args.lot))
    for group, title in GROUPS:
        members = [m for m in lines if m['group'] == group]
        if not members:
            continue
        print('\n== %s (%d)' % (title, len(members)))
        for m in members:
            runs = '; '.join('%s: %s' % (RUN_NAMES[r], m['run%d' % r]) for r in (0, 1, 2, 3, 4, 5) if m['run%d' % r])
            flags = ', '.join(x for x in (m['type'].upper(), m['origin'], '%s MB' % m['map_mb'],
                                          'LARGE' if m['large'] else '', 'AI' if m['ai'] == 'yes' else '') if x)
            print('  %s  [%s]' % (m['name'], flags))
            print('      %s' % m['path'])
            if runs:
                print('      %s' % runs)
            if m['cause']:
                print('      slowest: %s' % m['cause'])
            if m['was']:
                print('      before (sweep version below): %s' % m['was'])
            if m['scaling']:
                print('      scaling: %s%s' % (m['scaling'], ' (share %s)' % m['fill_share'] if m['fill_share'] else ''))
            if m['limits']:
                print('      limits: %s' % m['limits'])
    scaled = [m for m in lines if m['scaling']]
    if scaled:
        print('\nScaling runs: %d maps; %d reach %g fps at 85%%, %d more at 70%%; %d fill-limited, %d partly, %d not'
              % (len(scaled), sum('at 85%' in m['scaling'] and 'reaches' in m['scaling'] for m in scaled), args.perfect,
                 sum('at 70%' in m['scaling'] and 'reaches' in m['scaling'] for m in scaled),
                 sum(m['scaling'].endswith(', fill-limited') for m in scaled),
                 sum('partly' in m['scaling'] for m in scaled), sum('not fill-limited' in m['scaling'] for m in scaled)))


def html_report(lines, path, args):
    parts = ['<!doctype html><meta charset="utf-8"><title>Perf sweep</title><style>body{font:14px system-ui;margin:16px;'
             'background:#fff;color:#111}table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ddd;'
             'padding:4px 6px;text-align:left;vertical-align:top}h2{margin-top:28px}.p{color:#666;font-size:12px}'
             '@media (prefers-color-scheme:dark){body{background:#111;color:#eee}td,th{border-color:#333}.p{color:#aaa}}'
             '</style><h1>Perf sweep: ' + str(len(lines)) + ' maps</h1>']
    for group, title in GROUPS:
        members = [m for m in lines if m['group'] == group]
        if not members:
            continue
        parts.append('<h2>%s (%d)</h2><table><tr><th>Map</th><th>Type</th><th>Size</th><th>No bots</th><th>4 bots</th>'
                     '<th>Full bots / campaign</th><th>Slowest part</th><th>85%% / 70%%</th></tr>' % (html.escape(title),
                                                                                              len(members)))
        for m in members:
            parts.append('<tr><td>%s<div class="p">%s</div></td><td>%s %s%s</td><td>%s MB%s</td><td>%s</td><td>%s</td>'
                         '<td>%s</td><td>%s%s</td><td>%s</td></tr>' % (
                             html.escape(m['name']), html.escape(m['path']), m['type'].upper(), html.escape(m['origin']),
                             ', AI' if m['ai'] == 'yes' else '', m['map_mb'], ' (large)' if m['large'] else '',
                             html.escape(m['run0']), html.escape(m['run1']), html.escape(m['run2'] or m['run3']),
                             html.escape(m['cause']),
                             '<div class="p">limits: %s</div>' % html.escape(m['limits']) if m['limits'] else '',
                             html.escape(' / '.join(x for x in (m['run4'], m['run5']) if x) +
                                         ('; ' + m['scaling'] if m['scaling'] else ''))))
        parts.append('</table>')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(''.join(parts))


def gpu_ms(row):
    """the graphics chip's busy time a frame (its busy share of the frame time)"""
    fps, busy = number(row, 'avg_fps'), number(row, 'gpu_busy_pct')
    if not fps or busy is None:
        return None
    return busy / 100.0 * 1000.0 / fps


def file_scale(path):
    """a results file's res_scale: its first line's, else its name's (sweep_r70.csv)"""
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            m = re.search(r'res_scale (\d+)', f.readline())
        if m:
            return int(m.group(1))
    except OSError:
        pass
    m = re.search(r'_r(\d+)\.csv$', os.path.basename(path))
    return int(m.group(1)) if m else None


def write_list(maps, path, args):
    keys = []
    for key, runs in maps.items():
        group, slowest = judge(runs, args.perfect, args.little, args.lot)
        if group in ('little', 'lot', 'never') and slowest is not None and \
                (number(slowest, 'gpu_busy_pct', 0) or 0) >= args.list_gpu:
            keys.append(slowest['key'])
    with open(path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(''.join(k + '\n' for k in sorted(keys, key=str.lower)))
    print('\nWrote %s: %d maps under %g fps whose slowest run kept the graphics chip %g%%+ busy'
          % (path, len(keys), args.perfect, args.list_gpu))


def compare_builds(base_maps, other_paths, args):
    """a pass on another build (perf_sweep_pass: sweep_p1.csv) against the files: each map's runs, fps and the
    processor's time a frame (game ticks, drawing), and the change overall"""
    other_maps = maps_from(read_rows(other_paths))
    builds = sorted({row['build'] for runs in other_maps.values() for row in runs.values()})
    print('\nBuild comparison: %s against %s' % (', '.join(other_paths), ', '.join(args.files)))
    print('  (the pass: %s)' % ', '.join(builds))
    changes = []
    for key, other_runs in sorted(other_maps.items()):
        base_runs = base_maps.get(key)
        if not base_runs:
            continue
        name = next(iter(other_runs.values()))['name'] or key
        lines = []
        for run, orow in sorted(other_runs.items()):
            brow = base_runs.get(run)
            if not brow or brow['identity'] != orow['identity'] or orow['status'] != 'ok' or brow['status'] != 'ok':
                continue
            bf, of = number(brow, 'avg_fps', 0), number(orow, 'avg_fps', 0)
            bt, ot = number(brow, 'tick_ms', 0), number(orow, 'tick_ms', 0)
            bd, od = number(brow, 'draw_ms', 0), number(orow, 'draw_ms', 0)
            bg, og = number(brow, 'gpu_busy_pct', 0), number(orow, 'gpu_busy_pct', 0)
            changes.append((bf, of, bt, ot, bd, od, bg, og))
            lines.append('      %s: %.1f -> %.1f fps; game ticks %.2f -> %.2f ms a frame (%+.0f%%), drawing %.2f -> %.2f ms, '
                         'graphics chip busy %.0f -> %.0f%%' % (
                RUN_NAMES[run], bf, of, bt, ot, 100.0 * (ot - bt) / bt if bt else 0.0, bd, od, bg, og))
        if lines:
            print('  %s' % name)
            print('\n'.join(lines))
    if changes:
        n = len(changes)
        print('\n%d runs in both: fps %.1f -> %.1f on average; game ticks %.2f -> %.2f ms a frame (%+.1f%%); drawing '
              '%.2f -> %.2f ms (%+.1f%%)' % (
                  n, sum(c[0] for c in changes) / n, sum(c[1] for c in changes) / n,
                  sum(c[2] for c in changes) / n, sum(c[3] for c in changes) / n,
                  100.0 * (sum(c[3] for c in changes) - sum(c[2] for c in changes)) / max(sum(c[2] for c in changes), 1e-9),
                  sum(c[4] for c in changes) / n, sum(c[5] for c in changes) / n,
                  100.0 * (sum(c[5] for c in changes) - sum(c[4] for c in changes)) / max(sum(c[4] for c in changes), 1e-9)))
        print('graphics chip busy %.0f%% -> %.0f%% on average' % (sum(c[6] for c in changes) / n, sum(c[7] for c in changes) / n))
    return 0


def compare(base_maps, scaled_paths, args):
    scale = file_scale(scaled_paths[0]) or args.scale
    if scale == 100 or re.search(r'_p\d+\.csv$', os.path.basename(scaled_paths[0])):
        return compare_builds(base_maps, scaled_paths, args)
    if not scale:
        print('--compare: the scale is unknown (no "res_scale N" line or _rN name): give --scale', file=sys.stderr)
        return 1
    pixels = (scale / 100.0) ** 2
    scaled_maps = maps_from(read_rows(scaled_paths))
    lines = []
    for key, scaled_runs in scaled_maps.items():
        base_runs = base_maps.get(key)
        if not base_runs:
            continue
        group, base_slowest = judge(base_runs, args.perfect, args.little, args.lot)
        runs = []
        for run, srow in sorted(scaled_runs.items()):
            brow = base_runs.get(run)
            if not brow or brow['identity'] != srow['identity'] or srow['status'] not in ('ok', 'fail_fps') or \
                    brow['status'] not in ('ok', 'fail_fps'):
                continue
            bg, sg = gpu_ms(brow), gpu_ms(srow)
            share = fill_share(brow, srow, scale)
            runs.append((run, number(brow, 'avg_fps', 0), number(srow, 'avg_fps', 0), bg, sg, share))
        if not runs:
            continue
        slow = min(runs, key=lambda r: r[1])
        shares = [r[5] for r in runs if r[5] is not None]
        share = sum(shares) / len(shares) if shares else None
        verdict = 'unknown' if share is None else 'fill-limited' if share >= 0.6 else \
            'partly fill-limited' if share >= 0.3 else 'not fill-limited'
        reaches = slow[1] < args.perfect <= slow[2]
        lines.append({'name': base_slowest['name'] if base_slowest else key, 'key': key, 'group': group,
                      'verdict': verdict, 'share': share, 'reaches': reaches, 'runs': runs,
                      'gain': slow[2] - slow[1]})
    lines.sort(key=lambda m: -m['gain'])
    print('\nResolution scaling test: res_scale %d (%.0f%% of the pixels), %d maps in both' % (scale, pixels * 100,
                                                                                         len(lines)))
    for m in lines:
        print('  %s  [%s, %s%s]' % (m['name'], m['verdict'],
                                     'share %.2f' % m['share'] if m['share'] is not None else 'no GPU data',
                                     ', REACHES %g fps' % args.perfect if m['reaches'] else ''))
        for run, bf, sf, bg, sg, sh in m['runs']:
            print('      %s: %.1f -> %.1f fps; graphics chip %s -> %s ms a frame' % (
                RUN_NAMES[run], bf, sf, '%.1f' % bg if bg else '?', '%.1f' % sg if sg is not None else '?'))
    slow = [m for m in lines if m['group'] in ('little', 'lot', 'never')]
    print('\n%d of the %d maps under %g fps reach it at res_scale %d; %d are fill-limited, %d partly, %d not'
          % (sum(m['reaches'] for m in slow), len(slow), args.perfect, scale,
             sum(m['verdict'] == 'fill-limited' for m in slow), sum(m['verdict'] == 'partly fill-limited' for m in slow),
             sum(m['verdict'] == 'not fill-limited' for m in slow)))
    if args.compare_csv:
        with open(args.compare_csv, 'w', encoding='utf-8', newline='') as f:
            w = csv.writer(f)
            w.writerow(['name', 'key', 'group', 'verdict', 'fill_share', 'reaches', 'run', 'fps', 'fps_scaled',
                        'gpu_ms', 'gpu_ms_scaled'])
            for m in lines:
                for run, bf, sf, bg, sg, sh in m['runs']:
                    w.writerow([m['name'], m['key'], m['group'], m['verdict'],
                                '%.2f' % m['share'] if m['share'] is not None else '', 'yes' if m['reaches'] else '',
                                RUN_NAMES[run], bf, sf, '%.2f' % bg if bg else '', '%.2f' % sg if sg is not None else ''])
    return 0


def library_items(path):
    with open(path, encoding='utf-8') as f:
        data = json.load(f)
    items = data.get('items', []) if isinstance(data, dict) else data
    return [item for item in items if item.get('item') and item.get('state', '') != 'remove']


def write_library_list(library, path):
    items = library_items(library)
    with open(path, 'w', encoding='ascii', errors='replace', newline='') as f:
        for item in items:
            f.write('+Library/%s\r\n' % item['item'])
    print('wrote %s: %d library maps (copy it to the Xbox as perf\\sweep_list.txt)' % (path, len(items)))


def library_verdicts(maps, args):
    rows = []
    # (the same folder in two groups is two versions of one map, and an Xbox has one of them: one line for both)
    groups = {}
    for item in library_items(args.library):
        groups.setdefault(item['item'].lower(), []).append(item.get('group', ''))
    seen = set()
    for item in library_items(args.library):
        name = item['item']
        if name.lower() in seen:
            continue
        seen.add(name.lower())
        item = dict(item, group='/'.join(g for g in groups[name.lower()] if g))
        runs = maps.get(('+library/' + name).lower()) or maps.get(('+' + name).lower())
        fps = {}
        verdict, why = 'not tested', 'no results (not downloaded on this Xbox, or the pass hasn\'t reached it)'
        if runs:
            for run, row in runs.items():
                if row['status'] in ('ok', 'fail_fps') and number(row, 'avg_fps') is not None:
                    fps[run] = number(row, 'avg_fps')
            broken = [(run, row['status']) for run, row in sorted(runs.items())
                      if row['status'] in ('crashed', 'hung', 'bad_map', 'no_start')]
            first = fps.get(0, fps.get(3))
            if broken:
                verdict, why = 'REMOVE', '%s (%s)' % (broken[0][1].replace('_', ' '), RUN_NAMES.get(broken[0][0], 'run %d' % broken[0][0]))
            elif first is not None and first < args.remove_fps:
                verdict, why = 'REMOVE', '%.1f fps with no bots' % first
            elif fps.get(1) is not None and fps[1] < args.check_fps:
                verdict, why = 'CHECK', '%.1f fps with 4 bots' % fps[1]
            elif first is None or (0 in fps and 3 not in runs and 1 not in runs):
                verdict, why = 'not tested', 'only partly measured so far'
            else:
                verdict, why = 'KEEP', ''
                if fps.get(2) is not None and fps[2] < args.check_fps:
                    why = 'slow with a full game of bots (%.1f fps)' % fps[2]
        rows.append((verdict, name, item.get('group', ''), why,
                     ' / '.join('%s %.1f' % (RUN_NAMES.get(r, 'run %d' % r), fps[r]) for r in sorted(fps) if r <= 3)))
    order = {'REMOVE': 0, 'CHECK': 1, 'KEEP': 2, 'not tested': 3}
    rows.sort(key=lambda r: (order[r[0]], r[1].lower()))
    out = []
    counts = {}
    for verdict, name, group, why, line in rows:
        counts[verdict] = counts.get(verdict, 0) + 1
    out.append('Map library: %d maps. Take out %d, look at %d, fine %d, not tested %d (remove under %.0f fps with no '
               'bots, look at under %.0f fps with 4 bots)' % (len(rows), counts.get('REMOVE', 0), counts.get('CHECK', 0),
               counts.get('KEEP', 0), counts.get('not tested', 0), args.remove_fps, args.check_fps))
    current = None
    for verdict, name, group, why, line in rows:
        if verdict != current:
            out.append('')
            out.append({'REMOVE': '== Take out of the library', 'CHECK': '== Look at', 'KEEP': '== Fine',
                        'not tested': '== Not tested'}[verdict])
            current = verdict
        out.append('  %-24s %-14s %s%s' % (name, group, why, ('   [' + line + ']') if line else ''))
    text = '\n'.join(out)
    print(text)
    if args.library_out:
        with open(args.library_out, 'w', encoding='utf-8', newline='\r\n') as f:
            f.write(text + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='*', help="perf\\sweep.csv files from the Xbox")
    parser.add_argument('--library', help="the map library's library.json: sort its maps by the files' results")
    parser.add_argument('--library-list', help="write a sweep_list.txt of the library's maps (with --library)")
    parser.add_argument('--library-out', help="write the library's verdicts to this text file too")
    parser.add_argument('--remove-fps', type=float, default=20.0, help="library: take out under this with no bots")
    parser.add_argument('--check-fps', type=float, default=22.0, help="library: look at under this with 4 bots")
    parser.add_argument('--summary', help='write a CSV with one row a map')
    parser.add_argument('--html', help='write an HTML page of the report')
    parser.add_argument('--perfect', type=float, default=29.0)
    parser.add_argument('--little', type=float, default=22.0)
    parser.add_argument('--lot', type=float, default=15.0)
    parser.add_argument('--write-list', help='write sweep_list.txt: the maps for a res_scale pass')
    parser.add_argument('--list-gpu', type=float, default=80.0, help="the list's graphics chip busy line (percent)")
    parser.add_argument('--compare', nargs='+', help="a res_scale pass's sweep_rNN.csv: compared with the files")
    parser.add_argument('--compare-csv', help="write the comparison as a CSV")
    parser.add_argument('--scale', type=int, help="the compared pass's res_scale, when its file doesn't say")
    args = parser.parse_args()
    if args.library_list:
        if not args.library:
            parser.error('--library-list needs --library library.json')
        write_library_list(args.library, args.library_list)
        if not args.files:
            return 0
    if not args.files:
        parser.error('give the perf\\sweep CSV files to read')
    rows = read_rows(args.files)
    maps = maps_from(rows)
    if args.library:
        library_verdicts(maps, args)
        return 0
    lines = summary_rows(maps, args.perfect, args.little, args.lot, maps_from(rows, before=True))
    text_report(lines, args)
    if args.write_list:
        write_list(maps, args.write_list, args)
    if args.compare:
        return compare(maps, args.compare, args)
    if args.summary:
        with open(args.summary, 'w', encoding='utf-8', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=list(lines[0].keys()) if lines else ['group'])
            writer.writeheader()
            writer.writerows(lines)
    if args.html:
        html_report(lines, args.html, args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
