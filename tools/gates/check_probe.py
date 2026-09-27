#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Model-chain gate without a game server (Phase C group 5): the model probe
(`?modelprobe=N`, custom_draw.cpp) draws N instances of a .gr2 model on
the login screen - our reader, pose, skinning and custom draw, nothing of
the game's own drawing - and with `?probehash=F` prints an FNV-1a hash of
the canvas after the F-th probe frame (the frame is cleared first, because
the login screen behind it is not deterministic).

  python tools/gates/check_probe.py --record [--instances 20] [--frame 200]
  python tools/gates/check_probe.py --compare

The hash is a function of the model file, N, F and OUR code only: three
runs in a row gave the same hash. `--record` runs twice and
refuses to record when the two runs differ. Recorded as
tools/gates/probe_baseline.json; `--compare` fails when the hash, the
instance/mesh line or the canvas size differ, or when the probe did not
load the model.
"""
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import PORT, ROOT, tool  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, 'probe_baseline.json')
HARNESS = os.path.join(ROOT, 'tools', 'browser')
HTTP_PORT = 8731


def run_once(instances, frame, seconds):
    """Serves build/port over HTTP, runs the model probe page in headless Edge
    and returns {instances, frame, hash, canvas, built} from its console.
    """
    # only the client's files, never build/port/private (tools/site_server.py)
    server = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'site_server.py'), str(HTTP_PORT)],
                              cwd=PORT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.5)
        url = ('http://127.0.0.1:%d/client_measure.html?run=1&bridge=ws://127.0.0.1:1&modelprobe=%d&probehash=%d'
               % (HTTP_PORT, instances, frame))
        r = subprocess.run([tool('node'), 'measure.js', url, str(seconds * 1000)], cwd=HARNESS,
                           capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=seconds + 240)
        out = r.stdout + r.stderr
    finally:
        server.kill()
    console = out.split('=== PAGE ===')[0]
    m = re.search(r'canvas hash after (\d+) frames = ([0-9a-f]{16}) \((\d+)x(\d+)\)', console)
    # `built 20 instances of 3 meshes`
    inst = re.search(r'built (\d+) instances of (\d+) meshes', console)
    return {'instances': instances, 'frame': frame,
            'hash': m.group(2) if m else None,
            'canvas': '%sx%s' % (m.group(3), m.group(4)) if m else None,
            'built': '%s x %s' % (inst.group(1), inst.group(2)) if inst else None}


def main(argv):
    """`--record` (two identical runs required) or `--compare` of the probe's
    canvas hash, canvas size and built counts. 1 on a difference.
    """
    def arg(name, default):
        """Integer value of option `name`, or `default`."""
        return int(argv[argv.index(name) + 1]) if name in argv else default
    seconds = arg('--seconds', 30)
    if '--record' in argv:
        instances, frame = arg('--instances', 20), arg('--frame', 200)
        a = run_once(instances, frame, seconds)
        b = run_once(instances, frame, seconds)
        if not a['hash'] or a != b:
            print('NOT recorded: two runs differ or no hash: %r vs %r' % (a, b))
            return 1
        with open(BASELINE, 'w', encoding='utf-8') as f:
            json.dump(a, f, indent=1, sort_keys=True)
        print('recorded: hash %s after %d frames, %s instances, built %s, canvas %s' %
              (a['hash'], a['frame'], a['instances'], a['built'], a['canvas']))
        return 0
    if '--compare' in argv:
        base = json.load(open(BASELINE, encoding='utf-8'))
        cur = run_once(base['instances'], base['frame'], seconds)
        fails = [k for k in ('hash', 'canvas', 'built') if cur.get(k) != base.get(k)]
        for k in fails:
            print('FAIL %s: %r vs baseline %r' % (k, cur.get(k), base.get(k)))
        print('probe vs baseline: %d failures (hash %s, built %s)' % (len(fails), cur.get('hash'), cur.get('built')))
        return 1 if fails else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
