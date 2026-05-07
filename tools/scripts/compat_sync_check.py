#!/usr/bin/env python3
"""Compat-sync gate (#1029).

Generalizes the skill-sync / docs-sync pattern to the compat-matrix
domain. Given a diff range (base..head), assert that every
compat-mapped source file that was touched also has its required
artifacts updated:

    1. an entry in the matching `compat.json` section/prefix,
    2. the matching docs/reference/compat/<prefix>.md doc page, and
    3. a test that matches the configured glob.

If any of those is missing AND the tip commit (or any commit in the
range) does NOT carry the matching bypass trailer, the gate fails.

Bypass trailer (matches the existing `Version-Bump: skip` /
`Skill-Update: skip` shape):

    Compat-Update: skip prefix=<css|rn|yoga|react|html|canvas2d|imports|*> reason="..."

Multiple skip lines allowed, one per prefix. Bare `Compat-Update: skip`
(no reason) is rejected.

Modes:
    --mode=hint    advisory text only, exit 0
    --mode=report  exit 1 on drift when --enforce or
                   PULP_ENFORCE_PREPUSH=1 is set (advisory otherwise)
    --mode=apply   like report, but ALSO writes stub keys into
                   compat.json so the user only has to fill in details

Partial-blocker tolerance (#1027): until the compat.json sections are
populated, empty sections are NOT a self-check failure. A compat-json
requirement counts as satisfied when:
    - the requested prefix already has a non-empty entry in compat.json, OR
    - the section is empty (no compat-matrix data has been generated
      yet — accept "no requirement yet" so this infrastructure can
      land before #1027), OR
    - the same diff modified compat.json (someone added a new entry).

When #1027 ships the populated matrix, flip COMPAT_TOLERATE_EMPTY in the
config or just remove the empty-tolerance branch in
``_compat_json_satisfied``.

Uses JSON (not YAML) for zero-dependency execution on PEP-668 Python.
Mirrors the shape of ``tools/scripts/skill_sync_check.py``.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path
from typing import Iterable

# All compat sections recognized by the gate. Mirrors the top-level
# keys in compat.json. The wildcard `*` is allowed in
# compat_path_map.json to mean "any/all sections" — used when the
# source file dispatches across multiple compat surfaces (e.g.
# widget_bridge.cpp).
KNOWN_PREFIXES = {"css", "rn", "yoga", "react", "html", "canvas2d", "imports"}


# ── Types ───────────────────────────────────────────────────────────────


@dataclass
class Requirement:
    kind: str           # "compat-json" | "doc" | "test"
    prefix: str | None  # for compat-json: section name like "css", or "*"
    path: str | None    # for doc: literal path with optional {prefix} placeholder
    glob: str | None    # for test: glob like "test/test_widget_bridge*.cpp"


@dataclass
class CompatMap:
    paths: dict[str, list[Requirement]]


@dataclass
class Finding:
    source_path: str
    requirement: Requirement
    resolved_prefix: str  # the actual prefix this finding pertains to
    satisfied: bool
    bypass_reason: str | None
    detail: str           # human-readable "what's missing"


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


def git_range_trailers(base: str, head: str) -> dict[str, list[str]]:
    """Collect trailers from ALL commits in `base..head`, merged.

    CI checks out a synthetic merge commit as HEAD, so a bypass trailer
    on the branch's tip commit wouldn't be seen if we only looked at
    HEAD. Walk the whole range instead — any commit in the range
    carries the bypass. Mirrors the pattern in skill_sync_check.py.
    """
    try:
        body = subprocess.run(
            ["git", "log", "--format=%B%x00", f"{base}..{head}"],
            check=True, capture_output=True, text=True,
        ).stdout
    except subprocess.CalledProcessError:
        return {}

    result: dict[str, list[str]] = {}
    for body_chunk in body.split("\x00"):
        if not body_chunk.strip():
            continue
        trailers = subprocess.run(
            ["git", "interpret-trailers", "--parse"],
            input=body_chunk, capture_output=True, text=True,
        )
        for line in trailers.stdout.splitlines():
            if ":" not in line:
                continue
            key, _, value = line.partition(":")
            result.setdefault(key.strip().lower(), []).append(value.strip())
    return result


# ── Config loading ──────────────────────────────────────────────────────


def _strip_comments(data: dict) -> dict:
    if isinstance(data, dict):
        return {
            k: v for k, v in data.items()
            if not k.startswith("_") and k != "$schema"
        }
    return data


def load_compat_map(path: Path) -> CompatMap:
    raw = _strip_comments(json.loads(path.read_text(encoding="utf-8")))
    paths_raw = raw.get("paths", {}) or {}
    paths: dict[str, list[Requirement]] = {}
    for source, reqs in paths_raw.items():
        out: list[Requirement] = []
        for r in reqs:
            kind = r.get("kind")
            if kind == "compat-json":
                out.append(Requirement(
                    kind="compat-json",
                    prefix=str(r.get("prefix") or "*").rstrip("/"),
                    path=None,
                    glob=None,
                ))
            elif kind == "doc":
                out.append(Requirement(
                    kind="doc",
                    prefix=None,
                    path=str(r.get("path", "")),
                    glob=None,
                ))
            elif kind == "test":
                out.append(Requirement(
                    kind="test",
                    prefix=None,
                    path=None,
                    glob=str(r.get("glob", "")),
                ))
            else:
                # pulp #1171 (Codex P2 on #1068) — unknown `kind` was
                # skipped silently, so a typo like `"kind": "tests"` in
                # tools/scripts/compat_path_map.json silently dropped
                # that requirement from enforcement and CI passed
                # while a required compat artifact check was disabled.
                # Surface the typo as a hard error at config-load time
                # so the gate fails loudly instead of silently
                # under-enforcing.
                raise ValueError(
                    "compat_path_map.json: unknown requirement kind "
                    f"{kind!r} for source {source!r} (entry={r!r}); "
                    "valid kinds: 'compat-json', 'doc', 'test'."
                )
        paths[source] = out
    return CompatMap(paths=paths)


def load_compat_json(path: Path) -> dict:
    """Load the repo-root compat.json. Tolerate missing/empty file —
    #1027 may not have shipped yet."""
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            return {}
        return data
    except json.JSONDecodeError:
        return {}


