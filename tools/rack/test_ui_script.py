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

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.normpath(os.path.join(
    HERE, "..", "..", "examples", "forge-modular", "app", "ui", "main.js"))
# This checkout, not a hard-coded sibling. It pointed at
# /Volumes/Workshop/Code/pulp, so every bridge name was validated against a
# DIFFERENT tree: a function added here read as missing, and one deleted here
# read as present. The fallback is kept for a checkout without core/view, but
# the local tree wins.
PULP = REPO if os.path.isdir(os.path.join(REPO, "core", "view", "src")) \
       else "/Volumes/Workshop/Code/pulp"

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
    # A flex key the bridge does not know is silently ignored, so the layout is
    # wrong and nothing complains. Thirteen calls used "flex_direction" -- which
    # does not exist; the key is "direction" -- and every one of them did
    # nothing for as long as it took a screenshot to notice.
    layout_api = os.path.join(REPO, "core", "view", "src", "widget_bridge",
                              "layout_api.cpp")
    if os.path.exists(layout_api):
        known = set(re.findall(r'key == "([a-z_]+)"', open(layout_api).read()))
        if known:
            code = "\n".join(ln.split("//")[0] for ln in text.split("\n"))
            used = set(re.findall(r'setFlex\(\s*[^,]+,\s*"([a-z_]+)"', code))
            unknown = sorted(used - known)
            if unknown:
                for k in unknown[:6]:
                    print(f"  WRONG  setFlex key '{k}' is not one the bridge accepts")
                bad += 1
            else:
                print(f"  ok     every setFlex key is one of the bridge's {len(known)}")

    # Anything that paints like a control must BE one. This was missed three
    # times running -- the composer buttons, then the mode tabs, then the shelf
    # tabs -- each fixed in isolation without asking what else was a box
    # pretending to be a button. A row cannot take a click, show hover or hold
    # focus, so an id in these families has to come from createToggleButton.
    CONTROL_PREFIXES = ("btn-", "tab-", "shelf-patches", "shelf-modules", "rail-")
    toggles = set(re.findall(r'createToggleButton\(\s*"([^"]+)"', text))
    for helper in LOCAL:
        if re.search(rf"function {helper}\s*\(\s*id\b", text) and \
           re.search(rf"function {helper}\b[^{{]*{{[^}}]*createToggleButton", text, re.S):
            toggles |= set(re.findall(rf'\b{helper}\(\s*"([^"]+)"', text))
    rows = set(re.findall(r'create(?:Row|Col|Panel)\(\s*"([^"]+)"', text))
    for helper in LOCAL:
        if re.search(rf"function {helper}\s*\(\s*id\b", text) and \
           not re.search(rf"function {helper}\b[^{{]*{{[^}}]*createToggleButton", text, re.S):
            rows |= set(re.findall(rf'\b{helper}\(\s*"([^"]+)"', text))
    # rail-brand is the logo tile, which Forge draws as an accent square rather
    # than a button. An exception with a reason, not a hole in the rule.
    NOT_CONTROLS = {"rail-brand"}
    fake = sorted(r for r in rows - toggles - NOT_CONTROLS
                  if any(r.startswith(pre) for pre in CONTROL_PREFIXES)
                  and not r.endswith(("-glyph", "-label", "-text", "-name",
                                      "-sub", "-mark", "-gap", "-list", "-empty")))
    if fake:
        for f in fake[:6]:
            print(f"  WRONG  '{f}' paints like a control but is not a ToggleButton")
        bad += 1
    else:
        print(f"  ok     all {len(toggles)} control-shaped ids are real controls")

    # Not every set*() call addresses a widget. The shell's own state setters --
    # setMode("patch"), setRackStatus(...) -- take a value in the first slot, so
    # a naive scan reported "patch" as an unstyled widget. Scanning by shape
    # rather than by name is what makes this cheap; the exclusion list is what
    # keeps it honest.
    NOT_WIDGET_SETTERS = ("setMode", "setRackStatus", "setFidelity")
    styled = set()
    for name, target in re.findall(r'(set[A-Za-z]+)\(\s*"([^"]+)"', text):
        if name not in NOT_WIDGET_SETTERS:
            styled.add(target)
    missing = sorted(s for s in styled - created
                     if not any(s.startswith(c) for c in created))
    if missing:
        for m in missing[:6]:
            print(f"  WRONG  styles '{m}', which is never created")
        bad += 1
    else:
        print(f"  ok     all {len(styled)} styled ids are created "
              f"({len(created)} literal, {len(dynamic)} built)")

    # createLabel is (id, text, parent) -- three arguments. Called with two,
    # the parent lands in the TEXT slot and the label is never attached, so it
    # renders unparented at the top level. Every label in this shell did that,
    # and the earlier checks passed it: the function existed and the id was
    # created, so nothing looked wrong until a screenshot was taken.
    two_arg = re.findall(r'createLabel\(\s*[^,()]+(?:\+[^,()]+)?\s*,\s*[^,()]+\s*\)', text)
    if two_arg:
        for t in two_arg[:5]:
            print(f"  WRONG  {t.strip()} — createLabel takes (id, text, parent); "
                  f"with two arguments the parent becomes the text")
        bad += 1
    else:
        print("  ok     every createLabel passes id, text and parent")

    # A widget with no parent is created and never attached, so it renders at
    # the top level and the layout collapses into a vertical stack. The first
    # headless render of this shell looked exactly like that, and this check
    # passed it -- functions existed, ids were created, nothing was assembled.
    unparented = []
    for m in re.finditer(r'create[A-Za-z]+\(\s*"([^"]+)"\s*([,)])', text):
        wid, nxt = m.group(1), m.group(2)
        if nxt == ")" and wid != "root":
            unparented.append(wid)
    # A helper that parents through a variable is fine; only literals with no
    # second argument at all are a problem.
    if unparented:
        for u in sorted(set(unparented))[:6]:
            print(f"  WRONG  '{u}' is created with no parent — it will render "
                  f"at the top level instead of inside the layout")
        bad += 1
    else:
        print("  ok     every widget but the root is parented")

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
