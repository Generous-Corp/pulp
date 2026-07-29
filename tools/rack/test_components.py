#!/usr/bin/env python3
"""Guard: no generated panel may use Rack's CC BY-NC component graphics.

Rack's widget machinery is ours to build on -- the plugin licence exception
covers linking and using the API. Its *graphics* are licensed CC BY-NC 4.0,
and a module drawn with them carries a non-commercial condition on its
artwork, inherited by anyone who builds one and never mentioned to them.

The replacement is easy to undo by accident: one `createParamCentered<Trimpot>`
in a hand-edit or a regenerated emitter and the obligation is back, invisibly,
because nothing about the module looks different at a glance. So this reads the
emitted code and refuses the whole component library by name.

    python3 tools/rack/test_components.py
"""
from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.normpath(os.path.join(HERE, "..", "..", "examples", "forge-modular"))
GENERATED = os.path.join(PACK, "src", "generated_modules.hpp")
COMPONENTS = os.path.join(PACK, "res", "components")

# Rack's component library, by the names generated code would use. Not
# exhaustive over the whole library -- exhaustive over what a panel emitter
# plausibly reaches for, which is what has to stay out.
FORBIDDEN = [
    "PJ301MPort", "PJ3410Port", "CL1362Port",
    "ScrewSilver", "ScrewBlack",
    "RoundBlackKnob", "RoundBigBlackKnob", "RoundSmallBlackKnob",
    "RoundHugeBlackKnob", "Rogan", "Davies1900h", "BefacoTinyKnob",
    "Trimpot", "CKSS", "CKSSThree", "CKD6", "NKK",
    "VCVButton", "VCVLatch", "VCVSlider", "VCVLightSlider", "VCVLightLatch",
    "TinyLight", "SmallLight", "MediumLight", "LargeLight",
    "BefacoSlidePot", "LEDSlider", "LEDBezel",
]

# Ours, which the emitted code must be using instead.
EXPECTED = ["ForgeKnob", "ForgePort", "ForgeScrew"]

REQUIRED_ART = [
    "knob-large.svg", "knob.svg", "knob-small.svg", "trimpot.svg",
    "port.svg", "screw.svg", "toggle-0.svg", "toggle-1.svg",
    "switch3-0.svg", "switch3-1.svg", "switch3-2.svg",
    "slider-bg.svg", "slider-handle.svg",
    "light-tiny.svg", "light-small.svg", "light-medium.svg", "light-large.svg",
]


def scan(text: str) -> list:
    """Rack component names used as a widget type, not merely mentioned.

    Matched inside the angle brackets of a create*<...> call so that a comment
    explaining why we avoid `Trimpot` does not read as using one -- a guard
    that fires on its own rationale gets disabled within the week.
    """
    hits = []
    for m in re.finditer(r"create\w*Centered<([^>]+)>", text):
        inner = m.group(1)
        for name in FORBIDDEN:
            if re.search(rf"(?<![A-Za-z]){re.escape(name)}(?![A-Za-z])", inner):
                hits.append((name, inner))
    return hits


def main():
    bad = 0

    if not os.path.exists(GENERATED):
        print("  SKIP   nothing generated yet — run forge_modular.py first")
        return 0
    text = open(GENERATED).read()

    hits = scan(text)
    if hits:
        for name, inner in sorted(set(hits))[:8]:
            print(f"  WRONG  uses Rack's {name} (in {inner}) — CC BY-NC artwork")
        bad += 1
    else:
        print(f"  ok     no Rack component graphics in the emitted panels")

    used_ours = [e for e in EXPECTED if e in text]
    if len(used_ours) != len(EXPECTED):
        print(f"  WRONG  expected our components throughout; found only "
              f"{used_ours or 'none'}")
        bad += 1
    else:
        print("  ok     our own components are the ones being placed")

    missing = [a for a in REQUIRED_ART
               if not os.path.exists(os.path.join(COMPONENTS, a))]
    if missing:
        print(f"  WRONG  component art missing: {missing}")
        bad += 1
    else:
        print(f"  ok     all {len(REQUIRED_ART)} component drawings present")

    # Art that exists but is empty renders as nothing, and a panel of invisible
    # knobs is harder to diagnose than a missing file.
    thin = [a for a in REQUIRED_ART
            if os.path.exists(os.path.join(COMPONENTS, a))
            and os.path.getsize(os.path.join(COMPONENTS, a)) < 120]
    if thin:
        print(f"  WRONG  suspiciously small component art: {thin}")
        bad += 1
    else:
        print("  ok     no empty drawings")

    print(f"\n{'FAIL' if bad else 'ok'}: {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
