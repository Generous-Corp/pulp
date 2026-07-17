#!/usr/bin/env python3
"""Pending-intent liveness alarm — the time-based backstop for the
single-writer version-at-land bot.

Under the intent-trailer model, a release-worthy PR merges carrying a
positive ``Version-Bump: <surface>=<level>`` trailer and NO version-file
edit; the post-merge ``version_at_land`` bot assigns the number moments
later, committing a ``Version-Bump-Applied:`` marker. The auto-release
stranded-fix detector is taught (``PULP_ACCEPT_INTENT_TRAILERS=1``) to treat
such a pending intent as COVERED so it does not false-warn on every merge.

That correctness choice opens a blind spot: if the bot never runs or never
succeeds — a ``paths:`` filter miss, a disabled workflow, a bot push-permission
failure, an exhausted ``--ff-only`` retry loop — the intent stays pending
FOREVER and the detector stays quiet by design. Nothing tags; consumers are
silently stranded.

This alarm closes that blind spot. It is deliberately INDEPENDENT of the
detector's "pending == covered" classification: here, a positive intent that
remains unapplied *beyond a grace window* IS the alarm. It keys on the newest
``Version-Bump-Applied`` marker (the bot's own progress record), so it cannot
be fooled by a stale detector view.

Scope note — this alarm fires only on POSITIVE authored intent
(``<surface>=patch|minor|major``), read ``--no-merges``-scoped exactly as
``version_at_land`` honors it. A user-facing fix/feat that carries NO intent is
a *different* failure (the change routed around the PR gate); that is the
stranded-fix detector's job, and leaving it out here keeps the two alarms from
double-firing on the same commit.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gate_common import git_range_trailers  # noqa: E402
from version_bump_heuristics import surface_trailer_override  # noqa: E402
from version_bump_surfaces import load_config  # noqa: E402
from version_at_land import APPLIED_MARKER, last_applied_marker  # noqa: E402

POSITIVE_LEVELS = ("patch", "minor", "major")
DEFAULT_GRACE_MINUTES = 45


def _git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True, capture_output=True, text=True,
    ).stdout


def oldest_pending_intent_epoch(
    repo: Path, base: str, head: str, cfg, surface: str
) -> int | None:
    """Committer epoch of the OLDEST non-merge commit in ``base..head`` that
    declares a positive ``Version-Bump: <surface>=<level>`` intent, or None if
    none does. Oldest, because that is when the pending window started."""
    out = _git(
        repo, "log", "--no-merges", "--reverse", "--format=%H %ct",
        f"{base}..{head}",
    )
    for line in out.splitlines():
        if not line.strip():
            continue
        sha, ct = line.split()
        trailers = git_range_trailers(f"{sha}~1", sha, no_merges=True, cwd=repo)
        if surface_trailer_override(
            trailers, cfg.trailer_version_bump, surface
        ) in POSITIVE_LEVELS:
            return int(ct)
    return None


def assess(
    repo: Path, config_path: Path, head: str = "HEAD",
    grace_minutes: int = DEFAULT_GRACE_MINUTES, now_epoch: int | None = None,
) -> dict:
    """Pure detection. Returns a dict:
        {"overdue": [ {surface, level, age_minutes} ... ],
         "pending": [ ... within grace ... ],
         "base": <marker sha or None>}
    ``overdue`` non-empty == ALARM.
    """
    now_epoch = now_epoch if now_epoch is not None else int(time.time())
    cfg = load_config(config_path)
    base = last_applied_marker(repo, head)
    if not base:
        # No marker yet == the bot has never applied anything on this branch
        # (pre-flip / fresh history). There is no established "already
        # processed up to here" line to measure a strand against, so stay
        # quiet; the flip canary bootstraps the first marker.
        return {"overdue": [], "pending": [], "base": None}

    # The whole no-merges range is the source of truth for pending intent.
    trailers = git_range_trailers(base, head, no_merges=True, cwd=repo)
    overdue: list[dict] = []
    pending: list[dict] = []
    for surface in cfg.surfaces:
        level = surface_trailer_override(
            trailers, cfg.trailer_version_bump, surface.name
        )
        if level not in POSITIVE_LEVELS:
            continue
        started = oldest_pending_intent_epoch(
            repo, base, head, cfg, surface.name
        )
        if started is None:
            continue
        age_min = max(0, (now_epoch - started) // 60)
        row = {"surface": surface.name, "level": level, "age_minutes": age_min}
        (overdue if age_min >= grace_minutes else pending).append(row)
    return {"overdue": overdue, "pending": pending, "base": base}


def _fmt(rows: list[dict]) -> str:
    return ", ".join(
        f"{r['surface']}={r['level']} ({r['age_minutes']}m)" for r in rows
    )


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", default=".", type=Path)
    ap.add_argument("--config", default="tools/scripts/versioning.json", type=Path)
    ap.add_argument("--head", default="HEAD")
    ap.add_argument(
        "--grace-minutes", type=int,
        default=int(os.environ.get("PULP_PENDING_INTENT_GRACE_MIN",
                                   DEFAULT_GRACE_MINUTES)),
    )
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    result = assess(args.repo, args.config, args.head, args.grace_minutes)
    if args.json:
        import json
        print(json.dumps(result))

    if result["pending"]:
        print(f"::notice::pending intent within grace: {_fmt(result['pending'])}")
    if result["overdue"]:
        # Loud, exit non-zero — the workflow opens/updates the tracker issue.
        print(
            "::error::pending version intent has not been applied within "
            f"{args.grace_minutes} min (base marker {str(result['base'])[:8]}): "
            f"{_fmt(result['overdue'])}. The version-at-land bot did not run or "
            "failed — releases are silently stalled. See "
            "docs/guides/version-at-land-cutover.md."
        )
        return 1
    print(
        f"pending-intent liveness OK (base {str(result['base'])[:8] if result['base'] else '-'}): "
        f"{len(result['pending'])} within grace, 0 overdue."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
