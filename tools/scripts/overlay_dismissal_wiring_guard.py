#!/usr/bin/env python3
"""overlay_dismissal_wiring_guard.py — the overlay-dismissal policy has exactly
one implementation, and every platform host routes through it.

Pulp has two independent popup mechanisms a host must keep in step:

  A. `ComboBox`'s open native dropdown (`notify_global_click`,
     `active_popup_in`, `close_active_popup`).
  B. The generalized overlay slot (`RootInteractionState::active_overlay`),
     which `@pulp/react`'s `<View overlay>` prop — and every imported or
     materialized design's popover — claims via `View::claim_overlay()`.

Deciding what a press or an Escape means for those two is ONE policy, and it
lives in `core/view/src/overlay_dismissal.cpp` behind the verbs declared in
`core/view/include/pulp/view/overlay_dismissal.hpp`. Hosts own native event
plumbing only.

An earlier version of this guard checked something weaker: that a host which
consults (A) also mentions (B) *somehow*, accepting a hand-rolled equivalent as
readily as the shared verb. That sanctioned four independent copies of the
policy, and they drifted exactly as independent copies do — one read the
process-global shim mirror so a press in one editor dismissed another's
popover, one never honoured an overlay's outside-click consumption and never
routed a press into the overlay's own subtree, and two had no Escape path at
all. A guard that blesses the defect is worse than no guard, so the rule is now
the stronger one:

  1. A host that routes presses (it calls `ComboBox::notify_global_click`) must
     call `route_press_to_active_overlay` or `route_context_press`.
  2. No host may reach past the verbs to the slot itself. The slot-level
     spellings are the shared implementation's alone.

Exit codes:
    0 - every press-routing host calls a shared verb, and no host hand-rolls
    1 - a host skipped the verbs, or reached past them to the slot
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

# The single implementation. These two files are the policy, so the slot-level
# spellings below are theirs to use; everything else must go through the verbs.
POLICY_SOURCES = (
    "core/view/src/overlay_dismissal.cpp",
    "core/view/include/pulp/view/overlay_dismissal.hpp",
)

# Mechanism (A): the native ComboBox outside-click notification. Matched
# call-shaped so a file that merely names it in an #include comment — and
# delegates its actual press routing elsewhere — is correctly out of scope.
COMBO_MARKER = "notify_global_click("
COMBO_PATTERN = re.compile(r"\bnotify_global_click\s*\(")

# Mechanism (B), reached the only sanctioned way: through a shared verb.
VERB_PATTERNS = (
    re.compile(r"\broute_press_to_active_overlay\s*\("),
    re.compile(r"\broute_context_press\s*\("),
)

# Mechanism (B) reached the wrong way — a host re-deriving the policy from the
# slot. Each of these was a real drift site before the policy was unified.
HAND_ROLLED_PATTERNS = (
    re.compile(r"\bactive_overlay_\b"),
    re.compile(r"\bactive_overlay\b(?!_)"),
    re.compile(r"\boverlay_contains\s*\("),
    re.compile(r"\bdismiss_active_overlay\s*\("),
    re.compile(r"\bdismiss_claimed_overlay\s*\("),
    re.compile(r"\boverlay_consumes_outside_click\s*\("),
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
    return [index for index, line in enumerate(text.splitlines(), start=1)
            if pattern.search(line)]


def self_test() -> bool:
    """The guard must reject the shapes it exists to reject."""
    routed = """
    void press() {
        ComboBox::notify_global_click(target);
        const auto r = route_press_to_active_overlay(root, pt);
    }
    """
    hand_rolled = """
    void press() {
        ComboBox::notify_global_click(target);
        if (auto* o = View::active_overlay_) {
            if (!o->overlay_contains(pt)) View::dismiss_active_overlay();
        }
    }
    """
    unwired = """
    void press() { ComboBox::notify_global_click(target); }
    """
    decoys = """
    void press() {
        ComboBox::notify_global_click(target);
        route_press_to_active_overlay(root, pt);
    }
    const char* s = "View::active_overlay_";
    /* overlay_contains(pt); */
    """
    return (
        verdict(routed) == ()
        and verdict(hand_rolled) != ()
        and verdict(unwired) != ()
        and verdict(decoys) == ()
    )


def verdict(text: str) -> tuple[str, ...]:
    """Return the reasons `text` fails, empty when it is correctly wired.

    A file with no press routing at all is out of scope for rule 1 but still
    bound by rule 2 — a host must not hand-roll the policy on any path.
    """
    executable = executable_shape(text)
    reasons: list[str] = []
    if COMBO_PATTERN.search(executable) and not any(
        pattern.search(executable) for pattern in VERB_PATTERNS
    ):
        reasons.append(
            "routes presses to an open ComboBox but never calls "
            "route_press_to_active_overlay() / route_context_press(), so a "
            "React or imported-design popover can never be dismissed by an "
            "outside click here"
        )
    for pattern in HAND_ROLLED_PATTERNS:
        lines = matching_lines(executable, pattern)
        if lines:
            reasons.append(
                f"reaches past the shared verbs to the overlay slot itself "
                f"({pattern.pattern}) on line(s) "
                f"{', '.join(str(n) for n in lines)}"
            )
    return tuple(reasons)


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

    missing_policy = [rel for rel in POLICY_SOURCES if not (root / rel).is_file()]
    if missing_policy:
        # The verbs the hosts are required to call must exist, or the guard is
        # measuring compliance with a policy that is not there.
        print(
            "overlay-dismissal-wiring: FAIL - the shared policy is missing: "
            + ", ".join(missing_policy),
            file=sys.stderr,
        )
        return 1

    offenders: list[tuple[Path, tuple[str, ...]]] = []
    press_routing_hosts = 0
    inspected = 0

    for path in host_sources(root):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        inspected += 1
        if COMBO_PATTERN.search(executable_shape(text)):
            press_routing_hosts += 1
        reasons = verdict(text)
        if reasons:
            offenders.append((path, reasons))

    if press_routing_hosts == 0:
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
            "overlay-dismissal-wiring: FAIL - the overlay-dismissal policy has "
            "one implementation, in core/view/src/overlay_dismissal.cpp. These "
            "hosts do not route through it:",
            file=sys.stderr,
        )
        for path, reasons in offenders:
            print(f"  {path.relative_to(root)}", file=sys.stderr)
            for reason in reasons:
                print(f"      {reason}", file=sys.stderr)
        print(
            "\nFix: call pulp::view::route_press_to_active_overlay(root, pt) on "
            "the press path and pulp::view::route_escape_to_active_overlay(root) "
            "on the key path, mirroring "
            "core/view/platform/mac/window_host_mac.mm. Do not re-derive the "
            "decision from the slot.",
            file=sys.stderr,
        )
        return 1

    print(
        f"overlay-dismissal-wiring: OK - {press_routing_hosts} press-routing "
        f"host(s) of {inspected} inspected route through the single shared "
        "policy, and no host hand-rolls it."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
