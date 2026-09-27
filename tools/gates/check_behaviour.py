#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Behaviour gate without a game server: drive the client headless to the
login screen and compare what it did with a recorded baseline.

  python tools/gates/check_behaviour.py --record [--seconds 45]
  python tools/gates/check_behaviour.py --compare [--seconds 45]
  python tools/gates/check_behaviour.py --calibrate      # two runs, report what is stable

Runs tools/browser/measure.js (Edge headless, SwiftShader) against
build/port/client_measure.html served from build/port with `?bridge=ws://127.0.0.1:1`
(a dead bridge: the baseline was recorded without a game server, and with the
user's bridge running the client logs in and enters the world - a different
run), then extracts:
  * js_errors     - [exception] lines and ReferenceError/TypeError/abort in the console
  * frame_blocks  - frame-statistics blocks printed by the client (loop alive);
                  one block per 60 frames OR 5 s, whichever first (frame_stats.cpp),
                  so above 12 FPS the count follows the MONITOR
                  REFRESH RATE (78 in 40 s at 120 Hz, 39 at 60 Hz);
                  --record again after a display change, HEAD before the
                  change measured first (the A/B that proved it)
  * webgl         - context present
  * syserr        - /syserr.txt with time stamps blanked
  * missing       - /missing.txt (missing corpus files) verbatim
  * bridge        - the network layer reached the WebSocket bridge (refused is fine)
  * screenshot    - sha256 of screenshot.png (only meaningful if --calibrate says it is stable)
  * stubs_hit     - set of stubs reported "FIRST HIT" (first call of an unimplemented API)
  * rejected      - a rejected promise captured by the measurement page
Recorded as tools/gates/behaviour_baseline.json. `--compare` fails on: any
JS error, fewer frame blocks than 80% of the baseline, changed syserr or
missing, a different set of stubs hit, a rejected promise, WebGL missing,
bridge not reached, screenshot hash changed (when the baseline marks it
stable). The stubborn part - sound, IME, the world - is
NOT covered here; that needs the game server (plan §4 pkt 4).
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import ROOT, PORT, tool, read_text  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, 'behaviour_baseline.json')
HARNESS = os.path.join(ROOT, 'tools', 'browser')
HTTP_PORT = 8731


def run_once(seconds):
    """Serves build/port over HTTP, runs the client page (`?run=1&missing=1`) in
    headless Edge for `seconds` and returns the measures `extract` takes.
    """
    # only the client's files, never build/port/private (tools/site_server.py)
    server = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'site_server.py'), str(HTTP_PORT)],
                              cwd=PORT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.5)
        url = 'http://127.0.0.1:%d/client_measure.html?run=1&missing=1&bridge=ws://127.0.0.1:1' % HTTP_PORT
        shot = os.path.join(HARNESS, 'screenshot.png')
        if os.path.exists(shot):
            os.remove(shot)
        r = subprocess.run([tool('node'), 'measure.js', url, str(seconds * 1000)], cwd=HARNESS,
                           capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=seconds + 240)
        out = r.stdout + r.stderr
    finally:
        server.kill()
    return extract(out, shot)


def extract(out, shot):
    """Measures from the harness output: JS errors, frame-statistics blocks,
    stubs hit, rejected promises, WebGL, network bridge, `syserr.txt`,
    `missing.txt` and the screenshot.
    """
    # measure.js prints the console live (`[console] ...`) and then dumps the
    # last 8000 chars of the page text again under `=== PAGE ===`; count
    # only the live console lines, or every block near the end is counted
    # twice (reviewer review: 91 = 78 real + 13 duplicates)
    console = out.split('=== PAGE ===')[0]
    lines = console.splitlines()
    errors = [l for l in lines if l.startswith('[exception]') or re.search(r'ReferenceError|TypeError|RuntimeError|Aborted\(|abort\(', l)]
    # the client prints one statistics block per 0.5 s while the loop runs
    frame_blocks = sum(1 for l in lines if re.search(r'\bframe: \d+ frames in \d+ ms', l))
    # every stub hit the first time is a behaviour change (stubs.cpp)
    stubs_hit = sorted(set(re.findall(r'stub FIRST HIT: (\S+)', console)))
    m = re.search(r'=== REJECTED PROMISE ===\n(.*?)(?:\n=== |\Z)', out, re.S)
    rejected = (m.group(1) if m else '').strip()
    webgl = 'WebGL: YES' in out
    # `m2w network: bridge ...` (the log line of m2w.bridgeStart)
    bridge = bool(re.search(r'network: .*bridge|/to/\d', out))
    m = re.search(r'=== syserr\.txt ===\n(.*?)(?:\n=== |\Z)', out, re.S)
    syserr = m.group(1) if m else ''
    syserr = re.sub(r'\d{4} \d\d:\d\d:\d{5}', '<time>', syserr).strip()
    m = re.search(r'=== missing\.txt ===\n(.*?)(?:\n=== |\Z)', out, re.S)
    missing = (m.group(1) if m else '').strip()
    shot_hash = hashlib.sha256(open(shot, 'rb').read()).hexdigest() if os.path.exists(shot) else None
    m = re.search(r'successful reads: (\d+)', out)
    reads = int(m.group(1)) if m else 0
    return {'js_errors': errors[:20], 'frame_blocks': frame_blocks, 'webgl': webgl, 'bridge': bridge,
            'syserr': syserr, 'missing': missing, 'screenshot': shot_hash, 'reads': reads,
            'stubs_hit': stubs_hit, 'rejected': rejected}