# ── Glob matching (mirrors skill_sync_check) ───────────────────────────


@lru_cache(maxsize=None)
def _glob_to_regex(pattern: str) -> "re.Pattern[str]":
    """Translate a gitignore-style glob into an anchored regex.

    Mirrors ``skill_sync_check._glob_to_regex``. Kept here as a
    deliberate copy so each gate stays a single-file script. Semantics:
        - ``**`` matches zero or more path segments (including zero).
        - ``*``  matches zero or more characters within a single segment.
        - ``?``  matches exactly one character within a single segment.
        - Patterns are anchored at both ends.
    """
    parts = pattern.split("/")
    n = len(parts)

    STARSTAR = object()
    tokens: list = []
    for part in parts:
        if part == "**":
            tokens.append(STARSTAR)
            continue
        seg = ""
        for c in part:
            if c == "*":
                seg += "[^/]*"
            elif c == "?":
                seg += "[^/]"
            else:
                seg += re.escape(c)
        tokens.append(seg)

    out = ""
    for i, tok in enumerate(tokens):
        is_first = i == 0
        is_last = i == n - 1
        if tok is STARSTAR:
            if is_first and is_last:
                out += ".*"
            elif is_first:
                out += "(?:[^/]+/)*"
            elif is_last:
                if out.endswith("/"):
                    out = out[:-1]
                out += "(?:/.*)?"
            else:
                if not out.endswith("/"):
                    out += "/"
                out += "(?:[^/]+/)*"
        else:
            if not is_first:
                if not out.endswith("/") and not out.endswith(")?") \
                   and not out.endswith(")*"):
                    out += "/"
            out += tok

    return re.compile("^" + out + "$")


def _glob_match(path: str, pattern: str) -> bool:
    return _glob_to_regex(pattern).match(path) is not None


