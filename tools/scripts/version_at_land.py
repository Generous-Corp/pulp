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
  --mode dry-run (default): compute and print what it WOULD assign; write
    nothing. Run this while PRs still hand-bump — it changes nothing about
    releases.
  --mode apply: write the configured version files in place (the historical
    "caller commits" shape, kept for callers that stage the commit themselves).
  --push: the hardened single-writer transaction — recompute from a fresh
    `origin/main`, write the files, commit with a `Version-Bump-Applied:`
    marker, and push with `--ff-only`, retrying (each retry recomputes from the
    new origin tip) so two near-simultaneous drains never lose or duplicate a
    version. This is what the workflow uses once flipped off dry-run.

It reuses the existing version machinery (versioning.json surfaces, read_version,
write_version, bump_version) so there is ONE source of truth for what a version
file is and how a level maps to a number.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from gate_common import git_range_trailers
from version_bump_surfaces import Config, Surface, load_config, read_version, write_version
from version_bump_apply import bump_version

# Highest-wins ordering. `skip` and absent mean "no assignment for this surface".
_RANK = {"patch": 1, "minor": 2, "major": 3}
TRAILER = "Version-Bump"
APPLIED_MARKER = "Version-Bump-Applied"
BUMP_SUBJECT = "chore: bump versions"


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


# ── Git helpers ────────────────────────────────────────────────────────────

def _git(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=check, capture_output=True, text=True,
    )


def _rev_parse(repo: Path, ref: str) -> str:
    return _git(repo, "rev-parse", ref).stdout.strip()


def last_applied_marker(repo: Path, ref: str) -> str | None:
    """Newest commit reachable from `ref` carrying a `Version-Bump-Applied`
    marker — the last state this bot already processed. None if none exists."""
    # Anchor to the trailer form (`Version-Bump-Applied:` at line start) so a
    # stray mention of the string in some other commit body is not mistaken for
    # a marker.
    out = _git(repo, "log", "-E", "--grep", f"^{APPLIED_MARKER}:",
               "-1", "--format=%H", ref, check=False)
    sha = out.stdout.strip()
    return sha or None


def drain_base(repo: Path, head: str, fallback: str | None = None) -> str:
    """The exclusive start of the drain range: the last applied marker if one
    exists (the authoritative "already processed up to here"); otherwise the
    caller's `fallback` (e.g. a push event's `before` SHA) when it is a valid,
    reachable ancestor; otherwise the commit before `head`. Bounded, non-empty."""
    marker = last_applied_marker(repo, head)
    if marker:
        return marker
    if fallback:
        ok = _git(repo, "merge-base", "--is-ancestor", fallback, head, check=False)
        if ok.returncode == 0:
            return fallback
    return f"{head}~1"


def intent_trailers(repo: Path, base: str, head: str) -> dict[str, list[str]]:
    """Intent trailers over `base..head`, EXCLUDING merge commits.

    A "Merge origin/main into <branch>" re-sync commit can carry a stale
    `Version-Bump:` intent that was never meant to declare this range's
    release; honoring it silently escalates the assigned version. Read intent
    only from the PRs' own (non-merge) commits and squash commits."""
    return git_range_trailers(base, head, no_merges=True, cwd=repo)


def plan_for_range(repo: Path, config: Config, base: str, head: str) -> list[Assignment]:
    """Concrete plan: intent over `base..head` (non-merge) × current versions
    read from the working tree (which the caller has synced to `head`)."""
    trailers = intent_trailers(repo, base, head)
    return plan_assignments(config, trailers,
                            lambda s: read_version(repo, s.version_files[0]))


def _write_plan(repo: Path, config: Config, plan: list[Assignment]) -> list[str]:
    surfaces_by_name: dict[str, Surface] = {s.name: s for s in config.surfaces}
    edited: list[str] = []
    for a in plan:
        for vf in surfaces_by_name[a.surface].version_files:
            if write_version(repo, vf, a.assigned):
                edited.append(vf.path)
    return edited


# ── Hardened single-writer push transaction ─────────────────────────────────

