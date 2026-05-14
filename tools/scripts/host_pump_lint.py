#!/usr/bin/env python3
"""host_pump_lint.py — guard against the pulp-internal #71 foot-gun.

Every live-host main.cpp (examples/design-tool, examples/ui-preview, future
hosts) must drive BOTH halves of the WidgetBridge idle pump on every tick:

    bridge->poll_async_results();      // async-exec results + frame callbacks
    bridge->service_frame_callbacks(); // setTimeout / setInterval / __flushTimers__

Calling only the first half silently freezes every imported app's polling
state path: setInterval(fn, N) returns a valid id but `fn` never fires. The
underlying chart/canvas (which paints from a separate ref store) keeps
updating, but every React label that reads polled state stays frozen.
See scripted_ui.cpp:67-81 for the contract documentation; see PR #1957
follow-up commit for the concrete regression.

This lint scans every examples/*/main.cpp (and other host main files we
extend the map to) and flags any call to poll_async_results() whose
nearby context (next 30 lines, same `^}` block) does not also call
service_frame_callbacks(). Fails CI on violation.

Exit codes: 0 = clean, 1 = violation found, 2 = scan error.

Bypass for the rare host that genuinely doesn't need the second half
(e.g. a CLI tool that processes one event then exits): add an inline
comment on the same line as poll_async_results():
    bridge->poll_async_results();  // host-pump-lint: skip — one-shot CLI
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Hosts that are required to pump the bridge. Add new entries here as
# new live hosts come online.
HOST_FILES = [
    "examples/design-tool/main.cpp",
    "examples/ui-preview/main.cpp",
]

POLL_RE = re.compile(r"\bpoll_async_results\s*\(\s*\)")
SERVICE_RE = re.compile(r"\bservice_frame_callbacks\s*\(\s*\)")
SKIP_MARKER = "host-pump-lint: skip"
WINDOW_LINES = 30


def scan_file(path: Path) -> list[str]:
    """Return list of violation messages (empty if clean)."""
    if not path.exists():
        return []  # missing host = nothing to lint, not a failure
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    violations: list[str] = []
    for i, line in enumerate(lines):
        if not POLL_RE.search(line):
            continue
        if SKIP_MARKER in line:
            continue
        # Look forward in the same block (until next `^}` at column 0
        # or up to WINDOW_LINES) for the paired call.
        end = min(i + WINDOW_LINES, len(lines))
        paired = False
        for j in range(i, end):
            if j > i and lines[j].rstrip() == "}":
                # Closing brace at column 0 — leaving the handler block.
                break
            if SERVICE_RE.search(lines[j]):
                paired = True
                break
        if not paired:
            try:
                shown = str(path.relative_to(REPO_ROOT))
            except ValueError:
                shown = str(path)
            violations.append(
                f"{shown}:{i + 1}: "
                f"poll_async_results() not paired with service_frame_callbacks() "
                f"within {WINDOW_LINES} lines (or before block close)"
            )
    return violations


def main() -> int:
    all_violations: list[str] = []
    for rel in HOST_FILES:
        path = REPO_ROOT / rel
        try:
            all_violations.extend(scan_file(path))
        except OSError as e:
            print(f"host_pump_lint: failed to read {rel}: {e}", file=sys.stderr)
            return 2
    if all_violations:
        print("host_pump_lint: idle-pump pairing violations detected:", file=sys.stderr)
        for v in all_violations:
            print(f"  {v}", file=sys.stderr)
        print(
            "\nFix: add `bridge->service_frame_callbacks();` immediately after "
            "the `poll_async_results()` call in the same handler block.\n"
            "Background: pulp-internal #71. Without the second half, every "
            "imported app's setTimeout/setInterval queues forever and any "
            "polling-driven React state freezes.\n"
            "Genuine one-shot CLI exemption: append "
            f'`// {SKIP_MARKER} — <reason>` to the line.',
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
