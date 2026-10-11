#!/usr/bin/env python3
"""Dry-run and idle-gated reaper for per-worktree planning submodule clones.

Only ``.git/worktrees/*/modules/planning`` directories belonging to a registered
Pulp worktree may be selected. The worktree must be clean, merged into
origin/main, idle, and unused by any process. Deletion requires ``--yes``.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def run_git(repo: Path, *args: str) -> str:
    p = subprocess.run(["git", "-C", str(repo), *args], text=True, encoding="utf-8",
                       capture_output=True, check=False)
    if p.returncode:
        raise RuntimeError(p.stderr.strip() or "git command failed")
    return p.stdout


def worktrees(repo: Path) -> list[tuple[Path, str, str]]:
    out = run_git(repo, "worktree", "list", "--porcelain")
    rows: list[tuple[Path, str, str]] = []
    path: Path | None = None
    head = branch = ""
    for line in out.splitlines() + [""]:
        if line.startswith("worktree "):
            if path is not None:
                rows.append((path, head, branch))
            path, head, branch = Path(line[9:]).resolve(), "", ""
        elif line.startswith("HEAD "):
            head = line[5:].strip()
        elif line.startswith("branch "):
            branch = line[7:].removeprefix("refs/heads/")
        elif not line and path is not None:
            rows.append((path, head, branch))
            path = None
    return rows


def aliases(path: Path) -> set[str]:
    values = {str(path), os.path.realpath(path)}
    raw = str(path)
    if raw.startswith("/private/tmp/"):
        values.add(raw.removeprefix("/private"))
    elif raw.startswith("/tmp/"):
        values.add("/private" + raw)
    return values


def process_snapshot() -> tuple[list[str], list[str]] | None:
    try:
        ps = subprocess.run(["ps", "-axo", "args="], text=True, encoding="utf-8",
                            capture_output=True, check=False)
    except OSError:
        return None
    if ps.returncode or not ps.stdout.strip():
        return None
    lsof: list[str] = []
    if shutil.which("lsof"):
        try:
            p = subprocess.run(["lsof", "-n", "-P", "-Fn"], text=True, encoding="utf-8",
                               capture_output=True, check=False)
            if p.returncode:
                return None
            lsof = p.stdout.splitlines()
        except OSError:
            return None
    else:
        return None
    return ps.stdout.splitlines(), lsof


def active(path: Path, snapshot: tuple[list[str], list[str]]) -> bool:
    needles = aliases(path)
    for line in snapshot[0]:
        if any(needle in line for needle in needles):
            return True
    current = ""
    for line in snapshot[1]:
        if line.startswith("p"):
            current = line[1:]
        elif line.startswith("n") and any(line[1:] == n or line[1:].startswith(n + "/") for n in needles):
            return True
    return False


def fresh(path: Path, cutoff: float) -> bool | None:
    try:
        for root, dirs, files in os.walk(path):
            for name in files + dirs:
                p = Path(root) / name
                if p.is_symlink():
                    return False
                if p.stat().st_mtime >= cutoff:
                    return False
    except OSError:
        return None
    return True


def dirty(wt: Path) -> bool | None:
    p = subprocess.run(["git", "-C", str(wt), "status", "--porcelain",
                        "--untracked-files=normal", "--ignore-submodules=all"],
                       text=True, encoding="utf-8", capture_output=True, check=False)
    if p.returncode:
        return None
    return bool(p.stdout.strip())


def planning_path(repo: Path, wt: Path, common: Path) -> Path | None:
    p = subprocess.run(["git", "-C", str(wt), "rev-parse", "--git-path",
                        "modules/planning"], text=True, encoding="utf-8", capture_output=True,
                       check=False)
    if p.returncode or not p.stdout.strip():
        return None
    candidate = Path(p.stdout.strip())
    if not candidate.is_absolute():
        candidate = wt / candidate
    # Reject a symlink before resolving it. A replacement symlink must never
    # redirect the reaper into a source tree or another worktree's modules.
    if candidate.is_symlink():
        return None
    candidate = candidate.resolve()
    expected = common / "worktrees"
    try:
        candidate.relative_to(expected)
    except ValueError:
        return None
    if candidate.name != "planning" or candidate.parent.name != "modules":
        return None
    if not candidate.is_dir() or candidate.is_symlink():
        return None
    return candidate


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--yes", action="store_true", help="remove eligible clones")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--idle-hours", type=int, default=2)
    args = ap.parse_args(argv)
    if args.idle_hours < 0:
        ap.error("--idle-hours must be non-negative")
    repo = Path(__file__).resolve().parents[2]
    try:
        common = Path(run_git(repo, "rev-parse", "--git-common-dir").strip())
        if not common.is_absolute():
            common = (repo / common).resolve()
        else:
            common = common.resolve()
        tip = run_git(repo, "rev-parse", "origin/main").strip()
        rows = worktrees(repo)
    except RuntimeError as exc:
        print(f"clean_worktree_planning: {exc}; nothing removed", file=sys.stderr)
        return 3
    snapshot = process_snapshot()
    if snapshot is None:
        print("clean_worktree_planning: process/cwd state is unreadable; nothing removed", file=sys.stderr)
        return 3
    cutoff = time.time() - args.idle_hours * 3600
    primary = rows[0][0] if rows else repo.resolve()
    found = reclaimable = removed = kept = 0
    for wt, head, branch in rows:
        if wt == primary or wt == repo.resolve():
            continue
        found += 1
        reason = ""
        target = planning_path(repo, wt, common)
        if target is None:
            reason = "planning clone is absent or outside this repository's worktree modules"
        elif branch == "main" or head == tip:
            reason = "primary/default-tip worktree"
        elif not head or subprocess.run(["git", "-C", str(wt), "merge-base", "--is-ancestor", head, tip], check=False).returncode:
            reason = "unmerged or unproven history"
        else:
            state = dirty(wt)
            if state is None:
                reason = "worktree status is unreadable"
            elif state:
                reason = "dirty worktree"
            elif active(wt, snapshot) or active(target, snapshot):
                reason = "active worktree or planning clone"
            else:
                idle = fresh(target, cutoff)
                if idle is None:
                    reason = "planning clone freshness is unreadable"
                elif not idle:
                    reason = "planning clone is not idle"
                else:
                    reclaimable += 1
                    size = subprocess.run(["du", "-sh", str(target)], text=True, encoding="utf-8",
                                          capture_output=True, check=False).stdout.split("\t", 1)[0]
                    if args.yes:
                        # Recheck the invariant immediately before removal.
                        if planning_path(repo, wt, common) != target or dirty(wt) or active(wt, snapshot) or active(target, snapshot):
                            reason = "safety state changed before removal"
                        else:
                            shutil.rmtree(target)
                            removed += 1
                            print(f"  removed {size}\t{target}")
                            continue
                    else:
                        print(f"  would remove {size}\t{target}")
                        continue
        kept += 1
        if args.verbose:
            print(f"  keeping ({reason})\t{wt}")
    mode = "removed" if args.yes else "reclaimable"
    print(f"clean_worktree_planning: {found} registered non-primary worktree(s); {reclaimable} {mode}; kept {kept}.")
    if not args.yes:
        print("clean_worktree_planning: re-run with --yes to delete; dry-run made no changes.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