def apply_and_push(
    repo: Path,
    config: Config,
    remote: str = "origin",
    branch: str = "main",
    max_retries: int = 3,
    fallback_base: str | None = None,
    on_before_push: Callable[[int], None] | None = None,
) -> tuple[str, list[Assignment]]:
    """Recompute from a fresh `remote/branch`, write + commit the bump with a
    `Version-Bump-Applied` marker, and push `--ff-only`; retry on rejection.

    Returns `(status, plan)` where status is one of:
      "noop"    — no intent in the drained range; nothing pushed.
      "applied" — a bump commit was pushed to `remote/branch`.
      "exhausted" — every attempt lost the `--ff-only` race (should not happen
                    in practice; surfaced so the caller can fail loudly).

    Correctness under concurrency rests on three things:
      1. `--ff-only` — a second writer that advanced the branch between our
         recompute and our push is rejected, never silently clobbered (the
         `git push origin HEAD:main` bug in the deleted intent-bump workflow).
      2. recompute-per-attempt — each retry re-syncs to the new tip, so the
         drain range now starts AFTER the winner's marker and collapses to
         empty → we no-op instead of double-bumping.
      3. the `Version-Bump-Applied` marker — the next drain (and our own reruns)
         start after it, so an already-assigned range is never reprocessed.

    `on_before_push` is a test seam (advance the remote to force a non-ff race);
    production leaves it None.
    """
    for attempt in range(max_retries + 1):
        _git(repo, "fetch", "--quiet", remote, branch)
        head = _rev_parse(repo, f"{remote}/{branch}")
        # Hard-sync the working tree to the fetched tip so read_version sees the
        # authoritative current numbers. Ephemeral CI checkout — safe to reset.
        _git(repo, "reset", "--hard", head)

        base = drain_base(repo, head, fallback_base)
        plan = plan_for_range(repo, config, base, head)
        if not plan:
            return "noop", []

        edited = _write_plan(repo, config, plan)
        if not edited:
            # Intent declared but files already at/above target (a prior run
            # committed but the marker walk missed it). Treat as done, not a
            # spurious empty commit.
            return "noop", plan

        # Stage ONLY the version files we edited (never `git add -A` — this repo
        # carries a submodule gitlink that a blanket add would re-stage).
        _git(repo, "add", "--", *dict.fromkeys(edited))
        summary = "; ".join(f"{a.surface} {a.current}->{a.assigned} ({a.level})"
                            for a in plan)
        message = (
            f"{BUMP_SUBJECT}\n\n"
            f"Assigned from Version-Bump intent on {base[:12]}..{head[:12]}.\n"
            f"{summary}\n\n"
            f"{APPLIED_MARKER}: {head}\n"
        )
        _git(repo, "commit", "--no-verify", "-m", message)

        if on_before_push is not None:
            on_before_push(attempt)

        # No `--force`: `git push` refuses a non-fast-forward update by
        # default, which IS the `--ff-only` guarantee — a racing writer that
        # advanced `branch` after our recompute is rejected here, never
        # clobbered.
        pushed = _git(repo, "push", "--porcelain",
                      remote, f"HEAD:{branch}", check=False)
        if pushed.returncode == 0:
            return "applied", plan

        # Non-fast-forward (someone else advanced the branch) — discard our
        # local bump commit and retry from the new tip.
        _git(repo, "reset", "--hard", head)
    return "exhausted", []


# ── CLI ──────────────────────────────────────────────────────────────────────

def _repo_root() -> Path:
    return Path(subprocess.run(["git", "rev-parse", "--show-toplevel"],
                               check=True, capture_output=True, text=True).stdout.strip())


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base",
                    help="Start of the drain range (exclusive). Required in "
                         "--mode dry-run/apply. In --push it is only a FALLBACK "
                         "base (e.g. a push event's `before` SHA) used when the "
                         "bot has never run and no Version-Bump-Applied marker "
                         "exists yet; the marker takes precedence otherwise.")
    ap.add_argument("--head", default="HEAD", help="End of the range (default: HEAD).")
    ap.add_argument("--config", default="tools/scripts/versioning.json")
    ap.add_argument("--mode", choices=["dry-run", "apply"], default="dry-run",
                    help="dry-run (default): print, write nothing. apply: write "
                         "version files in place (caller commits).")
    ap.add_argument("--push", action="store_true",
                    help="Hardened single-writer transaction: recompute from a "
                         "fresh origin, write + commit with a Version-Bump-Applied "
                         "marker, and push --ff-only with retry. Ignores --base/"
                         "--head/--mode (derives the range from the remote).")
    ap.add_argument("--remote", default="origin")
    ap.add_argument("--branch", default="main")
    ap.add_argument("--max-retries", type=int, default=3)
    ap.add_argument("--json", action="store_true", help="Emit the plan as JSON.")
    args = ap.parse_args(argv)

    repo = _repo_root()
    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = repo / config_path
    config = load_config(config_path)

    if args.push:
        status, plan = apply_and_push(
            repo, config, remote=args.remote, branch=args.branch,
            max_retries=args.max_retries, fallback_base=args.base,
        )
        if args.json:
            print(json.dumps({"status": status,
                              "plan": [a.__dict__ for a in plan]}, indent=2))
        elif status == "noop":
            print("version-at-land: no version intent in range — nothing to push.")
        elif status == "applied":
            for a in plan:
                print(f"version-at-land: {a.surface} pushed {a.current} -> "
                      f"{a.assigned} ({a.level})")
        else:
            sys.stderr.write(
                "version-at-land: exhausted retries without a fast-forward push; "
                "the branch kept moving. Re-run.\n")
            return 1
        return 0

    if not args.base:
        ap.error("--base is required unless --push is given")

    # Read intent only from the PRs' own commits (non-merge) so a stray trailer
    # on a re-sync merge commit cannot escalate the assigned version.
    plan = plan_for_range(repo, config, args.base, args.head)

    if args.mode == "apply":
        _write_plan(repo, config, plan)

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