def _matches_any(path: str, patterns: Iterable[str]) -> bool:
    p = path.replace(os.sep, "/")
    for pat in patterns:
        if _glob_match(p, pat):
            return True
    return False


# ── Trailer parsing ─────────────────────────────────────────────────────


def parse_compat_update_trailer(
    trailers: dict[str, list[str]],
) -> dict[str, str]:
    """Parse `Compat-Update: skip prefix=<x> reason="..."` lines.

    Returns {prefix: reason}. Multiple skip lines allowed, one per
    prefix. Bare `skip` with no `prefix=` or no `reason=` is rejected
    (returns nothing for that line).
    """
    bypasses: dict[str, str] = {}
    for v in trailers.get("compat-update", []):
        if not v.lower().startswith("skip"):
            continue
        m_prefix = re.search(r"prefix\s*=\s*([A-Za-z0-9_./*-]+)", v)
        if not m_prefix:
            # Bare "skip" — reject (no audit trail).
            continue
        prefix = m_prefix.group(1).rstrip("/")
        reason_m = (
            re.search(r'reason\s*=\s*"([^"]*)"', v)
            or re.search(r"reason\s*=\s*(\S+)", v)
        )
        if not reason_m:
            # Reason required.
            continue
        reason = reason_m.group(1).strip()
        if not reason:
            continue
        bypasses[prefix] = reason
    return bypasses


# ── Resolution helpers ──────────────────────────────────────────────────


def _resolve_prefixes_for_source(
    requirement: Requirement,
    source_path: str,
    compat_data: dict,
) -> list[str]:
    """Determine which concrete prefixes a source-path requirement
    expands into.

    For most files the path-map declares the prefix directly
    (e.g. canvas2d/). For widget_bridge-style files that span every
    section, the path-map uses `*` and we expand to every KNOWN_PREFIX
    that exists in compat.json (so reviewers see one bullet per
    affected section).
    """
    if requirement.prefix and requirement.prefix != "*":
        return [requirement.prefix]
    # Wildcard: expand to all known sections that exist in compat.json.
    # If compat.json itself is missing/empty, fall back to KNOWN_PREFIXES.
    sections = [k for k in compat_data.keys() if k in KNOWN_PREFIXES]
    if not sections:
        sections = sorted(KNOWN_PREFIXES)
    return sorted(sections)


def _compat_json_satisfied(
    prefix: str,
    compat_data: dict,
    changed: list[str],
    compat_json_path: str,
) -> tuple[bool, str]:
    """Decide whether the compat-json requirement is satisfied.

    Tolerance rules per #1029 partial-blocker:
        1. compat.json modified in this diff → satisfied.
        2. section is non-empty (#1027 has populated it) → satisfied.
        3. section exists but is empty → tolerated (no requirement
           yet) → satisfied.
        4. section absent from compat.json → fail with guidance.
    """
    if compat_json_path in changed:
        return True, "compat.json modified in diff"
    if prefix not in compat_data:
        return False, (
            f"compat.json has no '{prefix}' section "
            f"(expected `\"{prefix}\": {{}}` at minimum)"
        )
    section = compat_data[prefix]
    if isinstance(section, dict) and len(section) == 0:
        return True, "section empty — tolerated until #1027 populates"
    return True, "section already populated"


def _doc_path_for(prefix: str, template: str) -> str:
    """Substitute {prefix} in a doc template path."""
    return template.replace("{prefix}", prefix)


# ── Core check ─────────────────────────────────────────────────────────


