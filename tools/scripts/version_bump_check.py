#!/usr/bin/env python3
"""Version-bump gate.

Given a diff range (base..head), decide whether each configured surface
needs a version bump (patch/minor/major). Three modes:

    report  exit 0 if every surface that moved has a bumped version,
            exit 1 otherwise. Authoritative gate for CI.
    apply   same as report, but for every surface missing a bump, rewrite
            the version file(s) in place and stage them for commit.
            Used by `pulp pr` to make bumps automatic.
    hint    advisory text only; always exits 0. Used by agent hooks.

Additional flag:

    --require-bump-for-fix-feat
        When set, asserts that PRs whose title carries a Conventional
        Commits `fix:` or `feat:` prefix (parsed from $GITHUB_PR_TITLE
        or --pr-title) include either an accepted bump-marker commit
        subject prefix (`chore: bump versions` canonical, or legacy
        `chore(versions): bump`) in the diff range OR a
        `Version-Bump: skip reason="..."` trailer on the tip commit.
        Near-misses like `chore: bump SDK to vX.Y.Z` deliberately do
        not count. This is the structural fix for the 2026-04-30
        incident where PR #1008 (a `fix(view):` user-facing fix) merged
        without an accompanying bump and consumers got stuck on an
        un-released main. Runs additively — the existing per-surface
        verdict pipeline is unchanged. Independent of `--mode`; if
        enabled it can fail even when the per-surface verdicts pass.

Heuristics (per surface, deliberately conservative):
    - If only internal_only_paths changed       → patch-suggested
    - If any public_api_paths changed (non-comment/whitespace diff)
                                                → minor-required
    - If a Version-Bump: <surface>=<level>      → that level is
      authoritative (Shipyard v0.25.0 / PR #152): used as-is, not
      just as a ceiling. Can lower a minor-heuristic to patch when the
      author judges wide-surface-area diffs as still semver-patch.
      The `reason="..."` string is the justification-of-record.
      Still path-filtered — a plugin-only `Version-Bump: sdk=major`
      is ignored when the SDK's trigger_paths weren't touched.
    - Conventional-commit subjects (`feat:`, `fix:`, `BREAKING:` or `!:`
      in subjects) may RAISE the heuristic verdict on a surface whose
      trigger_paths were actually touched. Cannot lower it. Skipped
      entirely when an explicit `<surface>=<level>` trailer is present
      (otherwise a `feat:` could silently revert an author-declared
      `=patch` back to `=minor`, defeating the trailer).
    - Revert commits (subject starts with `Revert` or `Revert-Of:` trailer)
      suppress signals from the reverted work.

Uses JSON configs (zero-dep).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

# Shared substrate (2026-05 refactor batch). `_strip_meta` is the
# version-bump-specific alias for `strip_meta`; we keep the public alias
# so other callers in this file (and any external imports) don't break.
from gate_common import (
    repo_root,
    git_diff_names,
    git_range_trailers,
    git_commit_trailers,
    glob_to_regex as _glob_to_regex,
    glob_match as _glob_match,
    matches_any as _matches_any,
    strip_meta as _strip_meta,
)


# ── Types ───────────────────────────────────────────────────────────────

# Ordered least → most impactful.
LEVELS = ("none", "patch", "minor", "major")


@dataclass
class VersionFile:
    path: str
    kind: str
    field: str | None = None
    pattern: str | None = None


@dataclass
class Surface:
    name: str
    label: str
    version_files: list[VersionFile]
    trigger_paths: list[str]
    public_api_paths: list[str] = field(default_factory=list)
    internal_only_paths: list[str] = field(default_factory=list)
    changelog: str | None = None


@dataclass
class Config:
    surfaces: list[Surface]
    generated_globs: list[str]
    trailer_version_bump: str


@dataclass
class Verdict:
    surface: Surface
    heuristic: str       # "none"|"patch"|"minor"|"major"
    trailer_override: str | None  # set / skip / None
    current_version: str | None
    final_level: str     # what level is actually needed after overrides


# ── Git helpers ─────────────────────────────────────────────────────────
# repo_root / git_diff_names / git_range_trailers / git_commit_trailers
# come from gate_common (see top-of-file import). Surface-specific helpers
# below.


def git_diff_ignore_whitespace_nonempty(base: str, head: str, path: str) -> bool:
    """True iff there is a meaningful (non-whitespace, non-comment) diff for `path`.

    First tries --ignore-all-space; if that leaves any hunks, checks whether
    every hunk line is a C/C++/Python comment. Useful for collapsing comment
    reflows down to patch-suggested instead of minor-required.
    """
    out = subprocess.run(
        ["git", "diff", "--ignore-all-space", f"{base}..{head}", "--", path],
        check=True, capture_output=True, text=True,
    )
    if not out.stdout.strip():
        return False

    has_real = False
    for line in out.stdout.splitlines():
        if not (line.startswith("+") or line.startswith("-")):
            continue
        if line.startswith(("+++", "---")):
            continue
        body = line[1:].strip()
        if not body:
            continue
        if body.startswith(("//", "#", "/*", "*/", "*")):
            continue
        has_real = True
        break
    return has_real


def git_log_subjects_and_bodies(base: str, head: str) -> list[tuple[str, str, str]]:
    """Return (sha, subject, body) tuples for each commit in base..head."""
    out = subprocess.run(
        ["git", "log", "--format=%H%x00%s%x00%B%x01", f"{base}..{head}"],
        check=True, capture_output=True, text=True,
    )
    commits: list[tuple[str, str, str]] = []
    for chunk in out.stdout.split("\x01"):
        chunk = chunk.strip()
        if not chunk:
            continue
        parts = chunk.split("\x00", 2)
        if len(parts) < 3:
            continue
        sha, subject, body = parts
        commits.append((sha, subject, body))
    return commits


def git_commit_files(sha: str) -> list[str]:
    """Files touched by a single commit. Used to scope conv-commit signals
    to surfaces whose trigger_paths the commit actually modified."""
    out = subprocess.run(
        ["git", "show", "--name-only", "--format=", sha],
        check=True, capture_output=True, text=True,
    )
    return [line for line in out.stdout.splitlines() if line.strip()]


# ── Config loading ──────────────────────────────────────────────────────


def load_config(path: Path) -> Config:
    data = _strip_meta(json.loads(path.read_text()))
    surfaces: list[Surface] = []
    for name, entry in (data.get("surfaces") or {}).items():
        vfs = [VersionFile(**_strip_meta(vf) if isinstance(vf, dict) else vf)
               for vf in entry.get("version_files", [])]
        surfaces.append(Surface(
            name=name,
            label=entry.get("label", name),
            version_files=vfs,
            trigger_paths=entry.get("trigger_paths", []),
            public_api_paths=entry.get("public_api_paths", []),
            internal_only_paths=entry.get("internal_only_paths", []),
            changelog=entry.get("changelog"),
        ))
    trailers = data.get("trailers") or {}
    return Config(
        surfaces=surfaces,
        generated_globs=data.get("generated_globs", []) or [],
        trailer_version_bump=trailers.get("version_bump", "Version-Bump"),
    )


# ── Version-file I/O ────────────────────────────────────────────────────

_CMAKE_PROJECT_VERSION_RE = re.compile(
    r"(project\s*\(\s*[A-Za-z_][\w]*\s+VERSION\s+)(\d+\.\d+\.\d+)",
    re.MULTILINE,
)


def read_version(repo: Path, vf: VersionFile) -> str | None:
    p = repo / vf.path
    if not p.exists():
        return None
    text = p.read_text()
    if vf.kind == "cmake_project_version":
        m = _CMAKE_PROJECT_VERSION_RE.search(text)
        return m.group(2) if m else None
    if vf.kind == "json_field":
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return None
        return data.get(vf.field)
    if vf.kind == "json_path":
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return None
        return _json_walk_get(data, vf.field or "")
    if vf.kind == "pyproject_version":
        m = re.search(r'^\s*version\s*=\s*"([^"]+)"', text, re.MULTILINE)
        return m.group(1) if m else None
    if vf.kind == "python_dunder_version":
        m = re.search(r'^\s*__version__\s*=\s*"([^"]+)"', text, re.MULTILINE)
        return m.group(1) if m else None
    if vf.kind == "regex" and vf.pattern:
        m = re.search(vf.pattern, text)
        return m.group(1) if m else None
    return None


def _json_walk_get(data, dotted_path: str):
    # Walk `plugins.0.version`-style paths. Numeric segments index into
    # arrays; string segments index into objects. Returns None on any
    # miss rather than raising, so the gate stays advisory-style on
    # shape drift.
    if not dotted_path:
        return None
    cur = data
    for seg in dotted_path.split("."):
        if seg == "":
            return None
        if isinstance(cur, list):
            try:
                cur = cur[int(seg)]
            except (ValueError, IndexError):
                return None
        elif isinstance(cur, dict):
            if seg not in cur:
                return None
            cur = cur[seg]
        else:
            return None
    return cur


def _json_walk_set(data, dotted_path: str, value) -> bool:
    # Mirror of _json_walk_get for write_version. Returns True iff the
    # walk reached a settable leaf.
    if not dotted_path:
        return False
    parts = dotted_path.split(".")
    if any(p == "" for p in parts):
        return False
    cur = data
    for seg in parts[:-1]:
        if isinstance(cur, list):
            try:
                cur = cur[int(seg)]
            except (ValueError, IndexError):
                return False
        elif isinstance(cur, dict):
            if seg not in cur:
                return False
            cur = cur[seg]
        else:
            return False
    last = parts[-1]
    if isinstance(cur, list):
        try:
            cur[int(last)] = value
        except (ValueError, IndexError):
            return False
        return True
    if isinstance(cur, dict):
        cur[last] = value
        return True
    return False


def write_version(repo: Path, vf: VersionFile, new: str) -> bool:
    p = repo / vf.path
    if not p.exists():
        return False
    text = p.read_text()
    new_text = text
    if vf.kind == "cmake_project_version":
        new_text = _CMAKE_PROJECT_VERSION_RE.sub(
            lambda m: f"{m.group(1)}{new}", text, count=1
        )
    elif vf.kind == "json_field":
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return False
        data[vf.field] = new
        # Preserve trailing newline if original had one.
        end = "\n" if text.endswith("\n") else ""
        new_text = json.dumps(data, indent=2) + end
    elif vf.kind == "json_path":
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return False
        if not _json_walk_set(data, vf.field or "", new):
            return False
        end = "\n" if text.endswith("\n") else ""
        new_text = json.dumps(data, indent=2) + end
    elif vf.kind == "pyproject_version":
        new_text = re.sub(
            r'(^\s*version\s*=\s*")([^"]+)(")',
            lambda m: f"{m.group(1)}{new}{m.group(3)}", text,
            count=1, flags=re.MULTILINE,
        )
    elif vf.kind == "python_dunder_version":
        new_text = re.sub(
            r'(^\s*__version__\s*=\s*")([^"]+)(")',
            lambda m: f"{m.group(1)}{new}{m.group(3)}", text,
            count=1, flags=re.MULTILINE,
        )
    elif vf.kind == "regex" and vf.pattern:
        # A capture-group-1 version string is required.
        new_text = re.sub(vf.pattern, lambda m: m.group(0).replace(m.group(1), new), text, count=1)
    else:
        return False
    if new_text == text:
        return False
    p.write_text(new_text)
    return True


# ── Matching / heuristics ───────────────────────────────────────────────
# _glob_to_regex / _glob_match / _matches_any come from gate_common.
# Single source of truth for matching changed-file paths against
# versioning.json's trigger_paths / public_api_paths /
# internal_only_paths / generated_globs. See gate_common.glob_to_regex
# for the post-#554 slash-boundary semantics.


def filter_generated(changed: list[str], globs: Iterable[str]) -> list[str]:
    gs = list(globs)
    return [f for f in changed if not _matches_any(f, gs)]


def is_revert_commit(subject: str, trailers: dict[str, list[str]]) -> bool:
    if subject.lower().startswith("revert"):
        return True
    if "revert-of" in trailers:
        return True
    return False


def classify_conventional(subject: str) -> str:
    """Map a commit subject to a bump level it would *suggest*.

    `BREAKING:` anywhere in subject or `!:` after the type → major.
    `feat:` → minor. `fix:` or `perf:` → patch. Otherwise none.
    """
    s = subject.strip()
    if re.search(r"\bBREAKING\b", s):
        return "major"
    # `type!:` or `type(scope)!:` signals breaking change.
    if re.match(r"^[a-zA-Z]+(\([^)]*\))?!:", s):
        return "major"
    m = re.match(r"^([a-zA-Z]+)(\([^)]*\))?:", s)
    if not m:
        return "none"
    kind = m.group(1).lower()
    if kind == "feat":
        return "minor"
    if kind in ("fix", "perf"):
        return "patch"
    return "none"


def max_level(*levels: str) -> str:
    best = "none"
    for lv in levels:
        if LEVELS.index(lv) > LEVELS.index(best):
            best = lv
    return best


def heuristic_for_surface(
    surface: Surface,
    changed: list[str],
    base: str,
    head: str,
) -> str:
    touched = [p for p in changed if _matches_any(p, surface.trigger_paths)]
    if not touched:
        return "none"

    # Filter to touched paths with a meaningful (non-whitespace, non-comment-only)
    # diff. Paths whose changes are entirely whitespace or comments don't
    # trigger a version bump — fall through to "none".
    meaningful = [p for p in touched if git_diff_ignore_whitespace_nonempty(base, head, p)]
    if not meaningful:
        return "none"

    # Any public-API path with a meaningful diff → minor-required.
    if any(_matches_any(p, surface.public_api_paths) for p in meaningful):
        return "minor"

    # Otherwise internal-only changes → patch-suggested (advisory; report mode
    # treats this as a warning, never hard fail).
    return "patch"


def surface_trailer_override(
    trailers: dict[str, list[str]],
    trailer_key: str,
    surface_name: str,
) -> str | None:
    """Parse `Version-Bump: <surface>=<level> reason="..."` from trailers.

    The documented trailer grammar accepts `patch`, `minor`, `major`, and
    `skip`. The internal `LEVELS` tuple includes the sentinel `"none"`
    so the heuristic pipeline can compare ranks, but `none` is NOT a
    valid trailer value — accepting it would let
    `Version-Bump: <surface>=none` silently downgrade a real `minor` /
    `major` heuristic to "no bump needed", bypassing the gate. Codex
    review on PR #629 caught this. Reject `none` here as defense-in-
    depth; the call site at `assess_surfaces` also filters it out.
    """
    for v in trailers.get(trailer_key.lower(), []):
        m = re.search(rf"{re.escape(surface_name)}\s*=\s*([A-Za-z]+)", v)
        if not m:
            continue
        level = m.group(1).lower()
        if level == "none":
            continue
        # pulp #1054 — `skip` levels MUST carry a non-empty
        # `reason="..."` (the level itself doesn't document intent —
        # the reason does). Mirrors the unscoped-form enforcement at
        # `_range_has_version_bump_skip_trailer()` (Codex P2 #1310 →
        # PR #1315). Per-surface `<surface>=patch|minor|major` levels
        # are explicit bump verdicts and don't need a reason — the
        # level itself is the documented intent.
        if level == "skip":
            rm = re.search(r'reason\s*=\s*"([^"]+)"', v)
            if not (rm and rm.group(1).strip()):
                continue
        if level in LEVELS or level == "skip":
            return level
    return None


# ── Version bumping arithmetic ──────────────────────────────────────────


def bump_version(current: str, level: str) -> str:
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)", current)
    if not m:
        return current
    major, minor, patch = map(int, m.groups())
    if level == "major":
        return f"{major + 1}.0.0"
    if level == "minor":
        return f"{major}.{minor + 1}.0"
    if level == "patch":
        return f"{major}.{minor}.{patch + 1}"
    return current


# ── Reporting / apply ───────────────────────────────────────────────────


def _extract_version_from_text(text: str, vf: VersionFile) -> str | None:
    if vf.kind == "cmake_project_version":
        m = _CMAKE_PROJECT_VERSION_RE.search(text)
        return m.group(2) if m else None
    if vf.kind == "json_field":
        try:
            return json.loads(text).get(vf.field)
        except json.JSONDecodeError:
            return None
    if vf.kind == "json_path":
        try:
            return _json_walk_get(json.loads(text), vf.field or "")
        except json.JSONDecodeError:
            return None
    if vf.kind == "pyproject_version":
        m = re.search(r'^\s*version\s*=\s*"([^"]+)"', text, re.MULTILINE)
        return m.group(1) if m else None
    if vf.kind == "python_dunder_version":
        m = re.search(r'^\s*__version__\s*=\s*"([^"]+)"', text, re.MULTILINE)
        return m.group(1) if m else None
    return None


def version_at_base(base: str, vf: VersionFile) -> str | None:
    """Read the version from a file as it existed at `base`, or None."""
    try:
        base_text = subprocess.run(
            ["git", "show", f"{base}:{vf.path}"],
            check=True, capture_output=True, text=True,
        ).stdout
    except subprocess.CalledProcessError:
        return None
    return _extract_version_from_text(base_text, vf)


def already_bumped(base: str, vf: VersionFile, repo: Path) -> bool:
    """True iff the version file's version at HEAD differs from at base."""
    p = repo / vf.path
    if not p.exists():
        return False
    base_ver = version_at_base(base, vf)
    if base_ver is None:
        return False
    head_ver = _extract_version_from_text((repo / vf.path).read_text(), vf)
    if head_ver is None:
        return False
    return base_ver != head_ver


