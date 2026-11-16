#!/usr/bin/env python3
"""Summarises run-parity-tests.sh output: per scenario and pairing, how many calls failed
each check, a one-sided Fisher p wherever a CompatImpl pairing failed more often than the
baseline, and p50/p90 of the connection time and of each side's first audio and video."""
import glob
import math
import os
import re
import sys
from collections import defaultdict

PAIRINGS = ('V-V', 'cV-V', 'V-cV')


def parse(path):
    text = open(path, errors='replace').read()
    r = {}
    m = re.search(r'^Call established:\s+yes \(at ([0-9.]+)s\)', text, re.M)
    r['setup'] = float(m.group(1)) if m else None
    m = re.search(r'^Audio heard:\s+caller (\d+) frames \(first at (-?[0-9.]+)s\), '
                  r'callee (\d+) frames \(first at (-?[0-9.]+)s\)', text, re.M)
    if m:
        r['audio caller'], r['audio callee'] = int(m.group(1)), int(m.group(3))
        r['first audio caller'] = float(m.group(2)) if float(m.group(2)) >= 0 else None
        r['first audio callee'] = float(m.group(4)) if float(m.group(4)) >= 0 else None
    m = re.search(r'^Video frames:\s+caller received (\d+), callee received (\d+)', text, re.M)
    if m:
        r['video caller'], r['video callee'] = int(m.group(1)), int(m.group(2))
    m = re.search(r'^Video first frame: caller at (-?[0-9.]+)s, callee at (-?[0-9.]+)s', text, re.M)
    if m:
        r['first video caller'] = float(m.group(1)) if float(m.group(1)) >= 0 else None
        r['first video callee'] = float(m.group(2)) if float(m.group(2)) >= 0 else None
    return r


CHECKS = {
    'not connected': lambda r: r['setup'] is None,
    'caller hears nothing': lambda r: r.get('audio caller', 0) == 0,
    'callee hears nothing': lambda r: r.get('audio callee', 0) == 0,
    'caller gets no video': lambda r: 'video caller' in r and r['video caller'] == 0,
    'callee gets no video': lambda r: 'video callee' in r and r['video callee'] == 0,
}


def fisher_worse(bad_base, n_base, bad, n):
    """P(at least `bad` failures among the n CompatImpl calls | both margins)."""
    total_bad = bad_base + bad
    total = n_base + n
    return sum(math.comb(n, k) * math.comb(n_base, total_bad - k) / math.comb(total, total_bad)
               for k in range(bad, min(n, total_bad) + 1) if total_bad - k <= n_base)


def quantile(values, q):
    values = sorted(v for v in values if v is not None)
    if not values:
        return None
    return values[min(len(values) - 1, max(0, math.ceil(q * len(values)) - 1))]


def timing(rs, key):
    p50, p90 = quantile([r.get(key) for r in rs], .5), quantile([r.get(key) for r in rs], .9)
    return '-' if p50 is None else f'{p50:.2f}/{p90:.2f}'


runs = defaultdict(list)
for path in glob.glob(os.path.join(sys.argv[1], '*.out')):
    scenario, pairing, _ = os.path.basename(path)[:-4].rsplit('.', 2)
    runs[(scenario, pairing)].append(parse(path))

for scenario in sorted({s for s, _ in runs}):
    print(f'== {scenario}')
    base = runs.get((scenario, 'V-V'), [])
    for pairing in PAIRINGS:
        rs = runs.get((scenario, pairing), [])
        if not rs:
            continue
        cells = []
        for name, failed in CHECKS.items():
            if name.endswith('video') and not any('video caller' in r for r in rs):
                continue
            bad = sum(1 for r in rs if failed(r))
            cell = f'{name} {bad}'
            if pairing != 'V-V' and base:
                bad_base = sum(1 for r in base if failed(r))
                if bad * len(base) > bad_base * len(rs):
                    cell += f' (p={fisher_worse(bad_base, len(base), bad, len(rs)):.2f})'
            cells.append(cell)
        print(f'  {pairing:5} n={len(rs):2} | ' + ' | '.join(cells))
        line = f'        p50/p90 s: connected {timing(rs, "setup")}'
        line += f' | caller hears {timing(rs, "first audio caller")} | callee hears {timing(rs, "first audio callee")}'
        if any('video caller' in r for r in rs):
            line += f' | caller sees {timing(rs, "first video caller")} | callee sees {timing(rs, "first video callee")}'
        print(line)
