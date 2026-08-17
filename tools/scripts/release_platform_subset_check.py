#!/usr/bin/env python3
"""Flag a release platform subset that has quietly become permanent.

``release_product_matrix.json``'s ``active_platforms`` lets a release ship a
subset of the platform inventory (e.g. darwin-arm64 only) with a one-line
edit. That knob is meant to be TEMPORARY — the standing decision is "not more
than a week without the others" — and the documented failure mode of every
temporary CI change is that nobody flips it back. This check makes the
reminder enforceable: it fails once ``active_platforms`` has been a strict
subset of ``platforms`` for longer than the allowed age, dating the subset
from the commit that BEGAN the current continuous subset streak (flipping
back to full resets the clock; narrowing an existing subset does not).

Used by ``.github/workflows/release-platform-subset-check.yml`` (scheduled),
which opens/updates a tracking issue on failure. Runs standalone too:

    python3 tools/scripts/release_platform_subset_check.py
    python3 tools/scripts/release_platform_subset_check.py --max-age-days 7

Exit codes: 0 — full set active, or subset within its allowance;
1 — subset overdue; 2 — cannot evaluate (bad matrix, no git history).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MATRIX_RELPATH = "tools/scripts/release_product_matrix.json"


@dataclass(frozen=True)
class MatrixRevision:
    """One historical state of the matrix file, newest first."""

    sha: str
    committed_at: datetime
    active: frozenset[str]
    inventory: frozenset[str]

    @property
    def is_subset(self) -> bool:
        return self.active != self.inventory


def active_of(doc: dict) -> frozenset[str]:
    return frozenset(doc.get("active_platforms", doc["platforms"]))


def inventory_of(doc: dict) -> frozenset[str]:
    return frozenset(doc["platforms"])


def subset_since(revisions: list[MatrixRevision]) -> datetime | None:
    """When the CURRENT continuous subset streak began, or None if full now.

    ``revisions`` is newest-first. The streak begins at the newest revision
    below which a full-set revision (or the start of history) sits — so a
    flip back to full genuinely resets the clock, while narrowing or
    reshuffling an existing subset does not.
    """
    if not revisions or not revisions[0].is_subset:
        return None
    streak_start = revisions[0]
    for revision in revisions[1:]:
        if not revision.is_subset:
            break
        streak_start = revision
    return streak_start.committed_at


def git_revisions(repo_root: Path = REPO_ROOT) -> list[MatrixRevision]:
    log = subprocess.run(
        ["git", "log", "--format=%H %ct", "--", MATRIX_RELPATH],
        cwd=repo_root,
        capture_output=True,
        text=True,
        check=True,
    )
    revisions: list[MatrixRevision] = []
    for line in log.stdout.splitlines():
        sha, _, epoch = line.strip().partition(" ")
        if not sha or not epoch:
            continue
        show = subprocess.run(
            ["git", "show", f"{sha}:{MATRIX_RELPATH}"],
            cwd=repo_root,
            capture_output=True,
            text=True,
        )
        if show.returncode != 0:
            # The file did not exist at this commit (a rename boundary);
            # treat as the start of history.
            break
        try:
            doc = json.loads(show.stdout)
            revisions.append(
                MatrixRevision(
                    sha=sha,
                    committed_at=datetime.fromtimestamp(
                        int(epoch), tz=timezone.utc
                    ),
                    active=active_of(doc),
                    inventory=inventory_of(doc),
                )
            )
        except (json.JSONDecodeError, KeyError, ValueError):
            # An unparseable historical revision ends the walk; the streak is
            # judged from what parses.
            break
    return revisions


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--max-age-days", type=float, default=7.0)
    parser.add_argument(
        "--now",
        help="ISO-8601 override for the current time (tests)",
    )
    parser.add_argument(
        "--github-output",
        action="store_true",
        help="Also print key=value lines for $GITHUB_OUTPUT consumption",
    )
    args = parser.parse_args(argv)
    now = (
        datetime.fromisoformat(args.now).astimezone(timezone.utc)
        if args.now
        else datetime.now(timezone.utc)
    )

    try:
        doc = json.loads((REPO_ROOT / MATRIX_RELPATH).read_text(encoding="utf-8"))
        active = active_of(doc)
        inventory = inventory_of(doc)
    except (OSError, json.JSONDecodeError, KeyError) as exc:
        print(f"ERROR: cannot read {MATRIX_RELPATH}: {exc}", file=sys.stderr)
        return 2

    missing = sorted(inventory - active)
    if not missing:
        print("OK: active_platforms covers the full platform inventory.")
        if args.github_output:
            print("overdue=false")
            print("missing=[]")
        return 0

    try:
        since = subset_since(git_revisions())
    except subprocess.CalledProcessError as exc:
        print(f"ERROR: git history unavailable: {exc}", file=sys.stderr)
        return 2
    if since is None:
        # The working tree is a subset but no committed revision is — the
        # subset is uncommitted or brand new this commit.
        since = now

    age = now - since
    overdue = age > timedelta(days=args.max_age_days)
    days = age.total_seconds() / 86400
    print(
        f"active_platforms is a strict subset (missing {missing}) "
        f"since {since.isoformat()} — {days:.1f} days "
        f"(allowance {args.max_age_days:g} days)."
    )
    if args.github_output:
        print(f"overdue={'true' if overdue else 'false'}")
        print(f"missing={json.dumps(missing)}")
        print(f"since={since.isoformat()}")
        print(f"age_days={days:.1f}")
    if overdue:
        print(
            "OVERDUE: the platform subset has outlived its allowance. Restore "
            "the paused platforms in tools/scripts/release_product_matrix.json "
            "(delete active_platforms, or add them back piecemeal), or "
            "explicitly renew the decision.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
