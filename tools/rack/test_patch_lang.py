#!/usr/bin/env python3
"""What the patch language must guarantee.

The headline property is ROUND TRIP: every patch on this machine rendered to
text and parsed back must be the same patch, compared on port INDICES, not on
port names. Names are what the text carries; indices are what Rack wires. A
comparison by name passes while the cable moves, which is the failure this file
exists to prevent — an early version of this test compared names, reported
123/139, and was hiding exactly that.
"""

from __future__ import annotations

import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import patch as P            # noqa: E402
import patch_lang as L       # noqa: E402

bad = 0


def ok(msg):
    print(f"  ok     {msg}")


def wrong(msg):
    global bad
    print(f"  WRONG  {msg}")
    bad += 1


def signature(doc, inv):
    """A patch reduced to what Rack actually wires AND sets.

    Values are in here because leaving them out is how the first version of
    this test reported 138/138 while every knob position was being dropped.
    The acid patch in the corpus is acid because its resonance is 0.88 and its
    decay 0.07 — same modules, same cables, completely different sound. A
    signature that cannot tell those apart is not a signature of a patch.
    """
    by = {m["id"]: m for m in doc.get("modules", [])}
    cables = []
    for c in doc.get("cables", []):
        s, t = by.get(c.get("outputModuleId")), by.get(c.get("inputModuleId"))
        if not s or not t:
            continue
        cables.append((s["plugin"], s["model"], c.get("outputId"),
                       t["plugin"], t["model"], c.get("inputId")))
    mods = []
    for m in doc.get("modules", []):
        vals = tuple(sorted(
            (int(p.get("id", -1)), round(float(p.get("value", 0.0)), 6))
            for p in (m.get("params") or [])
            if isinstance(p, dict) and "value" in p))
        mods.append((m["plugin"], m["model"], vals))
    return sorted(mods), sorted(cables)


inv = P.inventory()

# ---------------------------------------------------------------- the grammar

CASES = [
    ("a chain infers the middle port",
     "a : ForgeModular/VCO\nb : ForgeModular/VCA\nc : ForgeModular/FOURPOLE\n"
     "a.SAW >> b.IN >> c.IN\n", 2),
    ("a fan-out at the end of a chain",
     "a : ForgeModular/VCO\nb : ForgeModular/VCA\nc : ForgeModular/FOURPOLE\n"
     "a.SAW >> b.IN, c.IN\n", 2),
    ("a comment is not part of the patch",
     "a : ForgeModular/VCO   # the oscillator\nb : ForgeModular/VCA\n"
     "a.SAW >> b.IN   # audio\n", 1),
]
for name, text, want_cables in CASES:
    try:
        doc = L.parse(text, inv)
    except L.PatchLangError as e:
        wrong(f"{name}: {e}")
        continue
    if len(doc["cables"]) != want_cables:
        wrong(f"{name}: {len(doc['cables'])} cables, expected {want_cables}")
    else:
        ok(name)

# Errors must NAME the mistake and what would have been right. A parser that
# says only "invalid" has moved the work back onto the person.
BAD = [
    ("an unknown port is refused by name",
     "a : ForgeModular/VCO\nb : ForgeModular/VCA\na.VOTC >> b.IN\n", "VOTC"),
    ("an undeclared node is refused",
     "a : ForgeModular/VCO\na.SAW >> nowhere.IN\n", "nowhere"),
    ("an unknown module is refused",
     "a : ForgeModular/NoSuchThing\n", "NoSuchThing"),
    ("an ambiguous chain hop must be spelled out",
     "a : ForgeModular/VCO\nb : ForgeModular/VCO\nc : ForgeModular/VCA\n"
     "a.SAW >> b.V/OCT >> c.IN\n", "outputs"),
]
for name, text, needle in BAD:
    try:
        L.parse(text, inv)
        wrong(f"{name}: it was accepted")
    except L.PatchLangError as e:
        if needle.lower() in str(e).lower():
            ok(name)
        else:
            wrong(f"{name}: refused, but the message never says "
                  f"{needle!r}: {e}")

# Repeated port names are real: a DUAL AD has two TRIG inputs, one per channel.
# Resolving both to the first is not a naming nicety — it is a patch wired to
# the wrong channel.
i, o = L._ports(inv, "ForgeModular", "DUALAD")
if i.count("TRIG") >= 2:
    first = L._port_index(i, "TRIG")
    second = L._port_index(i, "TRIG#2")
    if first != second and second >= 0:
        ok("TRIG and TRIG#2 are different inputs on a DUAL AD")
    else:
        wrong(f"TRIG#2 did not select the second input ({first} vs {second})")
    if L.port_label(i, second) == "TRIG#2":
        ok("and the renderer writes the suffix back")
    else:
        wrong(f"the renderer wrote {L.port_label(i, second)!r} for the second TRIG")
else:
    ok("(this pack's DUAL AD has no repeated TRIG; nothing to check)")

# ------------------------------------------------------------- the round trip

d = os.path.expanduser("~/Library/Application Support/Forge Modular/"
                       "examples/forge-modular/patches")
files = sorted(glob.glob(d + "/*.vcv"))
if not files:
    print("  SKIP   no patches on this machine to round-trip "
          "(this is a skip, not a pass)")
else:
    same = diff = 0
    first_bad = None
    for f in files:
        try:
            orig = json.load(open(f))
        except Exception:                                   # noqa: BLE001
            continue
        if not orig.get("modules"):
            continue
        try:
            back = L.parse(L.render(orig, inv), inv)
        except Exception as e:                              # noqa: BLE001
            diff += 1
            first_bad = first_bad or (os.path.basename(f), str(e)[:120])
            continue
        if signature(orig, inv) == signature(back, inv):
            same += 1
        else:
            diff += 1
            first_bad = first_bad or (os.path.basename(f), "port indices differ")
    if diff == 0 and same > 0:
        ok(f"all {same} patches on this machine round-trip, port-index exact")
    else:
        wrong(f"{diff} of {same + diff} patches do not round-trip; "
              f"first: {first_bad}")