def _effective_prefixes_for_source(
    requirements: list[Requirement],
    compat_data: dict,
) -> list[str]:
    """Collect the prefix(es) that apply to a given source-path entry.

    A path-map entry usually declares a single concrete prefix on its
    `compat-json` requirement (e.g. `canvas2d`). All sibling requirements
    (doc, test) implicitly share that prefix — even if they themselves
    are unprefixed — so a single bypass like `prefix=canvas2d` covers
    every requirement for that source.

    For wildcard (`*`) compat-json requirements, expand to every known
    section present in compat.json (or `KNOWN_PREFIXES` if compat.json
    is empty / absent).
    """
    compat_reqs = [r for r in requirements if r.kind == "compat-json"]
    if not compat_reqs:
        return ["*"]

    prefixes: list[str] = []
    for r in compat_reqs:
        if r.prefix and r.prefix != "*":
            prefixes.append(r.prefix)
        else:
            sections = [k for k in compat_data.keys() if k in KNOWN_PREFIXES]
            if not sections:
                sections = sorted(KNOWN_PREFIXES)
            prefixes.extend(sorted(sections))
    # De-dupe while preserving order.
    seen: set[str] = set()
    out: list[str] = []
    for p in prefixes:
        if p not in seen:
            seen.add(p)
            out.append(p)
    return out


def compute_findings(
    changed: list[str],
    compat_map: CompatMap,
    compat_data: dict,
    bypasses: dict[str, str],
    compat_json_path: str = "compat.json",
) -> list[Finding]:
    """Walk every (source, requirement, prefix) triple and decide
    whether it's satisfied / bypassed / failing.

    The same `effective_prefixes` set applies to every requirement
    under a source-path entry — that way a single bypass trailer like
    `prefix=canvas2d` covers compat-json + doc + test in one go.
    """
    findings: list[Finding] = []

    for source, requirements in compat_map.paths.items():
        if source not in changed:
            continue

        effective_prefixes = _effective_prefixes_for_source(
            requirements, compat_data,
        )

        for req in requirements:
            for prefix in effective_prefixes:
                # Bypass: explicit prefix or wildcard.
                bypass = bypasses.get(prefix) or bypasses.get("*")

                if req.kind == "compat-json":
                    # Skip rows that don't match THIS requirement's
                    # declared prefix — a non-wildcard compat-json
                    # requirement only pertains to its own section.
                    if req.prefix and req.prefix != "*" \
                            and prefix != req.prefix:
                        continue
                    ok, detail = _compat_json_satisfied(
                        prefix, compat_data, changed, compat_json_path,
                    )
                    findings.append(Finding(
                        source_path=source,
                        requirement=req,
                        resolved_prefix=prefix,
                        satisfied=ok,
                        bypass_reason=bypass if not ok else None,
                        detail=detail,
                    ))
                elif req.kind == "doc":
                    if req.path and "{prefix}" in req.path:
                        doc_path = _doc_path_for(prefix, req.path)
                    else:
                        # Non-templated doc path: only emit one row per
                        # source (avoid one duplicate per prefix).
                        if prefix != effective_prefixes[0]:
                            continue
                        doc_path = req.path or ""
                    ok = doc_path in changed
                    findings.append(Finding(
                        source_path=source,
                        requirement=req,
                        resolved_prefix=prefix,
                        satisfied=ok,
                        bypass_reason=bypass if not ok else None,
                        detail=(
                            f"doc {doc_path} updated"
                            if ok else f"doc {doc_path} NOT updated"
                        ),
                    ))
                elif req.kind == "test":
                    # Tests aren't prefix-specific — emit one row only.
                    if prefix != effective_prefixes[0]:
                        continue
                    glob = req.glob or ""
                    test_hits = [c for c in changed if _matches_any(c, [glob])]
                    ok = bool(test_hits)
                    findings.append(Finding(
                        source_path=source,
                        requirement=req,
                        resolved_prefix=prefix,
                        satisfied=ok,
                        bypass_reason=bypass if not ok else None,
                        detail=(
                            f"matched {len(test_hits)} test file(s) for {glob}"
                            if ok else f"no test file matching {glob}"
                        ),
                    ))
    return findings


# ── Self-check ─────────────────────────────────────────────────────────


