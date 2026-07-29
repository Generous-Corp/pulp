#!/usr/bin/env python3
"""Check the shell's UI script against the functions the bridge actually has.

A scripted UI fails late and quietly: a call to a function the bridge does not
expose throws inside the JS context, at which point the window is already open
and half-built, and the error surfaces as a blank pane rather than as anything
naming the cause. The same is true of a typo in a widget id -- the call
succeeds, styles nothing, and the layout is subtly wrong with no complaint.

So this reads the bridge's registered names out of Pulp's source and holds the
script to them, and checks that every id styled was created.

    python3 tools/rack/test_ui_script.py
"""
from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.normpath(os.path.join(
    HERE, "..", "..", "examples", "forge-modular", "app", "ui", "main.js"))
PULP = "/Volumes/Workshop/Code/pulp"

def local_helpers(text: str) -> set:
    """Functions the script declares itself, which are not bridge calls.

    Derived rather than listed, because a hand-maintained list goes stale the
    moment somebody adds a helper -- and it fails in the direction that looks
    like a real problem, reporting the new helper as a function the bridge does
    not expose.
    """
    return set(re.findall(r"function\s+(\w+)\s*\(", text))


def bridge_names() -> set:
    """Every function the widget bridge registers, from Pulp's own source."""
    names = set()
    src = os.path.join(PULP, "core", "view", "src")
    if not os.path.isdir(src):
        return names
    for root, _dirs, files in os.walk(src):
        for f in files:
            if not f.endswith((".cpp", ".hpp")):
                continue
            try:
                text = open(os.path.join(root, f), errors="replace").read()
            except Exception:
                continue
            # Registered as a quoted name next to its binding.
            names |= set(re.findall(r'"((?:create|set|add|get|bind|on)[A-Za-z]+)"',
                                    text))
    return names


def main():
    if not os.path.exists(SCRIPT):
        print(f"  SKIP   no UI script at {SCRIPT}")
        return 0
    text = open(SCRIPT).read()
    LOCAL = local_helpers(text)

    known = bridge_names()
    if not known:
        print("  SKIP   could not read the bridge's names from Pulp source")
        return 0
    print(f"  ok     bridge exposes {len(known)} functions")

    bad = 0

    called = set(re.findall(r"\b((?:create|set|add|get|bind|on)[A-Za-z]+)\s*\(", text))
    unknown = sorted(c for c in called - known if c not in LOCAL)
    if unknown:
        for u in unknown:
            print(f"  WRONG  calls {u}(), which the bridge does not expose")
        bad += 1
    else:
        print(f"  ok     all {len(called - LOCAL)} bridge calls exist")

    # Every id styled must have been created, or the call quietly does nothing.
    created = set(re.findall(r'create[A-Za-z]+\(\s*"([^"]+)"', text))
    # A helper that creates from a parameter -- tab(id, ...), button(id, ...) --
    # hides the id from a direct scan, so take it from the call site instead.
    # Without this the check reports every button in the shell as uncreated,
    # which is how it behaved on its first run.
    for helper in LOCAL:
        if not re.search(rf"function {helper}\s*\(\s*id\b", text):
            continue
        created |= set(re.findall(rf'\b{helper}\(\s*"([^"]+)"', text))
    # Ids built by concatenation are checked by prefix rather than exactly.
    dynamic = set(re.findall(r'create[A-Za-z]+\(\s*(\w+)\s*\+', text))
    styled = set(re.findall(r'set[A-Za-z]+\(\s*"([^"]+)"', text))
    missing = sorted(s for s in styled - created
                     if not any(s.startswith(c) for c in created))
    if missing:
        for m in missing[:6]:
            print(f"  WRONG  styles '{m}', which is never created")
        bad += 1
    else:
        print(f"  ok     all {len(styled)} styled ids are created "
              f"({len(created)} literal, {len(dynamic)} built)")

    # The two-button send is a decision, not a detail: an inferred intent chip
    # was rejected because it guesses and the user has to notice the guess.
    for needed, why in (("btn-ask", "the Ask button"),
                        ("btn-build", "the Build button")):
        if needed not in created:
            print(f"  WRONG  {why} is missing — send must not be inferred")
            bad += 1

    print(f"\n{'FAIL' if bad else 'ok'}: {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