def main(argv):
    """`--calibrate` (two runs, what is stable), `--record` or `--compare` with
    the baseline (errors, frame blocks >= 80 %, stubs, bridge...). 1 on a
    failure.
    """
    seconds = int(argv[argv.index('--seconds') + 1]) if '--seconds' in argv else 45
    if '--calibrate' in argv:
        a, b = run_once(seconds), run_once(seconds)
        stable = {k: a[k] == b[k] for k in ('webgl', 'bridge', 'syserr', 'missing', 'screenshot')}
        print('run 1: frame_blocks %d reads %d errors %d | run 2: frame_blocks %d reads %d errors %d' %
              (a['frame_blocks'], a['reads'], len(a['js_errors']), b['frame_blocks'], b['reads'], len(b['js_errors'])))
        print('stable between runs: ' + ', '.join('%s=%s' % kv for kv in stable.items()))
        return 0
    cur = run_once(seconds)
    if '--record' in argv:
        cur['screenshot_stable'] = '--screenshot-stable' in argv
        with open(BASELINE, 'w', encoding='utf-8') as f:
            json.dump(cur, f, indent=1, sort_keys=True)
        print('recorded: frame_blocks %d, errors %d, webgl %s, bridge %s, missing %d B, syserr %d B, screenshot %s' %
              (cur['frame_blocks'], len(cur['js_errors']), cur['webgl'], cur['bridge'], len(cur['missing']), len(cur['syserr']),
               'stable' if cur['screenshot_stable'] else 'not compared'))
        return 0
    if '--compare' in argv:
        base = json.load(open(BASELINE, encoding='utf-8'))
        fails = []
        if cur['js_errors']:
            fails.append('JS errors: ' + '; '.join(cur['js_errors'][:3]))
        if not cur['webgl']:
            fails.append('no WebGL context')
        if base.get('bridge') and not cur['bridge']:
            fails.append('network layer did not reach the bridge')
        # three identical runs gave exactly the same count, so 80 % leaves
        # margin for a slower machine but still catches a multi-second stall
        if cur['frame_blocks'] < base['frame_blocks'] * 0.8:
            fails.append('frame blocks %d < 80%% of baseline %d (loop stalled?)' % (cur['frame_blocks'], base['frame_blocks']))
        if cur.get('stubs_hit') != base.get('stubs_hit'):
            fails.append('stubs hit differ: %s vs baseline %s' % (cur.get('stubs_hit'), base.get('stubs_hit')))
        if cur.get('rejected'):
            fails.append('rejected promise: ' + cur['rejected'][:120])
        if cur['syserr'] != base['syserr']:
            fails.append('syserr.txt differs')
        if cur['missing'] != base.get('missing'):
            fails.append('missing.txt differs')
        if base.get('screenshot_stable') and cur['screenshot'] != base['screenshot']:
            fails.append('login screen screenshot differs')
        for f in fails:
            print('FAIL ' + f)
        print('behaviour vs baseline: %d failures (frame_blocks %d/%d, errors %d)' %
              (len(fails), cur['frame_blocks'], base['frame_blocks'], len(cur['js_errors'])))
        return 1 if fails else 0
    raise SystemExit(__doc__)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
