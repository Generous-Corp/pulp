#!/usr/bin/env python3
"""Do CARTOG and the port-map reader agree on field names?

    tools/rack/test_portmap_fields.py

CARTOG builds the map as TEXT, inside Rack, with string concatenation.
PortMap reads it with a JSON parser, in the app. Nothing links the two: a
renamed field on either side parses cleanly and yields nothing, which looks
exactly like never having scanned — every vendor module drawn without jacks.

Every other port-map test uses a hand-written fixture, and a fixture agrees
with the parser by construction because one person wrote both.

Fields CARTOG writes that the reader ignores are listed below rather than
tolerated silently: measuring something and dropping it on read is a half-built
feature, and it should be a decision somebody made, not one nobody noticed.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CARTOG = os.path.join(HERE, "..", "..", "examples", "forge-modular", "src",
                      "CARTOG.cpp")
READER = os.path.join(HERE, "..", "..", "forge-seam", "modular", "portmap.cpp")

# Written on purpose and not read YET. Each needs a reason, so that removing
# one is a deliberate act and adding one is visible in review.
UNREAD_BY_DESIGN = {
    "lights":   "measured for a future light-drawing pass; nothing draws them",
    "displays": "same — a screen's rectangle, with nothing drawing screens yet",
    "kind":     "knob / slider / button / switch. The preview draws every "
                "control as a knob; reading this is how a fader stops looking "
                "like a dial",
    "type":     "the widget's class name, kept for diagnosing a bad scan",
}


def main():
    bad = 0
    written = set(re.findall(r'\\"([a-zA-Z]+)\\"', open(CARTOG).read()))
    read = set(re.findall(r'"([a-zA-Z]+)"', open(READER).read()))

    if not written:
        print("  WRONG  no emitted field names found in CARTOG.cpp — this "
              "check cannot see what it writes")
        return 1

    dropped = sorted(written - read)
    unexpected = [f for f in dropped if f not in UNREAD_BY_DESIGN]
    if unexpected:
        print(f"  WRONG  CARTOG writes fields the reader ignores, and nothing "
              f"says why: {unexpected}")
        print( "         A field renamed on one side parses clean and yields "
               "nothing.")
        bad += 1
    else:
        print(f"  ok     every dropped field is listed with a reason "
              f"({len(dropped)} of them)")

    # The other direction is the dangerous one: the reader asking for a field
    # nobody writes gets silence, not an error.
    core = {"modules", "plugin", "model", "pluginVersion", "size",
            "params", "inputs", "outputs", "index", "name", "x", "y"}
    missing = sorted(f for f in core if f not in written)
    if missing:
        print(f"  WRONG  the reader depends on fields CARTOG does not write: "
              f"{missing}")
        bad += 1
    else:
        print(f"  ok     every field the drawing needs is one CARTOG writes")

    print("\n" + ("all good" if bad == 0 else "FAILED"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
