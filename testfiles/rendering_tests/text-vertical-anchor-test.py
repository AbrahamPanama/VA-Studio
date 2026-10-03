#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check text-anchor against actual shaped advances, with no raster reference.

Run with the Inkscape executable as the only argument. The caller must provide
isolated INKSCAPE_PROFILE_DIR and XDG_CACHE_HOME directories and preserve stdout.
"""
import math
import os
from pathlib import Path
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: text-vertical-anchor-test.py INKSCAPE')
    for key in ('INKSCAPE_PROFILE_DIR', 'XDG_CACHE_HOME'):
        if not os.environ.get(key):
            raise SystemExit(f'{key} must name an isolated diagnostic directory')
    fixtures = Path(__file__).resolve().parent
    env = dict(os.environ, INKSCAPE_FONTCONFIG=str(fixtures / 'fonts/isolated.conf'),
               LC_NUMERIC='C')
    command = [sys.argv[1], '--query-all', str(fixtures / 'text-vertical-anchor.svg')]
    print('Command:', command, flush=True)
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    if result.returncode:
        raise SystemExit(f'Inkscape query failed: {result.returncode}')
    boxes = {}
    for line in result.stdout.splitlines():
        fields = line.split(',')
        if len(fields) != 5:
            raise SystemExit(f'Malformed query row: {line!r}')
        if fields[0] in boxes:
            raise SystemExit(f'Duplicate object: {fields[0]}')
        values = tuple(map(float, fields[1:]))
        if not all(map(math.isfinite, values)):
            raise SystemExit(f'Nonfinite geometry: {line!r}')
        boxes[fields[0]] = values
    failures = []
    for stem in ('horizontal', 'mixed', 'sideways', 'upright-lr', 'upright-rl',
                 'combining', 'spans', 'vertical-metrics'):
        axis = 0 if stem == 'horizontal' else 1
        one, two, middle, end = (boxes[f'{stem}-{suffix}'] for suffix in ('one', 'two', 'middle', 'end'))
        advance = two[axis + 2] - one[axis + 2]
        if advance <= 0:
            failures.append(f'{stem}: no positive glyph advance ({advance})')
        # Repeated identical glyphs expose advance through their ink bounds.
        # Two glyphs shift one advance for middle anchoring, two for end.
        for label, box, factor in (('middle', middle, 1), ('end', end, 2)):
            shift = two[axis] - box[axis]
            expected = factor * advance
            passed = abs(shift - expected) <= 0.001  # --query-all decimal precision
            print(f'{stem}/{label}: shift={shift:.6f}, expected={expected:.6f}: '
                  f'{"PASS" if passed else "FAIL"}')
            if not passed:
                failures.append(f'{stem}/{label}: measured and positioned advances differ')
            for coordinate in (1 - axis, 2, 3):
                if abs(two[coordinate] - box[coordinate]) > 0.001:
                    failures.append(f'{stem}/{label}: anchoring changed glyph geometry')
    if failures:
        raise SystemExit('\n'.join(failures))
    print('PASS: all 16 anchor checks and their geometry controls')


if __name__ == '__main__':
    main()
