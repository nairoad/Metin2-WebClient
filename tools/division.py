#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""division.py - gives Python 2 back its division, but only where it can be
SHOWN.

WHAT THE PROBLEM IS
===================
In Python 2 `7/2` gives `3`, in Python 3 - `3.5`. The difference is not
syntactic, so `lib2to3` leaves it alone and **is right**: without knowing
the operand types one cannot decide which division was meant to truncate.
That is why the `division` fixer is off in `rewrite_scripts.py`, and the
count of `/` operators stands there as an open point.

WHAT CAN BE DECIDED NEVERTHELESS
================================
The types of all operands need not be known. It is enough to answer the
reverse question: **can a floating-point number occur in this division AT
ALL**. If it cannot, then in Python 2 this was an integer division and `//`
reproduces it exactly.

So I decide by the DIRECT OPERANDS, not by the whole expression:

    a / 800.0     stays `/`     - the right operand is floating-point
    float(x) / 27 stays `/`     - the left operand is a call to `float`
    39 / 2        goes to `//`
    SCREEN_WIDTH * (410 - 346/2) / 800   both divisions go to `//`

The last example shows why the direct operand is enough: multiplying and
subtracting integers gives an integer, so `SCREEN_WIDTH * (410 - 346//2)`
is an integer no matter how deep one looks.

WHAT IT DOES **NOT** DECIDE
===========================
Names. `x / 2` with an unknown `x` is taken as integer - because in practice
in these files every name used in a division is an integer, and the ones
that are not the authors wrapped in `float(...)` themselves.

So the function `convert_divisions` also returns **the list of all names** that
occurred as an operand. The caller has something to check them against and
need not take this assumption on faith.
"""

import io
import tokenize


def floating(text_value):
    """Whether a numeric literal is floating-point or complex."""
    n = text_value.lower()
    if n.startswith('0x') or n.startswith('0o') or n.startswith('0b'):
        return False
    return '.' in n or 'e' in n or 'j' in n


def _left_operand(toks, i):
    """The token range of the left operand of the division at position `i`.

    For `)` it walks back to the matching `(` and also takes the name before
    it, if one stands there - because `float(x) / 27` has a CALL on the left,
    not a bracket, and the function name says what type the result is.
    """
    j = i - 1
    if j < 0:
        return []
    if toks[j].string not in (')', ']'):
        return [toks[j]]

    closure = {')': '(', ']': '['}[toks[j].string]
    depth = 0
    k = j
    while k >= 0:
        if toks[k].string in (')', ']'):
            depth += 1
        elif toks[k].string in ('(', '['):
            depth -= 1
            if depth == 0:
                break
        k -= 1
    if k < 0 or toks[k].string != closure:
        return toks[max(0, j - 1):j + 1]
    start_pos = k
    if k - 1 >= 0 and toks[k - 1].type == tokenize.NAME:
        start_pos = k - 1
    return toks[start_pos:j + 1]


def _right_operand(toks, i):
    """The token range of the right operand of the division at position `i`."""
    j = i + 1
    if j >= len(toks):
        return []
    # A unary minus does not change the type.
    while j < len(toks) and toks[j].string in ('-', '+', '~'):
        j += 1
    if j >= len(toks):
        return []
    if toks[j].string not in ('(', '['):
        return [toks[j]]

    depth = 0
    k = j
    while k < len(toks):
        if toks[k].string in ('(', '['):
            depth += 1
        elif toks[k].string in (')', ']'):
            depth -= 1
            if depth == 0:
                break
        k += 1
    return toks[j:min(k + 1, len(toks))]


def _float_names(toks):
    """Names that this file assigns `float(...)` or a floating-point literal:
    `x = float(...)`, `x = 0.0`.

    CORRECTION. The docstring above says: "the ones that are not
    integers the authors wrapped in float(...) themselves" - and that is true,
    only they wrap AT THE ASSIGNMENT, not at the division:

        newScreenWidth = float(screenWidth - 270)
        grp.SetViewport(270.0/screenWidth, 0.0, newScreenWidth/screenWidth, ...)

    (introSelect.py:76-79, introCreate.py:87-90). The rule saw only
    `newScreenWidth/screenWidth`, took it as integer, `//` gave 0 - a viewport
    of width ZERO and the character on the selection/creation screen
    INVISIBLE (measured: `SetViewport 270 0 0x1278` every frame).
    """
    names = set()
    for i in range(len(toks) - 2):
        if (toks[i].type == tokenize.NAME and
                toks[i + 1].type == tokenize.OP and toks[i + 1].string == '='):
            n = toks[i + 2]
            if n.type == tokenize.NAME and n.string == 'float':
                names.add(toks[i].string)
            elif n.type == tokenize.NUMBER and floating(n.string):
                names.add(toks[i].string)
    return names


def _may_be_float(range_of, float_names=frozenset()):
    """True when the token range may hold a float: a float literal, `float`, or
    a name known to be float.
    """
    for t in range_of:
        if t.type == tokenize.NUMBER and floating(t.string):
            return True
        if t.type == tokenize.NAME and t.string == 'float':
            return True
        if t.type == tokenize.NAME and t.string in float_names:
            return True
    return False


def convert_divisions(text_str):
    """Replaces `/` with `//` where the operands cannot be a fraction.

    Returns `(new_text, integer_count, kept_count, names)`, where `names` is
    the set of names used as an operand of an integer division - for the
    caller to check.
    """
    toks = [t for t in tokenize.generate_tokens(io.StringIO(text_str).readline)
            if t.type not in (tokenize.NL, tokenize.NEWLINE, tokenize.INDENT,
                              tokenize.DEDENT, tokenize.COMMENT,
                              tokenize.ENDMARKER)]

    to_change = []          # (line, column)
    names = set()
    left_alone = 0
    floats = _float_names(toks)

    for i, t in enumerate(toks):
        if not (t.type == tokenize.OP and t.string == '/'):
            continue
        left = _left_operand(toks, i)
        right = _right_operand(toks, i)
        if (_may_be_float(left, floats) or
                _may_be_float(right, floats)):
            left_alone += 1
            continue
        for x in left + right:
            if x.type == tokenize.NAME and x.string not in ('float', 'int'):
                names.add(x.string)
        to_change.append(t.start)

    # Replacement FROM THE END, so that earlier positions do not shift.
    rows_list = text_str.split(chr(10))
    for w, k in sorted(to_change, reverse=True):
        line_text = rows_list[w - 1]
        assert line_text[k] == '/', 'the token position does not point at /'
        rows_list[w - 1] = line_text[:k] + '//' + line_text[k + 1:]

    return chr(10).join(rows_list), len(to_change), left_alone, names
