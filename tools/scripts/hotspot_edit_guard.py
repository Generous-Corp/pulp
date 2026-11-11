#!/usr/bin/env python3
"""Refuse a branch that edits a registered shared hotspot aggregate directly.

A handful of generated files used to take an edit from nearly every pull
request, so two independent pull requests conflicted on them. Each such file
is split into one fragment per entry, and its combined view is assembled at
read or check time instead of committed. This guard keeps it that way: a
change that adds or modifies a registered aggregate path (a branch cut before
the split that still edits it, a hand-written copy, a tool that writes the old
single file) is refused, and the message names the fragment directory and the
command that writes it. Deleting the aggregate is allowed, since that is the
migration itself.

A hotspot that was a repeated literal inside hand-written files rather than a
file of its own (a value copied onto many rows) registers `path` as a glob and
an `added_line_pattern`: a change that adds a line matching the pattern to a
matching file is refused the same way, while every other edit to those files
passes.

Registered hotspots live in `tools/scripts/hotspot_edit_guard.json`.

  hotspot_edit_guard.py [--base REF] [--head REV]

The change is `merge-base(base, head)..head`, plus uncommitted edits when the
head is the working tree's HEAD, so it runs before a commit as well as at the
pre-push hook and on the drift-fast tier. Base resolution: --base,
PULP_HOTSPOT_GUARD_BASE, origin/$GITHUB_BASE_REF, HEAD^1 in a merge group or
on a pull request's merge checkout, then origin/main. Without that event
context (a local run) it falls back to origin/main as the clone last fetched
it, so on a stale clone it judges the branch against an old main; fetch first.

Exit codes: 0 no registered hotspot edited; 1 a registered hotspot was added
or modified; 2 the registry or the git range could not be read, or no base
resolved; 77 no base resolved under --skip-without-base (the ctest
registration's SKIP_RETURN_CODE: a checkout with no base to diff against, such
as a scheduled run on main, has no branch edit to judge; a skip, not a pass).
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Callable

REGISTRY = Path("tools/scripts/hotspot_edit_guard.json")
SCHEMA = "pulp-hotspot-edit-guard/v1"


def _git(root: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def load_registry(root: Path, path: Path | None = None) -> list[dict]:
    """The registered hotspots; raises ValueError for an unusable registry."""
    path = path or root / REGISTRY
    doc = json.loads(path.read_text(encoding="utf-8"))
    if doc.get("schema") != SCHEMA:
        raise ValueError(f"{path}: schema {doc.get('schema')!r}, expected {SCHEMA!r}")
    hotspots = doc.get("hotspots")
    if not isinstance(hotspots, list) or not hotspots:
        raise ValueError(f"{path}: no hotspots registered")
    for spot in hotspots:
        for key in ("path", "fragments", "writer"):
            if not isinstance(spot.get(key), str) or not spot[key]:
                raise ValueError(f"{path}: hotspot {spot!r} has no {key}")
        pattern = spot.get("added_line_pattern")
        if pattern is not None:
            if not isinstance(pattern, str) or not pattern:
                raise ValueError(f"{path}: hotspot {spot['path']!r} has an empty added_line_pattern")
            try:
                re.compile(pattern)
            except re.error as exc:
                raise ValueError(f"{path}: hotspot {spot['path']!r} added_line_pattern: {exc}") from exc
    return hotspots


def _is_commit(root: Path, ref: str) -> bool:
    return _git(root, "rev-parse", "--verify", "--quiet", ref + "^{commit}").returncode == 0


def resolve_base(root: Path, explicit: str | None, env: dict[str, str] | None = None) -> str | None:
    env = os.environ if env is None else env
    candidates = [explicit, env.get("PULP_HOTSPOT_GUARD_BASE")]
    if env.get("GITHUB_BASE_REF"):
        candidates.append("origin/" + env["GITHUB_BASE_REF"])
    if env.get("GITHUB_EVENT_NAME") in ("merge_group", "pull_request"):
        # Both check out a merge commit whose first parent is the base.
        candidates.append("HEAD^1")
    candidates.append("origin/main")
    for c in candidates:
        if c and _is_commit(root, c):
            return c
    return None


def changes(root: Path, base: str, head: str) -> dict[str, str]:
    """{path: status letter} for the range, uncommitted edits included when head is HEAD."""
    mb = _git(root, "merge-base", base, head)
    anchor = mb.stdout.strip() if mb.returncode == 0 and mb.stdout.strip() else base
    out: dict[str, str] = {}
    ranges = [[anchor, head]]
    if head == "HEAD":
        ranges.append([anchor])  # the working tree (staged and unstaged) against the anchor
    for rng in ranges:
        res = _git(root, "diff", "--name-status", "--no-renames", "-z", *rng)
        if res.returncode != 0:
            raise RuntimeError(f"git diff {' '.join(rng)} failed: {res.stderr.strip()}")
        fields = res.stdout.split("\0")
        for status, path in zip(fields[0::2], fields[1::2]):
            if path:
                out[path] = status[:1]
    # A path the working tree re-created after a committed deletion is an add.
    if head == "HEAD":
        for path in list(out):
            if out[path] == "D" and (root / path).exists():
                out[path] = "A"
    return out


def added_lines(root: Path, base: str, head: str, path: str) -> list[str]:
    """Lines the range adds to `path`, uncommitted edits included when head is HEAD."""
    mb = _git(root, "merge-base", base, head)
    anchor = mb.stdout.strip() if mb.returncode == 0 and mb.stdout.strip() else base
    ranges = [[anchor, head]] if head != "HEAD" else [[anchor]]
    lines: list[str] = []
    for rng in ranges:
        res = _git(root, "diff", "--no-renames", "--no-color", "-U0", *rng, "--", path)
        if res.returncode != 0:
            raise RuntimeError(f"git diff {' '.join(rng)} -- {path} failed: {res.stderr.strip()}")
        lines.extend(line[1:] for line in res.stdout.splitlines()
                     if line.startswith("+") and not line.startswith("+++"))
    return lines


def violations(hotspots: list[dict], changed: dict[str, str],
               added: "Callable[[str], list[str]] | None" = None) -> list[tuple[dict, str]]:
    """(hotspot, status) for every refused edit; status "L" is an added line.

    `added(path)` returns the lines the range adds to a path; it is needed only
    by a hotspot with an added_line_pattern."""
    found: list[tuple[dict, str]] = []
    for spot in hotspots:
        pattern = spot.get("added_line_pattern")
        if pattern is None:
            if changed.get(spot["path"]) in ("A", "M", "T"):
                found.append((spot, changed[spot["path"]]))
            continue
        regex = re.compile(pattern)
        for path in sorted(changed):
            if changed[path] in ("A", "M", "T") and fnmatch.fnmatchcase(path, spot["path"]) \
                    and added is not None and any(regex.search(line) for line in added(path)):
                found.append(({**spot, "path": path}, "L"))
    return found


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(allow_abbrev=False, description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument("--base", default=None)
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--registry", default=None, help=f"default {REGISTRY}")
    ap.add_argument("--skip-without-base", action="store_true",
                    help="exit 77 (skipped) instead of 2 when no base resolves")
    a = ap.parse_args(argv[1:])
    root = Path(a.root).resolve()
    try:
        hotspots = load_registry(root, Path(a.registry) if a.registry else None)
    except (OSError, ValueError) as exc:
        print(f"hotspot-edit-guard: ERROR: cannot read the registry: {exc}", file=sys.stderr)
        return 2
    base = resolve_base(root, a.base)
    if base is None and a.skip_without_base:
        print("hotspot-edit-guard: SKIPPED: no base resolved (no --base, PULP_HOTSPOT_GUARD_BASE, "
              "pull-request or merge-group merge checkout, or origin/main); there is no branch edit "
              "to judge. This is a skip, not a pass.")
        return 77
    if base is None:
        print("hotspot-edit-guard: ERROR: no base resolved (pass --base, or fetch origin/main); "
              "file=" + REGISTRY.as_posix() + "; cause_class=NO_BASE; safe_to_auto_apply=no", file=sys.stderr)
        return 2
    try:
        changed = changes(root, base, a.head)
    except RuntimeError as exc:
        print(f"hotspot-edit-guard: ERROR: {exc}", file=sys.stderr)
        return 2
    try:
        bad = violations(hotspots, changed, lambda path: added_lines(root, base, a.head, path))
    except RuntimeError as exc:
        print(f"hotspot-edit-guard: ERROR: {exc}", file=sys.stderr)
        return 2
    if not bad:
        print(f"hotspot-edit-guard: OK, no registered hotspot edited ({len(hotspots)} registered; "
              f"{len(changed)} path(s) changed since {base})")
        return 0
    print(f"hotspot-edit-guard: {len(bad)} registered hotspot aggregate(s) edited directly "
          f"(against {base}):")
    for spot, status in bad:
        if status == "L":
            print(f"  file={spot['path']}; cause_class=HOTSPOT_LITERAL_EDIT; evidence=adds a line matching "
                  f"`{spot['added_line_pattern']}` on this branch; fix=drop that line and record the value "
                  f"under {spot['fragments']} with `{spot['writer']}`; safe_to_auto_apply=no; "
                  "verify=`python3 tools/scripts/hotspot_edit_guard.py`")
            if spot.get("why"):
                print(f"    why: {spot['why']}")
            continue
        verb = "re-added" if status == "A" else "modified"
        print(f"  file={spot['path']}; cause_class=HOTSPOT_AGGREGATE_EDIT; evidence={verb} on this branch; "
              f"fix=drop the edit (`git rm {spot['path']}` or `git checkout {base} -- {spot['path']}`), merge "
              f"origin/main, then write the entry's fragment under {spot['fragments']} with `{spot['writer']}`; "
              "safe_to_auto_apply=no; verify=`python3 tools/scripts/hotspot_edit_guard.py`")
        if spot.get("why"):
            print(f"    why: {spot['why']}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