# Values survive, stated directly. The corpus check above would catch this
# too, but a failure there says "a patch differs" where this says which
# property was lost.
_probe = ("f : ForgeModular/FOURPOLE\n"
          "f.Cutoff = 0.34\n"
          "f.Resonance = 0.88\n")
try:
    _doc = L.parse(_probe, inv)
    _vals = {int(p["id"]): p["value"] for p in _doc["modules"][0]["params"]}
    if _vals.get(0) == 0.34 and _vals.get(1) == 0.88:
        ok("knob positions parse")
    else:
        wrong(f"knob positions did not parse: {_vals}")
    _again = L.parse(L.render(_doc, inv), inv)
    _v2 = {int(p["id"]): p["value"] for p in _again["modules"][0]["params"]}
    if _v2 == _vals:
        ok("and survive a round trip")
    else:
        wrong(f"knob positions changed through the round trip: {_vals} -> {_v2}")
except L.PatchLangError as e:
    wrong(f"a patch with knob positions did not parse: {e}")


# The advertisement must be a working program.
#
# Two drafts of the module docstring invented port names that do not exist —
# `out.LEFT` when the port is `To "device output 1"`, and `quant.IN` when it is
# `1V/octave pitch`. Both were written by reasoning about what a port is
# probably called instead of reading the inventory, and neither was caught
# because nothing ever ran the example. A language whose own example is a
# syntax error is worse than one with no example, so the example is now
# executed rather than believed.
import re as _re                                            # noqa: E402
_m = _re.search(r"EXAMPLE-BEGIN\n(.*?)\n\s*EXAMPLE-END", L.__doc__ or "", _re.S)
if not _m:
    wrong("the docstring has no EXAMPLE-BEGIN/END block to check")
else:
    _text = "\n".join(l[4:] if l.startswith("    ") else l
                      for l in _m.group(1).splitlines())
    try:
        _d = L.parse(_text, inv)
        if _d["modules"] and _d["cables"]:
            ok(f"the docstring's example parses ({len(_d['modules'])} modules, "
               f"{len(_d['cables'])} cables)")
        else:
            wrong("the docstring's example parses to an empty patch")
    except L.PatchLangError as _e:
        wrong(f"the docstring's example does not parse: {_e}")

# A number the renderer can print, the parser must read. `%g` emits 1e-05 for a
# small value, and the first grammar had no `-` inside the exponent — so the
# language refused its own output. The corpus never caught it because no patch
# held a value that small: a round-trip breaker hiding in the grammar rather
# than in the data.
for _v in ("1e-05", "-2.5e+10", "0.000012", "-0", "3"):
    try:
        _doc = L.parse(f"f : ForgeModular/FOURPOLE\nf.Cutoff = {_v}\n", inv)
        _got = _doc["modules"][0]["params"][0]["value"]
        if abs(_got - float(_v)) < 1e-12:
            ok(f"the grammar reads {_v}")
        else:
            wrong(f"{_v} parsed as {_got}")
    except L.PatchLangError as _e:
        wrong(f"the grammar cannot read {_v}, which its own renderer prints: {_e}")

# Whatever %g prints for any value in the corpus must read back.
_worst = None
for _f in files[:40]:
    try:
        _o = json.load(open(_f))
    except Exception:                                       # noqa: BLE001
        continue
    for _m2 in _o.get("modules", []):
        for _p in (_m2.get("params") or []):
            if isinstance(_p, dict) and "value" in _p:
                _s = f"{float(_p['value']):g}"
                if not _re.fullmatch(r"[-+]?(?:[0-9]*\.?[0-9]+)(?:[eE][-+]?[0-9]+)?", _s):
                    _worst = _worst or _s
if _worst:
    wrong(f"the renderer prints {_worst!r}, which the grammar does not accept")
else:
    ok("every value in the corpus survives %g and the grammar")


# The DOC's example must parse too, for the same reason the docstring's does.
#
# A contract document that shows a syntax error teaches the error. This one
# lives at docs/contracts/patch-language-v1.md and is a public description of
# the format, so it is the version most people will copy from.
_doc_path = os.path.join(HERE, "..", "..", "docs", "contracts",
                         "patch-language-v1.md")
if not os.path.exists(_doc_path):
    wrong(f"the contract doc is missing: {_doc_path}")
else:
    _md = open(_doc_path).read()
    _blocks = _re.findall(r"```\n(.*?)```", _md, _re.S)
    _checked = 0
    for _b in _blocks:
        # Only the blocks that look like the notation: a declaration line.
        if not _re.search(r"^\s*\w+\s*:\s*\w+/\w+", _b, _re.M):
            continue
        _checked += 1
        try:
            L.parse(_b, inv)
        except L.PatchLangError as _e:
            wrong(f"the contract doc has an example that does not parse: {_e}")
    if _checked:
        ok(f"every notation example in the contract doc parses ({_checked})")
    else:
        wrong("the contract doc has no notation example to check")

# The property that makes the language worth having: positions are not
# expressible, so a hand-written patch cannot have overlapping panels.
text = "\n".join(f"m{i} : ForgeModular/VCO" for i in range(6)) + "\n"
doc = L.parse(text, inv)
if P.overlaps(doc, inv):
    wrong(f"a parsed patch has overlapping panels: {P.overlaps(doc, inv)[:1]}")
else:
    ok("a parsed patch is laid out with no overlaps — positions are ours")

print("\nall good" if bad == 0 else "\nFAILED")
sys.exit(1 if bad else 0)
