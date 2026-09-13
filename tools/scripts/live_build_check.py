#!/usr/bin/env python3
"""Report a governed build that is running in this checkout right now.

Shipyard's `local` mac backend builds IN the checkout, so a validation run and an
agent editing the tree occupy the same directory with nothing between them. On
2026-08-16 two merges landed into a worktree while a validation build was live
inside it. The run reached 11% in two hours, died on the lane's 7200s cap, and
reported `Validation timed out` — naming the target, not the cause. Every fact
needed to diagnose it was on disk; nothing asked anyone to look.

`tools/ci/governed-build.sh` writes `.pulp-build-active` at the source-tree root
for the life of a build. This reads it.

The pid is the load-bearing field. The marker file necessarily survives a SIGKILL
— that is how the sibling `build-dir-sentinel.sh` detects interrupted builds at
all — so its mere presence proves nothing. `kill(pid, 0)` is what separates "a
build is running here" from "a build died here a while ago", and treating those
alike in either direction is the failure this check exists to avoid: a stale
marker that cries wolf gets ignored within a week, and a live one that stays
silent is the bug itself.

A proven-dead marker is REAPED here, not merely reported. Proof of death is
already in hand at that point, and the removal branch is unreachable for a live
build by construction — `kill(pid, 0)` has to have said the owner is gone. The
alternative, printing "safe to remove" and leaving the file, re-proves the same
finding on every `gates.sh` and every pre-push in that checkout forever; a
permanently unclearing advisory is indistinguishable from a passing check, and
trains readers to skim the section that also carries the live-build warning.
Reaping also closes a pid-reuse window: an unreaped marker whose pid is
eventually recycled by an unrelated process starts reading as a live build.

Reaping this marker cannot weaken interrupted-build detection. That is
`build-dir-sentinel.sh`'s `.pulp-build-incomplete`, a different file in the
build dir, whose SIGKILL survival is its design; this one lives at the source
root and answers the opposite question.

Advisory by design. Exits 0 whether or not a build is live; a build in your own
checkout is a fact to know, not a policy violation, and this must never be the
reason a push fails. The non-zero codes are defects rather than findings about
the tree, and they are the cases where this check could not act on what it saw:

    0  nothing live here, or a live build reported, or a dead marker reaped
    2  malformed marker (no usable pid) — a defect in the writer. Left in place:
       it is the evidence, and no liveness claim was ever established about it.
    3  a dead marker that could not be reaped (read-only tree, permissions).
       The loop genuinely cannot close, so this must not read as a clean pass.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

MARKER_NAME = ".pulp-build-active"


def repo_root(start: Path) -> Path | None:
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            cwd=start, capture_output=True, text=True, check=False,
        )
    except OSError:
        return None
    path = out.stdout.strip()
    return Path(path) if out.returncode == 0 and path else None


def parse_marker(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            fields[key.strip()] = value.strip()
    return fields


def pid_is_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # Owned by another user: it exists, which is what we asked.
        return True
    except OSError:
        return False
    return True


def reap_marker(marker: Path) -> str | None:
    """Delete a marker proven dead. Returns None on success, else the reason.

    A concurrent reaper (or a build starting and rewriting it) winning the race
    is success: the caller wanted the stale file gone, and it is gone.
    """
    try:
        marker.unlink()
    except FileNotFoundError:
        return None
    except OSError as error:
        return str(error)
    return None


def describe_age(started_epoch: str) -> str:
    try:
        started = int(started_epoch)
    except (TypeError, ValueError):
        return "unknown age"
    if started <= 0:
        return "unknown age"
    seconds = max(0, int(time.time()) - started)
    if seconds < 90:
        return f"{seconds}s"
    return f"{seconds // 60}m"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=None,
                        help="checkout to inspect (default: this git worktree)")
    parser.add_argument("--quiet-when-idle", action="store_true",
                        help="print nothing when no build is live")
    args = parser.parse_args()

    root = args.root or repo_root(Path.cwd())
    if root is None:
        print("live-build-check: not a git worktree; nothing to inspect")
        return 0

    marker = root / MARKER_NAME
    if not marker.exists():
        if not args.quiet_when_idle:
            print("live-build-check: no governed build active in this checkout")
        return 0

    try:
        fields = parse_marker(marker.read_text(encoding="utf-8", errors="replace"))
    except OSError as error:
        print(f"live-build-check: cannot read {marker}: {error}", file=sys.stderr)
        return 2

    raw_pid = fields.get("pid", "")
    try:
        pid = int(raw_pid)
    except ValueError:
        print(f"live-build-check: marker has no usable pid ({raw_pid!r}); "
              f"treating as malformed rather than guessing", file=sys.stderr)
        return 2

    if not pid_is_alive(pid):
        # A killed build leaves this behind by construction. That is not an
        # alarm about a build already over — it is litter with a proof of death
        # attached, so clear it here instead of re-deriving the same proof on
        # every future run. Echo the fields first: the file is about to go, and
        # what was building is the part worth keeping.
        summary = (f"pid {pid}, started {fields.get('started_at', 'unknown')}, "
                   f"command: {fields.get('command', 'unknown')}")
        error = reap_marker(marker)
        if error is None:
            print(f"live-build-check: reaped stale marker ({summary}) — that "
                  f"build is gone; removed {marker}")
            return 0
        print(f"live-build-check: stale marker ({summary}) is dead but could "
              f"not be removed: {error}", file=sys.stderr)
        print(f"live-build-check: {marker} will keep being re-reported until "
              f"it is deleted by hand", file=sys.stderr)
        return 3

    age = describe_age(fields.get("started_epoch", ""))
    print("")
    print("  ⚠︎  A GOVERNED BUILD IS RUNNING IN THIS CHECKOUT")
    print(f"     pid {pid}, running {age}, -j{fields.get('jobs', '?')}, "
          f"lease={fields.get('lease', 'none')}")
    print(f"     command: {fields.get('command', 'unknown')}")
    print("")
    print("     Shipyard's local mac backend builds in the checkout, so editing")
    print("     this tree — merge, rebase, checkout, branch switch — changes the")
    print("     sources under a running CMake. The build does not fail cleanly;")
    print("     it slows down and dies on the lane timeout, and the failure names")
    print("     the target rather than the cause.")
    print("")
    print("     Let it finish, or work in another worktree.")
    print(f"     Watch it:  shipyard watch --pr <N>     Marker: {marker}")
    print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
