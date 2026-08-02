#!/usr/bin/env python3
"""Lint for ``tools/scripts/skill_path_map.json``.

The map decides which SKILL.md a diff is required to update. A rule in it
that quietly matches nothing does not report a problem — it reports
nothing at all, which reads exactly like a clean run. Three such rules
sat on main unnoticed, and the file's own contract test was wired into
no CI step, so its redness was invisible too.

This script is the wired instrument. It runs whole-tree (not diff-scoped)
for the rules that catch rot the tree drifted into, and diff-scoped for
the rule that catches a claim being widened. It is pure stdlib and
sub-second, and it runs from ``gates.sh``, ``.githooks/pre-push``, and
the ``version-skill-check`` workflow.

Rules
-----
``schema``
    The map validates against ``skill_path_map.schema.json``, and that
    file exists. Without this, a whole-subsystem glob or a bare-array
    entry is well-formed JSON and passes unremarked.

``submodule``
    No pattern may be rooted inside a git submodule. The superproject's
    diff carries the gitlink (``planning``), never a path beneath it, so
    such a pattern cannot fire for any commit — it is unreachable by
    construction rather than merely empty today. No annotation unlocks
    it, because no annotation can make it reachable.

``empty``
    Every pattern matches at least one tracked file, unless the entry
    carries a ``_doc.empty-ok`` annotation. The annotation is restricted
    to ``external/`` paths (developer-supplied SDKs) and to entries with
    no patterns at all (process skills that own no source path), so it
    cannot be used to wave through in-tree rot.

``co-claim``
    A pattern claiming an entire second-level directory tree
    (``<root>/<sub>/**``) may not be *newly* claimed by a skill when
    another skill already claims it, unless the entry explains itself
    with ``_doc.scope``. Two skills owning a whole subsystem makes every
    edit in it demand two SKILL.md updates, which trains reflexive
    ``Skill-Update: skip`` trailers — and that reflex is how a genuinely
    missed skill update gets waved through. Diff-scoped, so it ratchets:
    existing claims are left to their owners, new ones must justify.

Usage
-----
    python3 tools/scripts/skill_path_map_lint.py               # all rules
    python3 tools/scripts/skill_path_map_lint.py --base main
    python3 tools/scripts/skill_path_map_lint.py --rules co-claim \\
        --base-map OLD.json --head-map NEW.json
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import json_schema_lite  # noqa: E402  — path inserted above
from gate_common import glob_match as _glob_match  # noqa: E402

MAP_RELPATH = "tools/scripts/skill_path_map.json"
SCHEMA_RELPATH = "tools/scripts/skill_path_map.schema.json"

ALL_RULES = ("schema", "submodule", "empty", "co-claim")


# ── Repo state ──────────────────────────────────────────────────────────


def index_entries(repo: Path, rev: str | None = None) -> tuple[list[str], list[str]]:
    """Return ``(tracked_files, submodule_paths)`` for ``rev`` (or the index).

    Submodules appear in the index with mode 160000 and are *excluded*
    from the tracked-file list — no path beneath them is ever tracked
    here, which is what makes a pattern rooted in one unreachable.
    """
    if rev:
        cmd = ["git", "ls-tree", "-r", "--full-tree", rev]
    else:
        cmd = ["git", "ls-files", "--stage"]
    out = subprocess.run(
        cmd, cwd=repo, text=True, capture_output=True, check=False
    )
    if out.returncode != 0:
        return [], []

    files: list[str] = []
    submodules: list[str] = []
    for line in out.stdout.splitlines():
        meta, _, path = line.partition("\t")
        if not path:
            continue
        mode = meta.split()[0] if meta else ""
        (submodules if mode == "160000" else files).append(path)
    return files, submodules


def read_blob(repo: Path, rev: str, relpath: str) -> str | None:
    out = subprocess.run(
        ["git", "show", f"{rev}:{relpath}"],
        cwd=repo,
        text=True,
        capture_output=True,
        check=False,
    )
    return out.stdout if out.returncode == 0 else None


# ── Map access ──────────────────────────────────────────────────────────


def entry_paths(entry: object) -> list[str]:
    """Patterns for one entry, mirroring ``skill_sync_check.load_skill_map``.

    A non-object entry yields zero patterns there, so it must yield zero
    here too — the two must never disagree about what a map means.
    """
    if isinstance(entry, dict):
        return [p for p in (entry.get("paths") or []) if isinstance(p, str)]
    return []


def entry_doc(entry: object, key: str) -> str | None:
    if not isinstance(entry, dict):
        return None
    doc = entry.get("_doc")
    if not isinstance(doc, dict):
        return None
    value = doc.get(key)
    return value if isinstance(value, str) and value.strip() else None


def owners(skill_map: dict) -> dict[str, list[str]]:
    """Pattern -> every skill that claims it."""
    out: dict[str, list[str]] = {}
    for skill, entry in skill_map.get("skills", {}).items():
        for pattern in entry_paths(entry):
            out.setdefault(pattern, []).append(skill)
    return out


def is_subsystem_claim(pattern: str) -> bool:
    """True for ``<root>/<sub>/**`` — an entire second-level directory tree."""
    parts = pattern.split("/")
    return len(parts) == 3 and parts[-1] == "**" and "*" not in "".join(parts[:2])


# ── Rules ───────────────────────────────────────────────────────────────


def check_schema(repo: Path, skill_map: dict, schema_path: Path) -> list[str]:
    if not schema_path.exists():
        return [
            f"schema: {SCHEMA_RELPATH} does not exist, so the map's "
            f'"$schema" pointer validates nothing.'
        ]
    schema = json.loads(schema_path.read_text())

    declared = skill_map.get("$schema")
    if isinstance(declared, str):
        target = (repo / MAP_RELPATH).parent / declared
        if not target.exists():
            return [
                f"schema: map declares \"$schema\": {declared!r}, which "
                f"resolves to a file that does not exist ({target})."
            ]

    try:
        violations = json_schema_lite.validate(skill_map, schema)
    except json_schema_lite.UnsupportedKeyword as exc:
        return [f"schema: {exc}"]
    return [f"schema: {v}" for v in violations]


def check_submodule(skill_map: dict, submodules: list[str]) -> list[str]:
    errors: list[str] = []
    for skill, entry in sorted(skill_map.get("skills", {}).items()):
        for pattern in entry_paths(entry):
            for sub in submodules:
                if pattern == sub or pattern.startswith(sub + "/"):
                    errors.append(
                        f"submodule: {skill}: {pattern!r} points inside the "
                        f"{sub!r} submodule. The superproject's diff only "
                        f"ever names {sub!r} itself, so this pattern can "
                        f"never match — drop it, or map the skill to a path "
                        f"tracked in this repo."
                    )
                    break
    return errors


def check_empty(
    skill_map: dict, files: list[str], submodules: list[str] = ()
) -> list[str]:
    errors: list[str] = []
    for skill, entry in sorted(skill_map.get("skills", {}).items()):
        patterns = entry_paths(entry)
        annotated = entry_doc(entry, "empty-ok")
        for pattern in patterns:
            if any(_glob_match(f, pattern) for f in files):
                continue
            # The submodule rule already reports these, with the reason
            # that actually explains them. One violation per defect.
            if any(
                pattern == s or pattern.startswith(s + "/") for s in submodules
            ):
                continue
            if annotated and pattern.startswith("external/"):
                continue
            hint = (
                " The entry's _doc.empty-ok annotation does not cover it: "
                "that escape hatch is for developer-supplied external/ SDK "
                "paths only."
                if annotated
                else ""
            )
            errors.append(
                f"empty: {skill}: {pattern!r} matches no tracked file, so "
                f"no diff can ever trigger this skill through it. Point it "
                f"at where the code moved, or drop it.{hint}"
            )
        if not patterns and not annotated:
            errors.append(
                f"empty: {skill}: claims no paths and has no _doc.empty-ok "
                f"annotation. A skill that owns no source path must say so."
            )
    return errors


def check_co_claim(base_map: dict | None, head_map: dict) -> list[str]:
    """Diff-scoped: a NEW claim on a subsystem another skill already owns."""
    if base_map is None:
        return []

    before = {
        (skill, pattern)
        for skill, entry in base_map.get("skills", {}).items()
        for pattern in entry_paths(entry)
    }
    head_owners = owners(head_map)

    errors: list[str] = []
    for skill, entry in sorted(head_map.get("skills", {}).items()):
        for pattern in entry_paths(entry):
            if (skill, pattern) in before:
                continue
            if not is_subsystem_claim(pattern):
                continue
            others = [s for s in head_owners.get(pattern, []) if s != skill]
            if not others:
                continue
            if entry_doc(entry, "scope"):
                continue
            errors.append(
                f"co-claim: {skill}: {pattern!r} claims an entire subsystem "
                f"that {', '.join(sorted(others))} already claims. Every edit "
                f"under it would then demand {len(others) + 1} SKILL.md "
                f"updates, which trains reflexive 'Skill-Update: skip' "
                f"trailers. Narrow the pattern to the surfaces this skill "
                f"actually documents, or add a _doc.scope annotation saying "
                f"why the whole subsystem belongs to it too."
            )
    return errors


# ── Driver ──────────────────────────────────────────────────────────────


def run(
    repo: Path,
    rules: tuple[str, ...],
    head_map: dict,
    base_map: dict | None,
    files: list[str],
    submodules: list[str],
    schema_path: Path,
) -> list[str]:
    errors: list[str] = []
    if "schema" in rules:
        errors += check_schema(repo, head_map, schema_path)
    if "submodule" in rules:
        errors += check_submodule(head_map, submodules)
    if "empty" in rules:
        errors += check_empty(head_map, files, submodules)
    if "co-claim" in rules:
        errors += check_co_claim(base_map, head_map)
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="skill_path_map.json lint")
    parser.add_argument("--repo-root", default=None)
    parser.add_argument(
        "--base",
        default="origin/main",
        help="Diff base for the co-claim rule. Empty string disables it.",
    )
    parser.add_argument("--head-map", default=None, help="Override the map file.")
    parser.add_argument(
        "--base-map",
        default=None,
        help="Override the base-side map (skips the git lookup). Lets a "
        "historical pair of map revisions be replayed directly.",
    )
    parser.add_argument("--schema", default=None)
    parser.add_argument(
        "--rules",
        default=",".join(ALL_RULES),
        help=f"Comma-separated subset of: {', '.join(ALL_RULES)}",
    )
    parser.add_argument(
        "--mode",
        choices=("report", "hint"),
        default="report",
        help="report: non-zero exit on violations; hint: advisory only",
    )
    args = parser.parse_args(argv)

    if args.repo_root:
        repo = Path(args.repo_root)
    else:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            text=True,
            capture_output=True,
            check=False,
        )
        if out.returncode != 0:
            sys.stderr.write("skill_path_map_lint: not in a git working tree\n")
            return 2
        repo = Path(out.stdout.strip())

    rules = tuple(r.strip() for r in args.rules.split(",") if r.strip())
    unknown = [r for r in rules if r not in ALL_RULES]
    if unknown:
        sys.stderr.write(f"skill_path_map_lint: unknown rule(s): {unknown}\n")
        return 2

    map_path = Path(args.head_map) if args.head_map else repo / MAP_RELPATH
    if not map_path.exists():
        sys.stderr.write(f"skill_path_map_lint: map not found: {map_path}\n")
        return 2
    try:
        head_map = json.loads(map_path.read_text())
    except json.JSONDecodeError as exc:
        print(f"skill_path_map_lint: {map_path} is not valid JSON: {exc}")
        return 1

    base_map = None
    if "co-claim" in rules:
        if args.base_map:
            base_map = json.loads(Path(args.base_map).read_text())
        elif args.base:
            blob = read_blob(repo, args.base, MAP_RELPATH)
            if blob is not None:
                try:
                    base_map = json.loads(blob)
                except json.JSONDecodeError:
                    base_map = None

    files, submodules = index_entries(repo)
    schema_path = Path(args.schema) if args.schema else repo / SCHEMA_RELPATH

    errors = run(
        repo=repo,
        rules=rules,
        head_map=head_map,
        base_map=base_map,
        files=files,
        submodules=submodules,
        schema_path=schema_path,
    )

    if not errors:
        print("skill-path-map lint: ok")
        return 0

    print(f"skill-path-map lint FAILED ({len(errors)} violation(s)):")
    for err in errors:
        print(f"  {err}")
    return 0 if args.mode == "hint" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
