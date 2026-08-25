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
COMBO_PATTERN = re.compile(r"\bnotify_global_click\s*\(")

# Mechanism (B): any consultation of the generalized overlay slot. A host may
# either call the shared portable verb or hand-roll the equivalent check, so
# accept any of these rather than mandating one spelling.
OVERLAY_PATTERNS = (
    re.compile(r"\broute_press_to_active_overlay\s*\("),
    re.compile(r"\bdismiss_active_overlay\s*\("),
    re.compile(r"\bactive_overlay_\b"),
    re.compile(r"\boverlay_contains\s*\("),
)


def executable_shape(text: str) -> str:
    """Blank comments and literals while preserving offsets and newlines."""
    chars = list(text)
    i = 0
    state = "code"
    quote = ""
    while i < len(chars):
        c = chars[i]
        nxt = chars[i + 1] if i + 1 < len(chars) else ""
        if state == "code":
            if c == "/" and nxt == "/":
                chars[i] = chars[i + 1] = " "
                i += 2
                state = "line_comment"
                continue
            if c == "/" and nxt == "*":
                chars[i] = chars[i + 1] = " "
                i += 2
                state = "block_comment"
                continue
            if c in {'"', "'"}:
                quote = c
                chars[i] = " "
                i += 1
                state = "literal"
                continue
        elif state == "line_comment":
            if c == "\n":
                state = "code"
            else:
                chars[i] = " "
            i += 1
            continue
        elif state == "block_comment":
            if c == "*" and nxt == "/":
                chars[i] = chars[i + 1] = " "
                i += 2
                state = "code"
                continue
            if c != "\n":
                chars[i] = " "
            i += 1
            continue
        else:
            if c == "\\" and i + 1 < len(chars):
                chars[i] = " "
                if chars[i + 1] != "\n":
                    chars[i + 1] = " "
                i += 2
                continue
            if c == quote:
                chars[i] = " "
                i += 1
                state = "code"
                continue
            if c != "\n":
                chars[i] = " "
            i += 1
            continue
        i += 1
    return "".join(chars)


def matching_lines(text: str, pattern: re.Pattern[str]) -> list[int]:
    return [index for index, line in enumerate(text.splitlines()) if pattern.search(line)]


def matching_offsets(text: str, pattern: re.Pattern[str]) -> list[int]:
    return [match.start() for match in pattern.finditer(text)]


def function_bodies(text: str) -> list[tuple[int, int]]:
    """Return brace ranges whose declaration prefix looks callable."""
    stack: list[int] = []
    ranges: list[tuple[int, int]] = []
    for index, char in enumerate(text):
        if char == "{":
            stack.append(index)
        elif char == "}" and stack:
            start = stack.pop()
            delimiter = max(text.rfind(";", 0, start),
                            text.rfind("{", 0, start),
                            text.rfind("}", 0, start))
            prefix = text[delimiter + 1:start].strip()
            if ")" in prefix:
                ranges.append((start, index))
    return ranges


def mechanisms_share_body(text: str) -> bool:
    executable = executable_shape(text)
    combos = matching_offsets(executable, COMBO_PATTERN)
    overlays = [
        offset
        for pattern in OVERLAY_PATTERNS
        for offset in matching_offsets(executable, pattern)
    ]
    bodies = function_bodies(executable)
    return bool(combos) and all(
        any(
            start < combo < end and
            any(start < overlay < end for overlay in overlays)
            for start, end in bodies
        )
        for combo in combos
    )


def self_test() -> bool:
    valid = """
    void press() {
        ComboBox::notify_global_click(target);
        route_press_to_active_overlay(root, pt);
    }
    """
    unrelated = """
    void press() { ComboBox::notify_global_click(target); }
    void escape() { View::dismiss_active_overlay(); }
    const char* decoy = "route_press_to_active_overlay(root, pt)";
    /* route_press_to_active_overlay(root, pt); */
    """
    return mechanisms_share_body(valid) and not mechanisms_share_body(unrelated)


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
    if not self_test():
        print(
            "overlay-dismissal-wiring: FAIL - structural guard self-test failed",
            file=sys.stderr,
        )
        return 1
    offenders: list[Path] = []
    checked = 0

    for path in host_sources(root):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        executable_text = executable_shape(text)
        combo_lines = matching_lines(executable_text, COMBO_PATTERN)
        if not combo_lines:
            continue
        checked += 1
        # Both mechanisms must occur inside one callable body. This rejects an
        # unrelated ESC handler, namespace-level token, comment, or string;
        # nested lambdas remain valid because their enclosing press handler is
        # also a callable brace range.
        paired = mechanisms_share_body(text)
        if not paired:
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