def assess_surfaces(
    cfg: Config,
    changed: list[str],
    base: str,
    head: str,
    repo: Path,
) -> list[Verdict]:
    trailers = git_range_trailers(base, head)
    verdicts: list[Verdict] = []
    for s in cfg.surfaces:
        heur = heuristic_for_surface(s, changed, base, head)
        # "Did the diff touch ANY of this surface's trigger paths?" is a
        # weaker condition than the heuristic (which also requires a
        # meaningful, non-comment diff). We use it to decide whether an
        # explicit trailer override is allowed to take effect even when the
        # heuristic is "none" (e.g. the author knows a comment-only change
        # is still API-visible via the docstring).
        trigger_touched = any(_matches_any(p, s.trigger_paths) for p in changed)

        override = surface_trailer_override(trailers, cfg.trailer_version_bump, s.name)
        final = heur
        skip_requested = (override == "skip")
        # `LEVELS` includes the sentinel "none" so the heuristic
        # pipeline can compare ranks; the trailer grammar does NOT
        # accept "none" as an explicit override (only `patch`,
        # `minor`, `major`, `skip`). Including "none" here would let
        # `Version-Bump: <surface>=none` silently downgrade a real
        # `minor` / `major` heuristic verdict to "no bump needed",
        # bypassing the gate. See Codex review on PR #629.
        explicit_level = override in LEVELS and override != "none"

        if skip_requested:
            final = "none"
        elif explicit_level:
            # `Version-Bump: <surface>=<level> reason="..."` is authoritative
            # (Shipyard v0.25.0 / PR #152): the author stated the level AND
            # accepted accountability via the `reason` string. Use it exactly —
            # this is *not* "can only raise". Patch can override a minor
            # heuristic when the author judges that a wide-surface-area diff
            # is still semver-patch (e.g. a bug fix that touches many files).
            # The reason string is the justification-of-record; reviewers can
            # still push back in PR review. Silently falling back to the
            # heuristic when the author asked for a lower level defeats the
            # entire point of the trailer. Untouched surfaces still ignore
            # the override to avoid rubber-stamping unrelated bumps.
            if not trigger_touched:
                final = "none"
            elif heur != "none":
                final = override
            else:
                # Surface has trigger-touched paths but the content
                # heuristic saw nothing meaningful (comments only, etc.).
                # Honor the override — the author knows the change is
                # API-visible even if the byte-diff doesn't show it.
                final = override

        # Promote via conventional-commit subjects on commits that touched
        # THIS surface — never from commits that only touched unrelated
        # paths. A plugin-only `feat:` cannot raise the SDK ceiling. An
        # explicit `Version-Bump: <surface>=skip` on the tip commit is
        # authoritative and is NOT raised back up by conv-commit subjects.
        # Same reasoning for an explicit `<surface>=<level>` trailer:
        # otherwise a `feat:` in a commit subject would silently raise an
        # author-declared `=patch` back to `=minor`, defeating the
        # trailer's purpose (Shipyard v0.25.0 / PR #152).
        if heur != "none" and not skip_requested and not explicit_level:
            conv_ceiling = "none"
            for sha, subject, body in git_log_subjects_and_bodies(base, head):
                if is_revert_commit(subject, {}):
                    continue
                # Scope to commits whose files intersect this surface's
                # trigger_paths — otherwise a feat: on the plugin can raise
                # the SDK.
                files = git_commit_files(sha)
                if not any(_matches_any(f, s.trigger_paths) for f in files):
                    continue
                conv_ceiling = max_level(conv_ceiling, classify_conventional(subject))
            if LEVELS.index(conv_ceiling) > LEVELS.index(final):
                final = conv_ceiling

        # patch-suggested is advisory; never hard-fails.
        # (Callers map this to a warning in report mode.)

        current = None
        for vf in s.version_files:
            current = read_version(repo, vf)
            if current:
                break

        verdicts.append(Verdict(
            surface=s,
            heuristic=heur,
            trailer_override=override,
            current_version=current,
            final_level=final,
        ))
    return verdicts


