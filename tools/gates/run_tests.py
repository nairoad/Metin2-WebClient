#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build and run the layer's unit tests (compat/tests/*_test.cpp) under node.

  python tools/gates/run_tests.py [name ...]     # default: the compat tests
  python tools/gates/run_tests.py --record        # save results as tools/gates/tests_baseline.txt
  python tools/gates/run_tests.py --compare       # exit 1 if any result differs from the baseline

Some tests are STALE (they assert behaviour that later parts changed on
purpose) - they fail today and must keep failing in exactly the same way
until fixed in a separate "behaviour" commit. That is what --record /
--compare enforce: the result line per test (ok/FAIL + failed-assertion
count) is the baseline, not "all green".

The commands come from build/port/README.md (em++ -O1 -Icompat <test>
<compat sources> -o <test>.js && node <test>.js). Tests that need the whole
staged TMP4 tree (eterbase_test, eterpack_test) are not run here -
they are listed in SKIPPED with the reason. Exit code 1 if any test fails.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, PORT, tool  # noqa: E402

# test name -> compat sources it links (relative to the repository root)
TESTS = {
    'd3dx8_math_test': ['compat/win32_compat.cpp'],
    'd3d8_fixedfunc_test': ['compat/d3d8_fixedfunc.cpp', 'compat/win32_compat.cpp'],
    'd3d8_states_test': ['compat/d3d8_states.cpp', 'compat/win32_compat.cpp'],
    'd3d8_fvf_test': ['compat/d3d8_fvf.cpp', 'compat/win32_compat.cpp'],
    'd3d8_image_test': ['compat/d3d8_image.cpp', 'compat/dxt.cpp', 'compat/jpeg_decode.cpp', 'compat/win32_compat.cpp'],
    'd3d8_pixels_test': ['compat/d3d8_pixels.cpp', 'compat/win32_compat.cpp'],
    'platform_codec_test': ['compat/platform_codec.cpp', 'compat/codepages.cpp', 'compat/win32_compat.cpp'],
    'platform_crash_test': ['compat/platform_crash.cpp', 'compat/win32_compat.cpp'],
    'platform_none_test': ['compat/platform_none.cpp', 'compat/cursor_web.cpp', 'compat/input_web.cpp', 'compat/win32_compat.cpp'],
}
# tests written in JavaScript (compat/tests/<name>.js), run by node directly -
# the address options on a real site vs a local page
JS_TESTS = ['runtime_options_test']
SKIPPED = {
    'eterbase_test': 'needs staged EterBase', 'eterpack_test': 'needs staged EterPack',
    # older tests that needed the whole client or a browser were removed.
}


def main(argv):
    """Builds and runs each test of TESTS (or the ones named) with em++ and node;
    `--record` stores the results in tests_baseline.txt, `--compare` checks
    them against it.
    """
    names = [a for a in argv if not a.startswith('-')] or list(TESTS) + JS_TESTS
    empp, node = tool('em++'), tool('node')
    failed, results = [], []
    for name in names:
        if name in JS_TESTS:
            r = subprocess.run([node, os.path.join(ROOT, 'compat', 'tests', name + '.js')],
                               cwd=ROOT, capture_output=True, text=True)
            ok = r.returncode == 0
            if not ok:
                failed.append(name)
            out = r.stdout + r.stderr
            line = '%s %s (%d OK, %d FAIL)' % (name, 'ok' if ok else 'FAIL',
                                               sum(1 for l in out.splitlines() if l.rstrip().endswith('OK')),
                                               sum(1 for l in out.splitlines() if l.rstrip().endswith('FAIL')))
            results.append(line)
            print(line)
            continue
        srcs = TESTS.get(name)
        if srcs is None:
            print('%-24s skipped: %s' % (name, SKIPPED.get(name, 'unknown test')))
            continue
        js = os.path.join(PORT, name + '.js')
        cmd = [empp, '-std=c++17', '-O1', '-w', '-sUSE_LIBJPEG=1', '-Icompat',
               'compat/tests/' + name + '.cpp'] + srcs + ['-o', js]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            failed.append(name)
            errs = [l for l in r.stderr.splitlines()
                    if 'undefined symbol' in l or ('error:' in l and 'wasm-ld' not in l)]
            print('%-24s BUILD FAILED\n%s' % (name, '\n'.join('  ' + e[:160] for e in errs[:10]) or r.stderr[-800:]))
            continue
        r = subprocess.run([node, js], cwd=PORT, capture_output=True, text=True)
        ok = r.returncode == 0
        if not ok:
            failed.append(name)
        out = r.stdout + r.stderr
        bad = sum(1 for l in out.splitlines() if l.rstrip().endswith('FAIL'))
        good = sum(1 for l in out.splitlines() if l.rstrip().endswith('OK'))
        line = '%s %s (%d OK, %d FAIL)' % (name, 'ok' if ok else 'FAIL', good, bad)
        results.append(line)
        print(line)
    print('tests: %d run, %d failed' % (len([n for n in names if n in TESTS or n in JS_TESTS]), len(failed)))
    base_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tests_baseline.txt')
    if '--record' in argv:
        with open(base_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(results) + '\n')
        print('recorded ' + base_path)
        return 0
    if '--compare' in argv:
        with open(base_path, encoding='utf-8') as f:
            base = f.read().splitlines()
        diff = [l for l in results if l not in base] + ['(missing now) ' + l for l in base if l not in results]
        for d in diff:
            print('DIFFERS: ' + d)
        print('compared with baseline: %d differences' % len(diff))
        return 1 if diff else 0
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
