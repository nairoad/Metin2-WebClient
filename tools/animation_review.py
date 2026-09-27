# -*- coding: utf-8 -*-
"""Survey of the animations in the corpus: curve shapes, degrees, track group
members, text tracks. A measurement BEFORE writing the sampling - so as to
write only what the data really carries, and to know what is NOT there.

    python tools/animation_review.py [N] [pattern]
"""
import collections
import os
import sys

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), 'tools')
if not os.path.isdir(TOOLS):
    TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import gr2  # noqa: E402

CORPUS = os.path.join(ROOT, 'build', 'port', 'corpus_files')


def file_list(pattern):
    """Paths of the corpus .gr2 files, optionally only those whose path contains
    `pattern` (case-insensitive).
    """
    for directory, _, names in os.walk(CORPUS):
        for n in names:
            if n.lower().endswith('.gr2'):
                p = os.path.join(directory, n)
                if pattern is None or pattern.lower() in p.replace(os.sep, '/').lower():
                    yield p


def field_names(node):
    """The member names of a node's type, as a tuple."""
    return tuple(str(p.name) for p in node.field_list())


def main():
    """`[N] [PATTERN]`: surveys the animations of N .gr2 files - curve shapes
    and degrees, track group/track/animation members, flags, text tracks,
    knot vs control counts - and prints each tally.
    """
    how_many = int(sys.argv[1]) if len(sys.argv) > 1 else 400
    pattern = sys.argv[2] if len(sys.argv) > 2 else None

    shape = collections.Counter()
    degrees_list = collections.Counter()
    group_fields = collections.Counter()
    path_fields = collections.Counter()
    animation_fields = collections.Counter()
    group_flags = collections.Counter()
    groups_per_animation = collections.Counter()
    animations_per_file = collections.Counter()
    textual = collections.Counter()
    texts = collections.Counter()
    dimension = collections.Counter()
    knots_vs_controls = collections.Counter()
    loop_translation = collections.Counter()
    with_animation = without = 0
    errors_found = collections.Counter()
    examples = {}

    for i, p in enumerate(file_list(pattern)):
        if i >= how_many:
            break
        try:
            _, image = gr2.open_file(p)
            k = gr2.root_node(image)
            anim = k['Animations']
        except Exception as e:
            errors_found[type(e).__name__ + ': ' + str(e)[:60]] += 1
            continue
        if not anim:
            without += 1
            continue
        with_animation += 1
        animations_per_file[len(anim)] += 1
        for a in anim:
            animation_fields[field_names(a)] += 1
            groups = a['TrackGroups']
            groups_per_animation[len(groups)] += 1
            for g in groups:
                group_fields[field_names(g)] += 1
                gs = g.lookup(1, 4)
                group_flags[gs.get('AccumulationFlags', gs.get('Flags'))] += 1
                lt = tuple(round(x, 3) for x in gs.get('LoopTranslation', []))
                loop_translation[lt != (0.0, 0.0, 0.0)] += 1
                tt = g['TextTracks']
                textual[len(tt)] += 1
                for t in tt:
                    for e in t['Entries']:
                        texts[e['Text']] += 1
                for t in g['TransformTracks']:
                    path_fields[field_names(t)] += 1
                    for name in ('PositionCurve', 'OrientationCurve',
                                  'ScaleShearCurve'):
                        c = t[name]
                        pk = field_names(c)
                        shape[(name, pk)] += 1
                        if 'Degree' in pk:
                            d = c.lookup(1, 10 ** 6)
                            deg = d['Degree']
                            nk = len(d['Knots'])
                            nc = len(d['Controls'])
                            degrees_list[(name, deg)] += 1
                            if nk:
                                dimension[(name, nc // nk, nc % nk == 0)] += 1
                                knots_vs_controls[(name, deg, 'knots==1')] += (nk == 1)
                            key_name = (name, deg, nk == 0)
                            if key_name not in examples:
                                examples[key_name] = (os.path.relpath(p, CORPUS),
                                                    gs['Name'], t['Name'], nk, nc)

    def print_tree(title, counter, sort=False):
        """Prints one tally under a title, by frequency (or sorted by key)."""
        print('\n' + title + ':')
        elevation = sorted(counter.items(), key=lambda kv: str(kv[0])) if sort \
            else counter.most_common()
        for k, v in elevation:
            print('  %6d %s' % (v, k))

    print('files with an animation: %d, without: %d' % (with_animation, without))
    print_tree('errors', errors_found)
    print_tree('animations per file', animations_per_file, True)
    print_tree('track groups per animation', groups_per_animation, True)
    print_tree('animation fields', animation_fields)
    print_tree('group fields', group_fields)
    print_tree('group flags (AccumulationFlags)', group_flags, True)
    print_tree('LoopTranslation non-zero', loop_translation, True)
    print_tree('text tracks per group', textual, True)
    print_tree('event texts (20 most common)',
           collections.Counter(dict(texts.most_common(20))))
    print_tree('track fields', path_fields)
    print_tree('curve shape', shape)
    print_tree('degrees', degrees_list, True)
    print_tree('dimension (Controls/Knots, divisible)', dimension, True)
    print('\nexamples (curve, degree, empty) -> (file, group, track, knots, controls):')
    for k, v in sorted(examples.items(), key=str):
        print('  %s -> %s' % (k, v))
    return 0


if __name__ == '__main__':
    sys.exit(main())
