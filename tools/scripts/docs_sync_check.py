#!/usr/bin/env python3
"""Docs-sync gate — #566 Phase 4 / #567.

Generalizes the skill-sync pattern to arbitrary docs. Given a diff
range (base..HEAD), assert that every doc whose mapped paths were
touched also has its doc file updated in the same range — or carries
a `Docs-Update: skip doc=<name> reason="..."` trailer on the tip
commit.

Uses the same zero-dependency JSON path-map + fnmatch-glob machinery
as tools/scripts/skill_sync_check.py; the two scripts are deliberately
parallel so an agent or contributor reading one can transfer the
pattern immediately.

Modes:
    --mode=report  (CI default)  Exit 1 if any doc is out of sync.
    --mode=hint    (agent hooks) Exit 0 always; print hints only.

Trailer bypass:
    Docs-Update: skip doc=<name> reason="..."  on the tip commit.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


# ── Types ──────────────────────────────────────────────────────────────


@dataclass
class DocEntry:
    name: str
    path: str
    paths: tuple[str, ...]


@dataclass
class Finding:
    doc: DocEntry
    touched_paths: list[str]
    doc_modified: bool
    bypass_reason: str | None


# ── Git helpers ────────────────────────────────────────────────────────


def repo_root() -> Path:
    return Path(
        subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True
        ).strip()
    )


def diff_files(base_ref: str) -> list[str]:
    # Let CalledProcessError propagate — a missing/unfetched base ref
    # (common failure mode locally) must fail loud, not silently exit 0
    # and claim "nothing to verify." Codex #613 P2.
    out = subprocess.check_output(
        ["git", "diff", "--name-only", f"{base_ref}...HEAD"], text=True
    )
    return [line.strip() for line in out.splitlines() if line.strip()]


def range_commit_messages(base_ref: str) -> str:
    """Concatenate every commit message in base..HEAD.

    CI pull_request events check out a synthetic merge commit as HEAD,
    so a `Docs-Update: skip` trailer on the branch tip wouldn't be
    visible via `git log -1`. Walk the whole range so trailers on any
    commit in the PR count as bypasses. Mirrors the
    `git_range_trailers` pattern in skill_sync_check.py. Codex #613 P1.
    """
    try:
        return subprocess.check_output(
            ["git", "log", "--format=%B", f"{base_ref}..HEAD"], text=True
        )
    except subprocess.CalledProcessError:
        return ""


# ── Map + matching ─────────────────────────────────────────────────────


def load_map(path: Path) -> list[DocEntry]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if int(data.get("schema_version", 0)) != 1:
        raise ValueError(f"{path}: unsupported schema_version")
    out: list[DocEntry] = []
    for name, entry in data.get("docs", {}).items():
        out.append(
            DocEntry(
                name=name,
                path=str(entry["path"]),
                paths=tuple(str(p) for p in entry["paths"]),
            )
        )
    return out


def matches_any(relpath: str, patterns: Iterable[str]) -> bool:
    for pat in patterns:
        if fnmatch.fnmatch(relpath, pat):
            return True
        # Accept `dir/**` as a prefix-style match against `dir/`.
        prefix = pat.rstrip("*").rstrip("/")
        if prefix and relpath.startswith(prefix + "/"):
            return True
    return False


# ── Trailer parsing ────────────────────────────────────────────────────


_TRAILER_RE = re.compile(
    r'^Docs-Update:\s*skip\s+doc=(\S+)\s+reason="([^"]+)"\s*$',
    re.MULTILINE,
)


def parse_bypass_trailers(message: str) -> dict[str, str]:
    return {m.group(1): m.group(2) for m in _TRAILER_RE.finditer(message)}


# ── Core check ─────────────────────────────────────────────────────────


def evaluate(
    docs: list[DocEntry],
    diff: list[str],
    message: str,
) -> list[Finding]:
    bypass = parse_bypass_trailers(message)
    findings: list[Finding] = []
    for doc in docs:
        touched = [p for p in diff if matches_any(p, doc.paths)]
        if not touched:
            continue
        doc_modified = doc.path in diff
        findings.append(
            Finding(
                doc=doc,
                touched_paths=touched,
                doc_modified=doc_modified,
                bypass_reason=bypass.get(doc.name),
            )
        )
    return findings


def render(findings: list[Finding]) -> tuple[str, bool]:
    """Return (report, all_good)."""
    lines: list[str] = []
    all_good = True
    for f in findings:
        if f.doc_modified:
            lines.append(f"[{f.doc.name}] ✓ updated ({f.doc.path})")
        elif f.bypass_reason:
            lines.append(f"[{f.doc.name}] ✓ bypassed ({f.bypass_reason})")
        else:
            all_good = False
            lines.append(f"[{f.doc.name}] ✗ {f.doc.path} NOT updated")
            for p in f.touched_paths[:5]:
                lines.append(f"    {p}")
            if len(f.touched_paths) > 5:
                lines.append(f"    ... +{len(f.touched_paths) - 5} more")
    if not findings:
        lines.append("docs-sync: no mapped paths touched — nothing to verify.")
    return "\n".join(lines), all_good


# ── Main ───────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default="origin/main",
                        help="Base ref for diff (default: origin/main)")
    parser.add_argument("--config", type=Path,
                        default=Path("tools/scripts/docs_sync_map.json"))
    parser.add_argument("--mode", choices=["report", "hint"], default="report",
                        help="report = hard-fail on drift; hint = advisory only")
    args = parser.parse_args(argv)

    os.chdir(repo_root())
    docs = load_map(args.config)
    try:
        diff = diff_files(args.base)
    except subprocess.CalledProcessError as exc:
        print(f"docs-sync: git diff against '{args.base}' failed: {exc}",
              file=sys.stderr)
        print("  Fetch the base ref (e.g. `git fetch origin main`) and retry.",
              file=sys.stderr)
        return 2
    message = range_commit_messages(args.base)
    findings = evaluate(docs, diff, message)
    report, all_good = render(findings)
    print(report)

    if not all_good and args.mode == "report":
        print("\nDocs-sync check FAILED.", file=sys.stderr)
        print("For each doc above marked ✗:", file=sys.stderr)
        print("  1. Update the doc file in this branch to reflect the code "
              "change, OR", file=sys.stderr)
        print("  2. Add a commit trailer on the tip commit with the exact "
              "form:", file=sys.stderr)
        print("     Docs-Update: skip doc=<name> reason=\"...\"",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