def self_check(compat_map: CompatMap, compat_data: dict) -> list[str]:
    """Return list of problems with the configuration itself."""
    errors: list[str] = []
    for source, reqs in compat_map.paths.items():
        for req in reqs:
            if req.kind == "compat-json" and req.prefix:
                if req.prefix != "*" and req.prefix not in KNOWN_PREFIXES:
                    errors.append(
                        f"self-check: compat_path_map.json[{source!r}] uses "
                        f"unknown prefix '{req.prefix}' (known: "
                        f"{', '.join(sorted(KNOWN_PREFIXES))} or '*')"
                    )
            if req.kind == "test" and not req.glob:
                errors.append(
                    f"self-check: compat_path_map.json[{source!r}] test "
                    f"requirement has no glob"
                )
            if req.kind == "doc" and not req.path:
                errors.append(
                    f"self-check: compat_path_map.json[{source!r}] doc "
                    f"requirement has no path"
                )
    # If compat.json is present, sanity-check its top-level keys against
    # the known prefix set. Unknown keys are non-fatal but warn-worthy.
    if compat_data:
        unknown = [k for k in compat_data.keys()
                   if not k.startswith("_")
                   and not k.startswith("$")
                   and k != "compat-schema-version"
                   and k not in KNOWN_PREFIXES]
        for k in unknown:
            errors.append(
                f"self-check: compat.json has unknown top-level key '{k}'"
            )
    return errors


# ── Apply mode ─────────────────────────────────────────────────────────


def apply_stubs(
    findings: list[Finding],
    compat_data: dict,
    compat_json_path: Path,
) -> list[str]:
    """For each unsatisfied compat-json finding whose section is
    missing, add an empty `{}` placeholder. Returns the list of
    sections added so the caller can report what changed.

    Doc and test stubs are NOT auto-created — those require
    human-authored content per #1029's "user just has to fill in
    details" directive (we add the JSON skeleton; the user authors
    the prose and tests).
    """
    added: list[str] = []
    if not compat_data:
        compat_data = {
            "compat-schema-version": "0.1",
            "_comment": "Stub created by compat_sync_check.py --mode=apply",
        }
        for p in sorted(KNOWN_PREFIXES):
            compat_data[p] = {}
            added.append(p)
    else:
        for f in findings:
            if f.requirement.kind != "compat-json":
                continue
            if f.satisfied:
                continue
            if f.resolved_prefix in KNOWN_PREFIXES \
                    and f.resolved_prefix not in compat_data:
                compat_data[f.resolved_prefix] = {}
                added.append(f.resolved_prefix)

    if added:
        compat_json_path.write_text(
            json.dumps(compat_data, indent=2) + "\n",
            encoding="utf-8",
        )
    return added


# ── Reporting ──────────────────────────────────────────────────────────


def render_report(
    findings: list[Finding],
    mode: str,
    enforce: bool,
) -> tuple[str, int]:
    lines: list[str] = []
    hard_failures: list[Finding] = []

    # Group by (source, prefix) for compact output.
    grouped: dict[tuple[str, str], list[Finding]] = {}
    for f in findings:
        grouped.setdefault((f.source_path, f.resolved_prefix), []).append(f)

    for (source, prefix), group in sorted(grouped.items()):
        lines.append(f"[{source}] (prefix={prefix})")
        for f in group:
            kind = f.requirement.kind
            if f.satisfied:
                lines.append(f"  ✓ {kind}: {f.detail}")
            elif f.bypass_reason:
                lines.append(
                    f"  ✓ {kind}: bypassed ({f.bypass_reason}) — {f.detail}"
                )
            else:
                lines.append(f"  ✗ {kind}: {f.detail}")
                hard_failures.append(f)

    if not findings:
        lines.append("compat-sync: no mapped paths touched — nothing to verify.")

    if hard_failures and mode in ("report", "apply"):
        lines.append("")
        lines.append("Compat-sync check FAILED.")
        lines.append("For each row above marked ✗, do ONE of:")
        lines.append("  1. Update compat.json (add prefix entry), the doc,")
        lines.append("     or add a matching test in this branch, OR")
        lines.append("  2. Add a commit trailer with the exact form:")
        # Show one suggestion line per unique missing prefix.
        seen: set[str] = set()
        for f in hard_failures:
            if f.resolved_prefix in seen:
                continue
            seen.add(f.resolved_prefix)
            lines.append(
                f'     Compat-Update: skip prefix={f.resolved_prefix} reason="..."'
            )
        if enforce:
            return "\n".join(lines), 1
        # Advisory: still print but exit 0.
        lines.append("")
        lines.append(
            "(advisory — set PULP_ENFORCE_PREPUSH=1 or pass --enforce to block)"
        )
        return "\n".join(lines), 0

    return "\n".join(lines), 0


