#!/usr/bin/env python3
"""Merge-time version bump from `Version-Bump:` INTENT trailers.

This is the merge-time half of the intent-trailer model (the gate half is
`version_bump_check.py --accept-intent-trailers`). Under that model a PR
declares its bump as a trailer and does NOT edit the version files, so two
PRs never collide on the version line — the "version-bump merge treadmill"
that re-conflicts every open branch each time `main` advances. The exact
number is assigned here, AFTER merge, from `main`'s current version.

Given the just-merged HEAD commit on `main`, this:

  1. parses its message for `Version-Bump: <surface>=<patch|minor|major>`
     trailers (the same surfaces as versioning.json: sdk, plugin, ...),
  2. for each declared surface, reads the CURRENT version from main and
     computes the next version at the declared level,
  3. writes every version file for that surface (keeping plugin.json and
     marketplace.json in lockstep).

It is idempotent: a surface whose files already advanced past HEAD~1 is
skipped, so a re-run (or a manual `--head` replay) does not double-bump.
CHANGELOG.md is intentionally NOT touched — Shipyard owns changelog regen
post-tag (see version_bump_apply.apply_bumps for the rationale).

The caller (the `intent-bump-on-merge` workflow) commits the staged files
as `chore: bump versions` and pushes to main, which triggers
`auto-release.yml` exactly as a PR-side bump does today.

Exit codes: 0 = bump written (and staged) OR nothing to do; 2 = config/usage
error. It never fails just because a commit carried no intent trailer — most
commits (docs, chores, merges) legitimately bump nothing.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Reuse the same surface config + version read/write/bump primitives the
# PR-side gate uses, so the two halves can never drift on file formats.
from version_bump_surfaces import (
    Config,
    Surface,
    load_config,
    read_version,
    write_version,
)
from version_bump_apply import bump_version

_LEVELS = ("patch", "minor", "major")
# `Version-Bump: <surface>=<level>` — one surface per trailer line. The skip
# form (`Version-Bump: skip reason="..."`) and bare reason text are ignored
# here: skip means "no release owed", which is the no-op default.
_TRAILER_RE = re.compile(
    r"^Version-Bump:\s*([A-Za-z0-9_-]+)\s*=\s*(patch|minor|major)\b",
    re.IGNORECASE,
)


def repo_root() -> Path:
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True, text=True, check=True,
    )
    return Path(out.stdout.strip())


def commit_message(repo: Path, ref: str) -> str:
    out = subprocess.run(
        ["git", "-C", str(repo), "log", "-1", "--format=%B", ref],
        capture_output=True, text=True, check=True,
    )
    return out.stdout


def parse_intents(message: str) -> dict[str, str]:
    """Return {surface: level}; last trailer wins for a repeated surface."""
    intents: dict[str, str] = {}
    for line in message.splitlines():
        m = _TRAILER_RE.match(line.strip())
        if m:
            intents[m.group(1).lower()] = m.group(2).lower()
    return intents


def _surface_already_bumped(repo: Path, surface: Surface, base_ref: str) -> bool:
    """True if any version file already differs from base_ref (prior run)."""
    for vf in surface.version_files:
        cur = read_version(repo, vf)
        try:
            prev = subprocess.run(
                ["git", "-C", str(repo), "show", f"{base_ref}:{vf.path}"],
                capture_output=True, text=True, check=True,
            ).stdout
        except subprocess.CalledProcessError:
            return False
        from version_bump_surfaces import _extract_version_from_text
        prev_ver = _extract_version_from_text(prev, vf)
        if cur is not None and prev_ver is not None and cur != prev_ver:
            return True
    return False


def apply_intent_bumps(
    repo: Path, cfg: Config, intents: dict[str, str], head: str, base: str
) -> list[str]:
    edited: list[str] = []
    by_name = {s.name: s for s in cfg.surfaces}
    for name, level in intents.items():
        surface = by_name.get(name)
        if surface is None:
            sys.stderr.write(
                f"apply_intent_bump: unknown surface '{name}' — skipping\n"
            )
            continue
        if _surface_already_bumped(repo, surface, base):
            sys.stderr.write(
                f"apply_intent_bump: surface '{name}' already bumped — skipping\n"
            )
            continue
        # Read the current (pre-bump) version from main and advance it.
        current = None
        for vf in surface.version_files:
            current = read_version(repo, vf)
            if current:
                break
        if not current:
            sys.stderr.write(
                f"apply_intent_bump: no readable version for surface '{name}'\n"
            )
            continue
        new = bump_version(current, level)
        if new == current:
            continue
        for vf in surface.version_files:
            if write_version(repo, vf, new):
                edited.append(vf.path)
        sys.stderr.write(
            f"apply_intent_bump: {name} {current} -> {new} ({level})\n"
        )
    if edited:
        subprocess.run(
            ["git", "-C", str(repo), "add", "--", *edited], check=False
        )
    return edited


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Apply Version-Bump intent trailers post-merge")
    ap.add_argument("--head", default="HEAD", help="commit carrying the intent trailers (default HEAD)")
    ap.add_argument("--base", default="HEAD~1", help="prior main commit, for idempotency (default HEAD~1)")
    ap.add_argument("--config", default=None)
    ap.add_argument("--repo-root", default=None)
    ap.add_argument(
        "--print-edited", action="store_true",
        help="print edited file paths to stdout (one per line) for the caller",
    )
    args = ap.parse_args(argv)

    root = Path(args.repo_root) if args.repo_root else repo_root()
    cfg_path = Path(args.config) if args.config else root / "tools" / "scripts" / "versioning.json"
    if not cfg_path.exists():
        sys.stderr.write(f"apply_intent_bump: config not found: {cfg_path}\n")
        return 2
    cfg = load_config(cfg_path)

    intents = parse_intents(commit_message(root, args.head))
    if not intents:
        sys.stderr.write("apply_intent_bump: no Version-Bump intent trailers — nothing to do\n")
        return 0

    edited = apply_intent_bumps(root, cfg, intents, args.head, args.base)
    if args.print_edited:
        for p in edited:
            print(p)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
