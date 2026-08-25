#!/usr/bin/env python3
"""overlay_dismissal_wiring_guard.py — every platform host that routes a press
to an open ComboBox must also consult the generalized overlay slot.

Pulp has two independent popup-dismissal mechanisms:

  A. `ComboBox::notify_global_click(target)` closes an open native ComboBox
     dropdown when the press lands outside it.
  B. `View::active_overlay_` + `overlay_contains()` + `dismiss_active_overlay()`
     closes a generalized overlay — what `@pulp/react`'s `<View overlay>` prop
     and every imported/materialized design's popover claims.

They are consulted by the platform hosts, not by the view tree, so each host
carries the obligation independently. That is the whole failure mode this guard
exists for: the standalone macOS host wires both, while the DAW plugin hosts
wired only (A). A dropdown built on (B) therefore stayed open forever when the
user clicked outside it inside a plugin editor, while the same UI dismissed
correctly in the standalone app.

The rule is one-directional and deliberately narrow: a host that consults
mechanism (A) has a press-routing path, so it must consult (B) there too. A host
with no press routing at all is out of scope and is not flagged.

Exit codes:
    0 - every host that consults ComboBox::notify_global_click also consults
        the generalized overlay slot
    1 - one or more hosts consult only the ComboBox mechanism
"""

from __future__ import annotations

import sys
import re
from pathlib import Path

# Hosts live under these roots; any source file below them is in scope.
HOST_ROOTS = (
    "core/view/platform",
    "core/view/include/pulp/view/platform",
    "core/view/include/pulp/view/web",
)

SOURCE_SUFFIXES = {".mm", ".cpp", ".hpp", ".h"}

# Mechanism (A): the native ComboBox outside-click notification. Matched
# call-shaped so a file that merely names it in an #include comment — and
# delegates its actual press routing elsewhere — is correctly out of scope.
COMBO_MARKER = "notify_global_click("

# Mechanism (B): any consultation of the generalized overlay slot. A host may
# either call the shared portable verb or hand-roll the equivalent check, so
# accept any of these rather than mandating one spelling.
OVERLAY_PATTERNS = (
    re.compile(r"\broute_press_to_active_overlay\s*\("),
    re.compile(r"\bdismiss_active_overlay\s*\("),
    re.compile(r"\bactive_overlay_\b"),
    re.compile(r"\boverlay_contains\s*\("),
)


def without_comments(text: str) -> str:
    """Remove C/C++ comments so prose cannot satisfy a wiring obligation."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.DOTALL)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def host_sources(root: Path) -> list[Path]:
    found: list[Path] = []
    for rel in HOST_ROOTS:
        base = root / rel
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                found.append(path)
    return found


def main() -> int:
    root = repo_root()
    offenders: list[Path] = []
    checked = 0

    for path in host_sources(root):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        executable_text = without_comments(text)
        if COMBO_MARKER not in executable_text:
            continue
        checked += 1
        if not any(pattern.search(executable_text) for pattern in OVERLAY_PATTERNS):
            offenders.append(path)

    if checked == 0:
        # A guard that inspects nothing cannot fail. Treat an empty census as a
        # broken guard, not as a pass.
        print(
            "overlay-dismissal-wiring: FAIL - no host source referenced "
            f"{COMBO_MARKER!r}; the guard is not measuring anything.",
            file=sys.stderr,
        )
        return 1

    if offenders:
        print(
            "overlay-dismissal-wiring: FAIL - these hosts route presses to an "
            "open ComboBox but never consult the generalized overlay slot, so "
            "a React/imported popover can never be dismissed by an outside "
            "click in them:",
            file=sys.stderr,
        )
        for path in offenders:
            print(f"  {path.relative_to(root)}", file=sys.stderr)
        print(
            "\nFix: call pulp::view::route_press_to_active_overlay(root, pt) "
            "on the press path, mirroring "
            "core/view/platform/mac/window_host_mac.mm.",
            file=sys.stderr,
        )
        return 1

    print(
        f"overlay-dismissal-wiring: OK - {checked} press-routing host(s) "
        "consult both the ComboBox and the generalized overlay mechanism."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