# ── Main ───────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Compat-sync gate (#1029)")
    parser.add_argument("--base", default="origin/main",
                        help="Diff base (default: origin/main)")
    parser.add_argument("--head", default="HEAD",
                        help="Diff head (default: HEAD)")
    parser.add_argument(
        "--config",
        default=None,
        help=(
            "Path to compat_path_map.json "
            "(default: tools/scripts/compat_path_map.json at repo root)"
        ),
    )
    parser.add_argument(
        "--compat-json",
        default=None,
        help="Path to compat.json (default: <repo>/compat.json)",
    )
    parser.add_argument(
        "--mode",
        choices=("report", "hint", "apply"),
        default="report",
        help=(
            "report: print drift, hard-fail iff --enforce or "
            "PULP_ENFORCE_PREPUSH=1; "
            "hint: advisory text only, exit 0; "
            "apply: like report but auto-add stub compat.json sections"
        ),
    )
    parser.add_argument(
        "--enforce",
        action="store_true",
        help="Hard-fail on drift (CI sets this; pre-push hook sets via env).",
    )
    parser.add_argument(
        "--repo-root",
        default=None,
        help="Override repo root (default: git rev-parse --show-toplevel)",
    )
    args = parser.parse_args(argv)

    enforce = args.enforce or os.environ.get("PULP_ENFORCE_PREPUSH") == "1"

    try:
        root = Path(args.repo_root) if args.repo_root else repo_root()
    except subprocess.CalledProcessError:
        sys.stderr.write(
            "compat_sync_check: not in a git repo (or --repo-root invalid)\n"
        )
        return 2

    cfg_path = (
        Path(args.config) if args.config
        else root / "tools" / "scripts" / "compat_path_map.json"
    )
    if not cfg_path.exists():
        sys.stderr.write(
            f"compat_sync_check: config not found: {cfg_path}\n"
        )
        return 2

    compat_json_path = (
        Path(args.compat_json) if args.compat_json
        else root / "compat.json"
    )

    compat_map = load_compat_map(cfg_path)
    compat_data = load_compat_json(compat_json_path)

    self_errors = self_check(compat_map, compat_data)
    if self_errors:
        print("\n".join(self_errors), file=sys.stderr)
        if args.mode == "report" and enforce:
            return 1
        if args.mode == "apply":
            return 1

    try:
        changed = git_diff_names(args.base, args.head)
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(
            f"compat_sync_check: git diff against '{args.base}' failed: {exc}\n"
            "  Fetch the base ref (e.g. `git fetch origin main`) and retry.\n"
        )
        return 2

    trailers = git_range_trailers(args.base, args.head)
    bypasses = parse_compat_update_trailer(trailers)

    # Compute compat.json path RELATIVE to repo root for the changed-file
    # comparison (`git diff --name-only` returns repo-relative paths).
    try:
        compat_json_rel = str(
            compat_json_path.relative_to(root)
        ).replace(os.sep, "/")
    except ValueError:
        compat_json_rel = "compat.json"

    findings = compute_findings(
        changed=changed,
        compat_map=compat_map,
        compat_data=compat_data,
        bypasses=bypasses,
        compat_json_path=compat_json_rel,
    )

    if args.mode == "apply":
        added = apply_stubs(findings, compat_data, compat_json_path)
        if added:
            print(
                f"compat-sync: added stub sections in {compat_json_path}: "
                f"{', '.join(added)}"
            )
            # Re-evaluate now that stubs have been written.
            compat_data = load_compat_json(compat_json_path)
            findings = compute_findings(
                changed=changed + [compat_json_rel],
                compat_map=compat_map,
                compat_data=compat_data,
                bypasses=bypasses,
                compat_json_path=compat_json_rel,
            )

    text, code = render_report(findings, args.mode, enforce)
    if text:
        print(text)

    if args.mode == "hint":
        return 0
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
