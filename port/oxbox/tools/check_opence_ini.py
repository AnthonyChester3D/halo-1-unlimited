#!/usr/bin/env python3
# port/oxbox/tools/check_opence_ini.py: port/oxbox/opence.ini, the reference of every opence.ini setting, has to list
# every setting the game reads (oxbox_entry.c and oxbox_perf.c read_settings), and nothing else, and has to fit in what
# the game reads of the file. build.sh runs it first: the build stops while they disagree.
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..')
READ_LIMIT = 8192 - 1   # (oxbox_entry.c and oxbox_perf.c read_settings: char text[8192], its last byte the end)


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8', errors='replace') as f:
        return f.read()


def settings_read(path, pattern):
    text = read(path)
    start = text.find('static void read_settings(')
    if start < 0:
        sys.exit('check_opence_ini: no read_settings in %s' % path)
    end = text.find('\n}\n', start)
    return set(re.findall(pattern, text[start:end]))


code = settings_read('port/oxbox/oxbox_entry.c', r'same_text\(name, "([a-z0-9_]+)"\)') | \
    settings_read('port/oxbox/oxbox_perf.c', r'strcmp\(name, "([a-z0-9_]+)"\)')
reference = read('port/oxbox/opence.ini')
listed = set(re.findall(r'^#?([a-z][a-z0-9_]*) *=', reference, re.M))
problems = []
for name in sorted(code - listed):
    problems.append('%s is read by the game but not in port/oxbox/opence.ini: add it, with its default' % name)
for name in sorted(listed - code):
    problems.append('%s is in port/oxbox/opence.ini but the game never reads it' % name)
size = os.path.getsize(os.path.join(ROOT, 'port/oxbox/opence.ini'))   # (as the game reads it: CR LF and all)
if size > READ_LIMIT:
    problems.append('port/oxbox/opence.ini is %d bytes: the game reads only %d' % (size, READ_LIMIT))
if problems:
    sys.exit('\n'.join('check_opence_ini: ' + problem for problem in problems))
print('port/oxbox/opence.ini: all %d settings, %d bytes' % (len(code), size))