def render_report(
    verdicts: list[Verdict],
    mode: str,
    base: str,
    repo: Path,
) -> tuple[str, int]:
    lines: list[str] = []
    failures = 0
    warnings = 0
    for v in verdicts:
        if v.final_level == "none":
            lines.append(f"[{v.surface.name}] {v.surface.label}: no bump needed")
            continue
        # Every version file in the surface must have moved, not just one.
        # Otherwise surfaces with multiple files (plugin.json + marketplace.json)
        # could pass with only one bumped, causing split-brain versions.
        per_file = [(vf, already_bumped(base, vf, repo)) for vf in v.surface.version_files]
        all_bumped = all(bumped for _, bumped in per_file)
        any_bumped = any(bumped for _, bumped in per_file)

        if all_bumped:
            tag = "✓ bumped"
        elif any_bumped:
            unbumped = [vf.path for vf, b in per_file if not b]
            tag = f"✗ partial bump — not moved: {', '.join(unbumped)}"
        elif v.final_level == "patch":
            # Advisory only — not a hard fail.
            tag = "? bump suggested (patch)"
        else:
            tag = "✗ bump required"
        lines.append(
            f"[{v.surface.name}] {v.surface.label}: "
            f"heuristic={v.heuristic}"
            f"{' override=' + v.trailer_override if v.trailer_override else ''} "
            f"final={v.final_level} "
            f"current={v.current_version or '?'} "
            f"{tag}"
        )
        if not all_bumped:
            # Partial-bump is always a hard fail — split-brain versions are
            # never acceptable. Patch-suggested stays advisory only when
            # nothing has been bumped at all.
            if any_bumped:
                failures += 1
            elif v.final_level == "patch":
                warnings += 1
            else:
                failures += 1

    if mode == "hint":
        return "\n".join(lines), 0

    if failures:
        lines.append("")
        lines.append("Version-bump check FAILED.")
        lines.append("Apply the required bump with:")
        lines.append("  python3 tools/scripts/version_bump_check.py --mode=apply")
        lines.append("Or record an explicit override on the tip commit:")
        lines.append('  Version-Bump: <surface>=<patch|minor|major|skip> reason="..."')
        return "\n".join(lines), 1

    return "\n".join(lines), 0


