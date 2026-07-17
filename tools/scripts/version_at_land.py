#!/usr/bin/env python3
"""version_at_land.py — single-writer version assignment on main (T1.1).

The problem: every `fix:`/`feat:` PR must hand-write a version number ABOVE
main's current value into `CMakeLists.txt` / `.claude-plugin/*.json`. That is a
shared counter — only one PR can own `main+1` — so N parallel PRs endlessly
re-bump and re-conflict on the VERSION line (the biggest single source of
merge-land thrash; see planning/2026-07-07-parallel-merge-land-coordination.md).

The fix: PRs stop hand-bumping. This bot — the single writer, running post-merge
on `main` — assigns the explicit semver once, in commit order, when it is the
sole writer. No two PRs ever contend for the same number.

To assign the RIGHT number the bot must reproduce exactly what the hand-bump
model wrote. That model does NOT read positive `Version-Bump:` trailers as its
signal — those are `skip`/override escapes used only a handful of times. It
derives the level from a PATH + CONVENTIONAL-COMMIT heuristic
(`version_bump_heuristics.assess_surfaces`: public-API paths → minor, other
touched source → patch, `feat:` → minor, `fix:`/`perf:` → patch, `BREAKING`/`!`
→ major), honoring `Version-Bump: <surface>=skip` and explicit
`<surface>=<level>` trailers as overrides. So this bot calls the SAME
`assess_surfaces` the gate and the `--mode=apply` writer call — there is one
heuristic, imported, never duplicated — and bumps each surface's base version by
the level it returns.

Rollout is staged and this script is safe at every stage:
  --dry-run (default): compute and print what it WOULD assign; write nothing.
    Run this while PRs still hand-bump — it changes nothing about releases.
  --apply: write the configured version files (the caller/workflow commits the
    result with a `Version-Bump-Applied:` marker so the next drain skips it).

It reuses the existing version machinery (versioning.json surfaces, the
`assess_surfaces` heuristic, version_at_base, write_version, bump_version) so
there is ONE source of truth for what a version file is, how a level is derived,
and how a level maps to a number.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path

from gate_common import git_diff_names
from version_bump_surfaces import Config, Surface, load_config, version_at_base
from version_bump_heuristics import assess_surfaces, filter_generated
from version_bump_apply import bump_version

# The next drain starts after the commit carrying this marker, so an applied
# bump is never re-assigned.
APPLIED_MARKER = "Version-Bump-Applied"


@dataclass(frozen=True)
class Assignment:
    surface: str
    level: str
    current: str
    assigned: str


def _current_at_base(base: str, surface: Surface) -> str | None:
    """The surface's version at `base` — the value the assignment bumps FROM.

    Reads the drain-window base (the last processed commit), NOT HEAD. Reading
    HEAD would be self-referential: any version-file edit already inside the
    range (a hand-bump during the transition, or the bot's own prior partial
    apply) would be read back as the "current" value and bumped a second time.
    The base is the last state the bot already accounted for, so bumping from it
    is idempotent. Mirrors `version_bump_apply.apply_bumps`, which computes its
    target from `version_at_base` for the same reason.
    """
    for vf in surface.version_files:
        cur = version_at_base(base, vf)
        if cur:
            return cur
    return None


def plan_assignments(config: Config, changed: list[str], base: str, head: str,
                     repo: Path) -> list[Assignment]:
    """For each surface the heuristic says needs a bump, what version to assign.

    Delegates the level decision to `assess_surfaces` — the identical pipeline
    the version-bump gate and the `--mode=apply` writer use — so the bot's
    assignment cannot drift from what a hand-bump would have written. A surface
    whose `final_level` is `none` (no meaningful touched source, or an explicit
    `Version-Bump: <surface>=skip`) gets no assignment.
    """
    verdicts = assess_surfaces(config, changed, base, head, repo)
    out: list[Assignment] = []
    for v in verdicts:
        if v.final_level == "none":
            continue
        # Bump FROM the base version, not the HEAD read that assess_surfaces
        # stored on the verdict (see _current_at_base). Fall back to the HEAD
        # read only when the file did not exist at base.
        current = _current_at_base(base, v.surface)
        if current is None:
            current = v.current_version
        if not current:
            continue
        out.append(Assignment(v.surface.name, v.final_level, current,
                              bump_version(current, v.final_level)))
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

    changed = filter_generated(git_diff_names(args.base, args.head),
                               config.generated_globs)
    plan = plan_assignments(config, changed, args.base, args.head, repo)

    surfaces_by_name: dict[str, Surface] = {s.name: s for s in config.surfaces}
    if args.mode == "apply":
        from version_bump_surfaces import write_version
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
