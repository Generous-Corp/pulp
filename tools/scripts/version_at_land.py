#!/usr/bin/env python3
"""version_at_land.py — single-writer version assignment on main (T1.1).

The problem: every `fix:`/`feat:` PR must hand-write a version number ABOVE
main's current value into `CMakeLists.txt` / `.claude-plugin/*.json`. That is a
shared counter — only one PR can own `main+1` — so N parallel PRs endlessly
re-bump and re-conflict on the VERSION line (the biggest single source of
merge-land thrash; see planning/2026-07-07-parallel-merge-land-coordination.md).

The fix: PRs only DECLARE intent via `Version-Bump: <surface>=<level>` trailers.
This bot — the single writer, running post-merge on `main` — assigns the
explicit semver once, in commit order, when it is the sole writer. No two PRs
ever contend for the same number.

Rollout is staged and this script is safe at every stage:
  --dry-run (default): compute and print what it WOULD assign; write nothing.
    Run this while PRs still hand-bump — it changes nothing about releases.
  --apply: write the configured version files (the caller/workflow commits the
    result with a `Version-Bump-Applied:` marker so the next drain skips it).

It reuses the existing version machinery (versioning.json surfaces, read_version,
write_version, bump_version) so there is ONE source of truth for what a version
file is and how a level maps to a number.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from gate_common import git_range_trailers
from version_bump_surfaces import Config, Surface, load_config, read_version, write_version
from version_bump_apply import bump_version

# Highest-wins ordering. `skip` and absent mean "no assignment for this surface".
_RANK = {"patch": 1, "minor": 2, "major": 3}
TRAILER = "Version-Bump"
APPLIED_MARKER = "Version-Bump-Applied"


@dataclass(frozen=True)
class Assignment:
    surface: str
    level: str
    current: str
    assigned: str


def aggregate_intent(trailers: dict[str, list[str]], surface_name: str) -> str | None:
    """Highest declared bump level for `surface_name` across the range.

    Scans every `Version-Bump: <surface>=<level>` value and returns the
    max-rank level (major > minor > patch). `skip` and unknown levels are
    ignored — a surface with only `skip` (or no) intent gets no assignment.
    """
    best: str | None = None
    for value in trailers.get(TRAILER.lower(), []):
        m = re.search(rf"{re.escape(surface_name)}\s*=\s*([A-Za-z]+)", value)
        if not m:
            continue
        level = m.group(1).lower()
        if level not in _RANK:
            continue  # skip / unknown → not an assignment
        if best is None or _RANK[level] > _RANK[best]:
            best = level
    return best


def plan_assignments(config: Config, trailers: dict[str, list[str]],
                     current_version) -> list[Assignment]:
    """Pure: for each surface with a declared intent, what version to assign.

    `current_version(surface) -> str | None` reads the surface's current
    version (injected so this is testable without a repo).
    """
    out: list[Assignment] = []
    for surface in config.surfaces:
        level = aggregate_intent(trailers, surface.name)
        if level is None:
            continue
        current = current_version(surface)
        if not current:
            continue
        out.append(Assignment(surface.name, level, current,
                              bump_version(current, level)))
    return out


# ── CLI ──────────────────────────────────────────────────────────────────────

def _repo_root() -> Path:
    import subprocess
    return Path(subprocess.run(["git", "rev-parse", "--show-toplevel"],
                               check=True, capture_output=True, text=True).stdout.strip())


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", required=True,
                    help="Start of the drain range (exclusive) — the last "
                         "processed commit / bot marker.")
    ap.add_argument("--head", default="HEAD", help="End of the range (default: HEAD).")
    ap.add_argument("--config", default="tools/scripts/versioning.json")
    ap.add_argument("--mode", choices=["dry-run", "apply"], default="dry-run",
                    help="dry-run (default): print, write nothing. apply: write "
                         "version files (caller commits).")
    ap.add_argument("--json", action="store_true", help="Emit the plan as JSON.")
    args = ap.parse_args(argv)

    repo = _repo_root()
    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = repo / config_path
    config = load_config(config_path)

    trailers = git_range_trailers(args.base, args.head)
    plan = plan_assignments(config, trailers,
                            lambda s: read_version(repo, s.version_files[0]))

    surfaces_by_name: dict[str, Surface] = {s.name: s for s in config.surfaces}
    if args.mode == "apply":
        for a in plan:
            for vf in surfaces_by_name[a.surface].version_files:
                write_version(repo, vf, a.assigned)

    if args.json:
        print(json.dumps([a.__dict__ for a in plan], indent=2))
    elif not plan:
        print("version-at-land: no version intent in range — nothing to assign.")
    else:
        verb = "would assign" if args.mode == "dry-run" else "assigned"
        for a in plan:
            print(f"version-at-land: {a.surface} {verb} {a.current} -> {a.assigned} "
                  f"({a.level})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