def apply_bumps(
    verdicts: list[Verdict],
    base: str,
    repo: Path,
) -> list[str]:
    """Write new versions for surfaces that need a bump and aren't already bumped."""
    edited: list[str] = []
    for v in verdicts:
        if v.final_level in ("none", "patch"):
            continue
        # Skip if ALL version files are already at the target; otherwise
        # apply to every file (keeps plugin.json and marketplace.json in
        # lockstep after a partial-bump from a prior run).
        all_bumped = all(already_bumped(base, vf, repo) for vf in v.surface.version_files)
        if all_bumped or not v.current_version:
            continue
        # Compute the target from the BASE version, not v.current_version.
        # v.current_version reflects the first readable file at HEAD, which
        # may already have been bumped by a prior partial run — bumping it
        # again would double-bump (e.g. 0.1.0 -> 0.2.0 -> 0.3.0). Reading
        # the base keeps a partial-apply idempotent.
        base_ver: str | None = None
        for vf in v.surface.version_files:
            base_ver = version_at_base(base, vf)
            if base_ver:
                break
        source_ver = base_ver or v.current_version
        new_ver = bump_version(source_ver, v.final_level)
        for vf in v.surface.version_files:
            if write_version(repo, vf, new_ver):
                edited.append(vf.path)
        # CHANGELOG.md is intentionally NOT written here (C1, 2026-05).
        # Ownership moved to Shipyard post-tag sync via
        # `.github/workflows/post-tag-sync.yml` and the
        # `shipyard changelog regenerate` command. PR-side stub insertion
        # was the source of repeated multi-PR-train rebases: PR A and PR
        # B both insert `## [0.105.0]` headers, the first one merges, the
        # second one conflicts on the same line. Letting Shipyard own the
        # full regen at tag time eliminates the conflict class entirely.
        # `versioning.json` still carries each surface's `changelog`
        # field — Shipyard reads it.
    # Stage for commit so callers see them in `git status`.
    if edited:
        subprocess.run(["git", "-C", str(repo), "add", "--"] + edited, check=False)
    return edited


