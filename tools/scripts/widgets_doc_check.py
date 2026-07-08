#!/usr/bin/env python3
"""widgets_doc_check.py — keep docs/reference/widgets.md in sync with the code.

Every developer-facing `class X : public View` primitive in
core/view/include/pulp/view/*.hpp must have a row in docs/reference/widgets.md
(its class name in backticks). When a new primitive is added, the catalog has
to grow with it — otherwise the live list of "what's possible" silently drifts.

Internal / tooling / inspector widgets that are NOT part of the developer-facing
primitive surface are listed in EXCLUDE below with a reason; adding a new one
there is a deliberate, reviewable decision.

Exit 0 if every primitive is documented (or excluded); exit 1 listing the
undocumented classes otherwise.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADERS = ROOT / "core" / "view" / "include" / "pulp" / "view"
DOC = ROOT / "docs" / "reference" / "widgets.md"

# Direct View subclasses that are NOT developer-facing design-system primitives
# (inspector internals, the in-app design tool, host/model dev tooling). Each is
# intentionally omitted from the public widget catalog.
EXCLUDE = {
    "AudioInspectorPanel": "component inspector internal",
    "AudioWaveformView": "inspector-internal waveform (use WaveformView)",
    "DesignFrameView": "in-app design tool internal",
    "DesignStepper": "in-app design tool internal",
    "DesignTabGroup": "in-app design tool internal",
    "InspectorHighlight": "component inspector overlay internal",
    "LiveConstantEditor": "developer live-tweak tooling",
    "ModelManagerView": "audio-model management tooling",
    "PluginManagerPanel": "plugin-host management tooling",
    "ThemeEditor": "in-app theme authoring tooling",
}

CLASS_RE = re.compile(r"^class\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*public\s+View\b", re.M)


def find_primitives() -> set[str]:
    names: set[str] = set()
    for hpp in sorted(HEADERS.glob("*.hpp")):
        names.update(CLASS_RE.findall(hpp.read_text(encoding="utf-8", errors="ignore")))
    return names


def documented() -> set[str]:
    text = DOC.read_text(encoding="utf-8", errors="ignore")
    # Any `ClassName` in backticks counts as documented.
    return set(re.findall(r"`([A-Za-z_][A-Za-z0-9_]*)`", text))


def main() -> int:
    if not DOC.exists():
        print(f"ERROR: {DOC.relative_to(ROOT)} is missing", file=sys.stderr)
        return 1
    primitives = find_primitives()
    docd = documented()
    missing = sorted(c for c in primitives if c not in docd and c not in EXCLUDE)
    stale_excludes = sorted(c for c in EXCLUDE if c not in primitives)

    if missing:
        print("ERROR: developer-facing View primitives missing from "
              "docs/reference/widgets.md:", file=sys.stderr)
        for c in missing:
            print(f"  - {c}  (add a catalog row, or add to EXCLUDE in "
                  f"tools/scripts/widgets_doc_check.py with a reason)", file=sys.stderr)
    if stale_excludes:
        print("ERROR: widgets_doc_check EXCLUDE lists classes that no longer "
              "exist (remove them):", file=sys.stderr)
        for c in stale_excludes:
            print(f"  - {c}", file=sys.stderr)

    if missing or stale_excludes:
        return 1
    print(f"widgets.md in sync: {len(primitives)} primitives "
          f"({len(EXCLUDE)} excluded as internal/tooling).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
