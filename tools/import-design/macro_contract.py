#!/usr/bin/env python3
"""Macro-contract gate: every macro bound exactly once, nothing invented.

Three distinct failures, each of which ships a plugin that looks finished:

  unbound    a macro no control drives -- the parameter exists and nothing
             on the panel can move it
  duplicate  two controls driving one macro -- they fight, and whichever
             the user touches second wins
  invented   a control bound to a macro the DSP does not have -- moves
             nothing at all

None of these are visible in a render, and none are caught by pixel-parity or
by the component contract. A panel can be pixel-perfect, use only real
components, and still control nothing.
"""
from __future__ import annotations

import re
from collections import Counter

# data-pulp-param / -meter / -action, single or double quoted.
BINDING = re.compile(r'data-pulp-(param|meter|action)\s*=\s*["\']([^"\']*)["\']')


def bindings(html: str) -> list[tuple[str, str]]:
    """Return [(channel, macro)] in document order."""
    return [(channel, value.strip()) for channel, value in BINDING.findall(html)]


def check_macros(html: str, declared: dict[str, list[str]]):
    """Compare authored bindings against the macros the DSP actually exposes.

    `declared` maps channel -> macro names, e.g.
        {"param": ["drive.amount", "mix.wet"], "meter": ["out.peak"]}

    Returns (unbound, duplicated, invented), each a list of (channel, macro).
    """
    found = bindings(html)
    counts = Counter(found)

    # An empty value is not a binding at all -- it is a control that looks
    # bound to every gate that only asks "is the attribute present?".
    invented = [(c, m) for (c, m) in counts if not m or m not in declared.get(c, [])]
    duplicated = [pair for pair, n in counts.items() if n > 1 and pair not in invented]
    bound = {pair for pair in counts if pair not in invented}
    unbound = [(c, m) for c, names in declared.items() for m in names
               if (c, m) not in bound]

    return sorted(unbound), sorted(duplicated), sorted(invented)


def describe(unbound, duplicated, invented) -> str:
    out = []
    for c, m in invented:
        label = f"'{m}'" if m else "an empty value"
        out.append(f"  bound to {label} ({c}), which the DSP does not expose")
    for c, m in duplicated:
        out.append(f"  '{m}' ({c}) is driven by more than one control — they fight")
    for c, m in unbound:
        out.append(f"  '{m}' ({c}) has no control — nothing on the panel moves it")
    return "\n".join(out)


if __name__ == "__main__":
    failures = []

    def expect(name, cond, detail=""):
        print(f"  {'ok  ' if cond else 'FAIL'} {name}" + ("" if cond else f" {detail}"))
        if not cond:
            failures.append(name)

    declared = {"param": ["drive.amount", "tone.tilt", "mix.wet"],
                "meter": ["out.peak"]}

    good = ('<div class="pulp-knob" data-pulp-param="drive.amount"></div>'
            '<div class="pulp-knob" data-pulp-param="tone.tilt"></div>'
            '<div class="pulp-knob" data-pulp-param="mix.wet"></div>'
            '<div class="pulp-meter" data-pulp-meter="out.peak"></div>')
    u, d, i = check_macros(good, declared)
    expect("a complete panel passes", not u and not d and not i, (u, d, i))

    # Each failure in isolation, so a fix to one cannot mask another.
    u, d, i = check_macros(good.replace(
        '<div class="pulp-knob" data-pulp-param="mix.wet"></div>', ''), declared)
    expect("an unbound macro fails", u == [("param", "mix.wet")], u)

    u, d, i = check_macros(good.replace(
        'data-pulp-param="tone.tilt"', 'data-pulp-param="drive.amount"'), declared)
    expect("a duplicate binding fails", d == [("param", "drive.amount")], d)
    expect("the duplicate also leaves one unbound", u == [("param", "tone.tilt")], u)

    u, d, i = check_macros(good.replace(
        'data-pulp-param="mix.wet"', 'data-pulp-param="mix.wetness"'), declared)
    expect("an invented macro fails", i == [("param", "mix.wetness")], i)

    # An empty value is the dangerous one: the attribute is present, so any
    # gate that only checks presence calls it bound.
    u, d, i = check_macros(good.replace('data-pulp-param="mix.wet"',
                                        'data-pulp-param=""'), declared)
    expect("an empty binding is not 'bound'", i == [("param", "")] and ("param", "mix.wet") in u,
           (i, u))

    # Right name, wrong channel -- a meter cannot drive a parameter.
    u, d, i = check_macros(good.replace('data-pulp-meter="out.peak"',
                                        'data-pulp-param="out.peak"'), declared)
    expect("channel is part of the identity", i == [("param", "out.peak")], i)

    expect("single quotes parse", bindings("<div data-pulp-param='a.b'>") == [("param", "a.b")])

    print(f"\n{'FAILED: ' + ', '.join(failures) if failures else 'all checks passed'}")
    raise SystemExit(1 if failures else 0)