# ── PR-title fix/feat-needs-bump check ─────────────────────────────────


# Conventional Commits prefix for user-facing changes that must ship
# with a version bump. We accept `fix:` and `feat:` (with optional
# `(scope)` suffix) — `chore:`, `docs:`, `test:`, `refactor:`,
# `perf:`, `style:`, `build:`, `ci:`, `revert:` are explicitly NOT
# user-facing release events. `feat!:` and `fix!:` are still caught
# (the `!` lives between `feat`/`fix` and the colon).
_FIX_FEAT_TITLE_RE = re.compile(r"^(fix|feat)(\([^)]*\))?!?:\s")
BUMP_COMMIT_SUBJECT_PREFIXES = (
    "chore: bump versions",
    "chore(versions): bump",
)


def _is_fix_or_feat_title(title: str) -> bool:
    return bool(_FIX_FEAT_TITLE_RE.match(title.strip()))


def _range_has_bump_commit(base: str, head: str) -> bool:
    """True iff any commit in base..head has an accepted bump-marker
    subject prefix. `chore: bump versions` is the canonical subject
    `pulp pr` writes when `version_bump_check.py --mode=apply` rewrote
    a version file. Using subject prefix instead of trailer matching
    keeps the check robust against squash-merge subject mangling.
    """
    for _sha, subject, _body in git_log_subjects_and_bodies(base, head):
        s = subject.strip().lower()
        if any(s.startswith(prefix) for prefix in BUMP_COMMIT_SUBJECT_PREFIXES):
            return True
    return False


