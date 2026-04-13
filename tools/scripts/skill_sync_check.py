#!/usr/bin/env python3
"""Skill-sync gate.

Given a diff range (base..head), check that every skill whose mapped paths
were touched also has its SKILL.md updated in the same range — or has a
`Skill-Update: skip skill=<name> reason="..."` trailer on the tip commit.

Authoritative gate for CI; also runs as a local pre-push hook (advisory)
and from agent hooks in hint mode. Source of truth for the whole family.
See tools/scripts/versioning.json and tools/scripts/skill_path_map.json.

Uses JSON (not YAML) for zero-dependency execution on PEP-668 Python.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


# ── Types ───────────────────────────────────────────────────────────────


@dataclass
class Config:
    skills_dir: Path
    path_map_file: Path
    generated_globs: list[str] = field(default_factory=list)
    trailer_skill_update: str = "Skill-Update"


@dataclass
class SkillMap:
    skills: dict[str, list[str]]


@dataclass
class Finding:
    skill: str
    touched_paths: list[str]
    skill_md_modified: bool
    bypass_reason: str | None


# ── Git helpers ─────────────────────────────────────────────────────────


def repo_root() -> Path:
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        check=True, capture_output=True, text=True,
    )
    return Path(out.stdout.strip())


def git_diff_names(base: str, head: str) -> list[str]:
    out = subprocess.run(
        ["git", "diff", "--name-only", f"{base}..{head}"],
        check=True, capture_output=True, text=True,
    )
    return [line for line in out.stdout.splitlines() if line.strip()]


def git_commit_trailers(ref: str) -> dict[str, list[str]]:
    try:
        body = subprocess.run(
            ["git", "log", "-1", "--format=%B", ref],
            check=True, capture_output=True, text=True,
        ).stdout
    except subprocess.CalledProcessError:
        return {}

    trailers = subprocess.run(
        ["git", "interpret-trailers", "--parse"],
        input=body, capture_output=True, text=True,
    )
    result: dict[str, list[str]] = {}
    for line in trailers.stdout.splitlines():
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        result.setdefault(key.strip().lower(), []).append(value.strip())
    return result


# ── Config loading ──────────────────────────────────────────────────────


def _strip_comments(data: dict) -> dict:
    # Drop top-level `_comment` and `$schema` keys to keep in-memory data tidy.
    if isinstance(data, dict):
        return {k: v for k, v in data.items() if not k.startswith("_") and k != "$schema"}
    return data


def load_config(path: Path, repo: Path) -> Config:
    data = _strip_comments(json.loads(path.read_text()))
    skills_section = data.get("skills", {})
    trailers = data.get("trailers", {})

    skills_dir = Path(skills_section["skills_dir"])
    if not skills_dir.is_absolute():
        skills_dir = repo / skills_dir

    path_map_file = Path(skills_section["path_map"])
    if not path_map_file.is_absolute():
        path_map_file = repo / path_map_file

    return Config(
        skills_dir=skills_dir,
        path_map_file=path_map_file,
        generated_globs=data.get("generated_globs", []) or [],
        trailer_skill_update=trailers.get("skill_update", "Skill-Update"),
    )


def load_skill_map(path: Path) -> SkillMap:
    data = _strip_comments(json.loads(path.read_text()))
    raw = data.get("skills", {}) or {}
    skills: dict[str, list[str]] = {}
    for name, entry in raw.items():
        paths = entry.get("paths", []) if isinstance(entry, dict) else []
        skills[name] = list(paths)
    return SkillMap(skills=skills)


# ── Matching ────────────────────────────────────────────────────────────


def _glob_match(path: str, pattern: str) -> bool:
    """fnmatch-with-`**` support.

    fnmatch treats `*` as matching `/`, which is already what we want for
    `**` semantics — but we also need `**/X` to match `X` at the repo root.
    Normalize the pattern so that `**` expands to allow any segment count,
    including zero.
    """
    # Normalize `**/` at the start to match both `foo/X` and `X`.
    if pattern.startswith("**/"):
        tail = pattern[3:]
        return fnmatch.fnmatchcase(path, tail) or fnmatch.fnmatchcase(path, pattern)
    return fnmatch.fnmatchcase(path, pattern)


def _matches_any(path: str, patterns: Iterable[str]) -> bool:
    p = path.replace(os.sep, "/")
    for pat in patterns:
        if _glob_match(p, pat):
            return True
    return False


def filter_generated(changed: list[str], globs: Iterable[str]) -> list[str]:
    gs = list(globs)
    return [f for f in changed if not _matches_any(f, gs)]


def parse_skill_update_trailer(trailers: dict[str, list[str]], key: str) -> dict[str, str]:
    bypasses: dict[str, str] = {}
    for v in trailers.get(key.lower(), []):
        if not v.lower().startswith("skip"):
            continue
        m = re.search(r"skill\s*=\s*([A-Za-z0-9_.-]+)", v)
        if not m:
            continue
        skill = m.group(1)
        reason_m = (
            re.search(r'reason\s*=\s*"([^"]*)"', v)
            or re.search(r"reason\s*=\s*(\S+)", v)
        )
        reason = reason_m.group(1) if reason_m else "(no reason given)"
        bypasses[skill] = reason
    return bypasses


# ── Core check ──────────────────────────────────────────────────────────


def self_check(skill_map: SkillMap, skills_dir: Path) -> list[str]:
    errors: list[str] = []
    if not skills_dir.exists():
        return errors
    for entry in sorted(skills_dir.iterdir()):
        if not entry.is_dir():
            continue
        if entry.name not in skill_map.skills:
            errors.append(
                f"self-check: skill directory '{entry.name}' has no entry in skill_path_map.json"
            )
    for name in skill_map.skills:
        if not (skills_dir / name).exists():
            errors.append(
                f"self-check: skill_path_map.json references missing skill '{name}'"
            )
    return errors


def compute_findings(
    changed: list[str],
    skill_map: SkillMap,
    skills_dir: Path,
    repo: Path,
    bypasses: dict[str, str],
) -> list[Finding]:
    findings: list[Finding] = []
    try:
        rel = skills_dir.relative_to(repo)
    except ValueError:
        rel = skills_dir
    # str.lstrip removes *characters* not a prefix, which would eat the
    # leading '.' in '.agents/skills'. Normalize manually instead.
    rel_skills_dir = str(rel).replace(os.sep, "/")
    if rel_skills_dir.startswith("./"):
        rel_skills_dir = rel_skills_dir[2:]

    for skill, patterns in skill_map.skills.items():
        touched = [p for p in changed if _matches_any(p, patterns)]
        if not touched:
            continue
        skill_md_prefix = f"{rel_skills_dir}/{skill}/"
        skill_md_modified = any(p.startswith(skill_md_prefix) for p in changed)
        findings.append(
            Finding(
                skill=skill,
                touched_paths=sorted(touched),
                skill_md_modified=skill_md_modified,
                bypass_reason=bypasses.get(skill),
            )
        )
    return findings


# ── Reporting ───────────────────────────────────────────────────────────


def render_report(
    findings: list[Finding],
    trailer_key: str,
    mode: str,
) -> tuple[str, int]:
    lines: list[str] = []
    hard_failures: list[Finding] = []

    for f in findings:
        if f.skill_md_modified:
            status = "✓ SKILL.md updated"
        elif f.bypass_reason is not None:
            status = f'✓ bypassed ({f.bypass_reason})'
        else:
            status = "✗ SKILL.md NOT updated"
            hard_failures.append(f)
        lines.append(f"[{f.skill}] {status}")
        if mode != "report" or not f.skill_md_modified:
            for tp in f.touched_paths[:8]:
                lines.append(f"    {tp}")
            if len(f.touched_paths) > 8:
                lines.append(f"    … {len(f.touched_paths) - 8} more")

    if hard_failures and mode in ("report", "apply"):
        lines.append("")
        lines.append("Skill-sync check FAILED.")
        lines.append("For each skill above marked ✗:")
        lines.append("  1. Add a gotcha / note to the skill's SKILL.md in this branch, OR")
        lines.append("  2. Add a commit trailer on the tip commit with the exact form:")
        for f in hard_failures:
            lines.append(
                f'     {trailer_key}: skip skill={f.skill} reason="..."'
            )
        return "\n".join(lines), 1

    if not findings:
        lines.append("skill-sync: no mapped paths touched — nothing to verify.")
    return "\n".join(lines), 0


# ── Main ────────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Skill-sync gate")
    parser.add_argument("--base", default="origin/main",
                        help="Diff base (default: origin/main)")
    parser.add_argument("--head", default="HEAD",
                        help="Diff head (default: HEAD)")
    parser.add_argument(
        "--config",
        default=None,
        help="Path to versioning.json (default: tools/scripts/versioning.json at repo root)",
    )
    parser.add_argument(
        "--mode",
        choices=("report", "hint", "apply"),
        default="report",
        help="report: fail on issues (CI); hint: advisory text only; apply: same as report (skill content cannot be auto-generated)",
    )
    parser.add_argument(
        "--repo-root",
        default=None,
        help="Override repo root (default: git rev-parse --show-toplevel)",
    )
    args = parser.parse_args(argv)

    root = Path(args.repo_root) if args.repo_root else repo_root()
    cfg_path = (
        Path(args.config) if args.config
        else root / "tools" / "scripts" / "versioning.json"
    )
    if not cfg_path.exists():
        sys.stderr.write(f"skill_sync_check: config not found: {cfg_path}\n")
        return 2

    cfg = load_config(cfg_path, root)

    skill_map = load_skill_map(cfg.path_map_file)

    self_errors = self_check(skill_map, cfg.skills_dir)
    if self_errors:
        print("\n".join(self_errors), file=sys.stderr)
        if args.mode in ("report", "apply"):
            return 1

    changed = git_diff_names(args.base, args.head)
    changed = filter_generated(changed, cfg.generated_globs)

    trailers = git_commit_trailers(args.head)
    bypasses = parse_skill_update_trailer(trailers, cfg.trailer_skill_update)

    findings = compute_findings(
        changed=changed,
        skill_map=skill_map,
        skills_dir=cfg.skills_dir,
        repo=root,
        bypasses=bypasses,
    )

    text, code = render_report(findings, cfg.trailer_skill_update, args.mode)
    if text:
        print(text)

    if args.mode == "hint":
        return 0
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
