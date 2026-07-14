#!/usr/bin/env python3
"""Keep Pulp's own framework surfaces free of foreign-framework vocabulary.

Pulp's UI and drawing layers must stand on their own. A capability lands
because a Pulp user wants it, described in Pulp's terms — never as
"the thing framework X calls Y". Two reasons, both load-bearing:

  1. A capability that can only be explained by pointing at another
     framework is a compatibility shim wearing a feature's clothes. The
     naming rule is a design check: if the doc-comment needs a foreign
     name to make sense, the API is wrong.

  2. Pulp is MIT and reimplements nothing. Source that cites a
     copyleft framework's internals as its rationale reads as derivation
     regardless of how the code was actually written.

Translation vocabulary (`SomeFramework::Widget -> pulp::view::Widget`) is
data, and it belongs in the importer that owns it — not here.

DELIBERATELY NOT FLAGGED
------------------------
* Plug-in format APIs Pulp implements (`IPlugView`, `IPluginFactory`,
  `IComponent`, ...). Those are Steinberg's VST3 interface names and Pulp
  speaks them by necessity. Word boundaries keep them out of the net.
* `core/format/host_quirks/**` and `host-quirks.json`. Those files cite
  prior-art audits ON PURPOSE: the citation IS the clean-room audit trail
  the Reference-Lineage trailer requires. Removing it would destroy the
  provenance it exists to record.

Usage:
    python3 tools/scripts/framework_neutrality_check.py            # scan
    python3 tools/scripts/framework_neutrality_check.py --mode=report   # CI (exit 1)
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Surfaces that must stay framework-neutral. These are the layers a UI import
# would otherwise contaminate.
SCANNED_ROOTS = [
    "core/canvas",
    "core/view",
    "core/signal",
    "core/state",
]

# Paths whose foreign-framework references are the point (see module docstring).
EXEMPT_SUBSTRINGS = [
    "core/format/host_quirks/",
    "host-quirks.json",
]

SCANNED_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp", ".mm", ".m", ".txt", ".cmake"}

# Word-boundary anchored so VST3's `IPlugView` / `IPluginFactory` do not match
# `iplug`, and so `juice`/`prejudice` do not match `juce`.
PATTERNS = [
    (re.compile(r"\bjuce\b", re.I), "foreign UI framework"),
    (re.compile(r"\biplug2?\b", re.I), "foreign UI framework"),
    (re.compile(r"\bvstgui\b", re.I), "foreign UI framework"),
    (re.compile(r"\bhise\b", re.I), "foreign UI framework"),
    (re.compile(r"\blook\s*and\s*feel\b", re.I), "foreign skinning concept"),
    (re.compile(r"\bLookAndFeel\w*"), "foreign skinning concept"),
    (re.compile(r"\bAPVTS\b"), "foreign parameter-tree concept"),
    (re.compile(r"\bAudioProcessorValueTreeState\b"), "foreign parameter-tree concept"),
    (re.compile(r"\bdraw(Rotary|Linear)Slider\b"), "foreign draw-hook name"),
    (re.compile(r"\baddAndMakeVisible\b"), "foreign view-tree call"),
    (re.compile(r"\bSafePointer\b"), "foreign lifetime type"),
]

SUGGESTION = """
  Describe the behaviour, not its ancestry. Instead of:
      // Matches <Framework>::Slider::setSkewFactor
  write what it actually does, in units a Pulp reader can act on:
      /// Skew of 1.0 is linear. Below 1.0 biases resolution toward the minimum.

  If the API only makes sense as "the <Framework> thing", it is a shim, not a
  feature — redesign it. Framework->Pulp translation tables belong in the
  importer that owns them.
""".rstrip()


def is_exempt(rel: str) -> bool:
    return any(x in rel for x in EXEMPT_SUBSTRINGS)


def scan() -> list[tuple[str, int, str, str]]:
    hits: list[tuple[str, int, str, str]] = []
    for root in SCANNED_ROOTS:
        base = REPO / root
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in SCANNED_SUFFIXES:
                continue
            rel = str(path.relative_to(REPO))
            if is_exempt(rel):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for lineno, line in enumerate(text.splitlines(), 1):
                for pattern, why in PATTERNS:
                    if pattern.search(line):
                        hits.append((rel, lineno, line.strip()[:100], why))
                        break
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mode", choices=["hint", "report"], default="report")
    args = ap.parse_args()

    hits = scan()
    if not hits:
        print(f"framework-neutrality: clean ({', '.join(SCANNED_ROOTS)})")
        return 0

    print("framework-neutrality: foreign-framework vocabulary in Pulp's own surfaces\n")
    for rel, lineno, line, why in hits:
        print(f"  {rel}:{lineno}: {why}")
        print(f"      {line}")
    print(SUGGESTION)
    print(f"\n{len(hits)} finding(s).")

    if args.mode == "hint":
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