def _range_has_version_bump_skip_trailer(base: str, head: str) -> bool:
    """True iff ANY commit in base..head carries a top-level
    `Version-Bump: skip reason="..."` trailer. Surface-specific skip
    trailers (e.g. `sdk=skip`) are NOT honored here — to bypass the
    fix/feat-needs-bump check entirely the author must say so
    explicitly.

    A non-empty reason is required; bare `Version-Bump: skip` is
    rejected so the author has to record *why*.
    """
    trailers = git_range_trailers(base, head)
    for value in trailers.get("version-bump", []):
        # Accept `skip reason="..."` (no surface prefix) to opt out of
        # the *entire* fix/feat check. Per-surface `<surface>=skip`
        # trailers do NOT count — those are scoped to the per-surface
        # verdict pipeline and should not silently bypass the
        # user-facing-PR check.
        m = re.match(r"^\s*skip\b(.*)$", value.strip(), re.IGNORECASE)
        if not m:
            continue
        rest = m.group(1)
        # Require a non-empty reason="..." (matching the documented
        # bypass grammar — empty-reason bypasses are rejected).
        rm = re.search(r'reason\s*=\s*"([^"]+)"', rest)
        if rm and rm.group(1).strip():
            return True
    return False


def check_fix_feat_requires_bump(
    pr_title: str,
    base: str,
    head: str,
) -> tuple[bool, str]:
    """Returns (passed, message). `passed=True` means either:

    - the PR title is not a `fix:` / `feat:` (no requirement), OR
    - the title matches and the diff range contains a bump commit, OR
    - the title matches and the tip commit carries a top-level
      `Version-Bump: skip reason="..."` trailer.

    Otherwise returns (False, error-with-suggestions).
    """
    if not pr_title or not pr_title.strip():
        # Defensive: no title supplied means the workflow couldn't
        # resolve $GITHUB_PR_TITLE. Don't false-fail the gate — the
        # per-surface verdict is still authoritative.
        return True, (
            "fix/feat-needs-bump: PR title not provided; skipping check "
            "(this is normal on push events and workflow_dispatch)."
        )

    if not _is_fix_or_feat_title(pr_title):
        return True, (
            f"fix/feat-needs-bump: PR title {pr_title!r} is not a "
            "`fix:` or `feat:` user-facing change — no bump required."
        )

    if _range_has_bump_commit(base, head):
        return True, (
            f"fix/feat-needs-bump: PR title {pr_title!r} matches; "
            "found `chore: bump versions` commit in the diff range — OK."
        )

    if _range_has_version_bump_skip_trailer(base, head):
        return True, (
            f"fix/feat-needs-bump: PR title {pr_title!r} matches; "
            'no bump commit found, but a `Version-Bump: skip reason="..."` '
            "trailer is present in the range — bypass honored."
        )

    return False, (
        f"fix/feat-needs-bump: PR title {pr_title!r} is a user-facing "
        "`fix:` / `feat:` change but the diff range contains NO commit "
        "with subject `chore: bump versions` (canonical; legacy "
        "`chore(versions): bump` is also accepted) AND no top-level "
        '`Version-Bump: skip reason="..."` trailer. Commit subjects like '
        "`chore: bump SDK to vX.Y.Z` do not satisfy this guard.\n"
        "\n"
        "User-facing fixes/features that land without a version bump "
        "are stranded on main — `auto-release.yml` will not tag, and "
        "consumers cannot reach the change. This is the structural "
        "fix for the 2026-04-30 incident (PR #1008 → issue #1009).\n"
        "\n"
        "Resolution — pick one:\n"
        "  • Run `shipyard pr` (or `pulp pr`) so version_bump_check "
        "can apply the bump and append a `chore: bump versions` commit.\n"
        "  • If the fix/feat is genuinely not user-facing (rare — "
        "consider re-titling to `chore:` / `docs:` / `refactor:` "
        "instead), add a top-level trailer to the tip commit:\n"
        '      Version-Bump: skip reason="<why this fix doesn\'t need a release>"\n'
        "  • Branch protection on `main` SHOULD make this an enforced "
        "required check; see docs/guides/release-watchdog.md."
    )


# ── Main ────────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Version-bump gate")
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--config", default=None)
    parser.add_argument("--mode", choices=("report", "hint", "apply"), default="report")
    parser.add_argument("--repo-root", default=None)
    parser.add_argument(
        "--require-bump-for-fix-feat",
        action="store_true",
        help=(
            "Additively require that PRs titled `fix:`/`feat:` (read "
            "from $GITHUB_PR_TITLE or --pr-title) include either a "
            '`chore: bump versions` commit or a `Version-Bump: skip '
            'reason="..."` trailer. Hard-fails when violated. Wired '
            "into version-skill-check.yml on PR triggers."
        ),
    )
    parser.add_argument(
        "--pr-title",
        default=None,
        help=(
            "Override the PR title used by --require-bump-for-fix-feat. "
            "Defaults to $GITHUB_PR_TITLE. Empty / unset means the "
            "check is skipped (normal for push and workflow_dispatch)."
        ),
    )
    args = parser.parse_args(argv)

    root = Path(args.repo_root) if args.repo_root else repo_root()
    cfg_path = Path(args.config) if args.config else root / "tools" / "scripts" / "versioning.json"
    if not cfg_path.exists():
        sys.stderr.write(f"version_bump_check: config not found: {cfg_path}\n")
        return 2

    cfg = load_config(cfg_path)

    changed = git_diff_names(args.base, args.head)
    changed = filter_generated(changed, cfg.generated_globs)

    verdicts = assess_surfaces(cfg, changed, args.base, args.head, root)

    if args.mode == "apply":
        edited = apply_bumps(verdicts, args.base, root)
        # Re-assess after editing: re-read current versions and re-check.
        verdicts_after = assess_surfaces(cfg, changed, args.base, args.head, root)
        text, code = render_report(verdicts_after, mode="report", base=args.base, repo=root)
        if edited:
            print("Edited files:")
            for e in edited:
                print(f"  {e}")
        if text:
            print(text)
        # `--require-bump-for-fix-feat` is meaningful in apply mode too:
        # if `pulp pr` ran apply and didn't actually edit anything (no
        # surface needed a bump), but the PR title is `fix:` / `feat:`,
        # the check should still flag it — that means the heuristic
        # found nothing actionable *and* the author didn't record a
        # skip trailer. Better to fail here than at the CI gate.
        if args.require_bump_for_fix_feat:
            ff_passed, ff_msg = check_fix_feat_requires_bump(
                args.pr_title if args.pr_title is not None
                else os.environ.get("GITHUB_PR_TITLE", ""),
                args.base, args.head,
            )
            print(ff_msg)
            if not ff_passed:
                return 1
        return code

    text, code = render_report(verdicts, args.mode, args.base, root)
    if text:
        print(text)

    if args.require_bump_for_fix_feat:
        # Hint mode keeps its "always exit 0" contract — the fix/feat
        # check still prints its message, but never raises the exit code.
        ff_passed, ff_msg = check_fix_feat_requires_bump(
            args.pr_title if args.pr_title is not None
            else os.environ.get("GITHUB_PR_TITLE", ""),
            args.base, args.head,
        )
        # Print with a separator so the new check's output is easy to
        # spot in CI logs.
        print()
        print("── fix/feat-needs-bump check ──────────────────────────")
        print(ff_msg)
        if not ff_passed and args.mode != "hint":
            return 1

    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
