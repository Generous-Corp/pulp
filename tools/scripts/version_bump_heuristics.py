#!/usr/bin/env python3
"""Version-bump gate — heuristics cluster.

Per-surface bump-level inference: git-diff helpers, glob filtering,
conventional-commit classification, the path/content heuristic, trailer
override parsing, and the `assess_surfaces` pipeline that fuses them.

`version_bump_check.py` re-exports every public symbol from this module
so external importers and the CLI entrypoint are unaffected.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path
from typing import Iterable

# Shared gate helpers.
from gate_common import (
    git_range_trailers,
    matches_any as _matches_any,
)

from version_bump_surfaces import (
    LEVELS,
    Config,
    Surface,
    Verdict,
    read_version,
)


# Patchable-helper indirection. Tests and external callers may patch
# helpers on the `version_bump_check` entrypoint; resolving through that
# module at call time keeps those patches visible inside
# `heuristic_for_surface` / `assess_surfaces`. The import is deferred to
# avoid the `version_bump_check → cluster` import cycle; by the time these
# functions run, the entrypoint is fully loaded. Falls back to this
# module's own definitions when the entrypoint isn't importable (e.g.
# cluster module exercised in isolation). The compatibility tests in
# test_version_bump_check_extra.py depend on this entrypoint-level patching.
def _vbc():
    try:
        import version_bump_check as _v
        return _v
    except ImportError:  # pragma: no cover - isolated-module fallback
        return None


def _h(name: str):
    """Resolve a patchable helper through version_bump_check, falling
    back to this module's own binding."""
    v = _vbc()
    if v is not None and hasattr(v, name):
        return getattr(v, name)
    return globals()[name]


# ── Git helpers ─────────────────────────────────────────────────────────
# repo_root / git_diff_names / git_range_trailers / git_commit_trailers
# come from gate_common (see version_bump_check.py top-of-file import).
# Surface-specific helpers below.


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
    _whitespace_check = _h("git_diff_ignore_whitespace_nonempty")
    meaningful = [p for p in touched if _whitespace_check(base, head, p)]
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
    valid trailer value; accepting it would let
    `Version-Bump: <surface>=none` silently downgrade a real `minor` /
    `major` heuristic to "no bump needed", bypassing the gate. Reject
    `none` here as defense-in-depth; the call site at `assess_surfaces`
    also filters it out.
    """
    for v in trailers.get(trailer_key.lower(), []):
        m = re.search(rf"{re.escape(surface_name)}\s*=\s*([A-Za-z]+)", v)
        if not m:
            continue
        level = m.group(1).lower()
        if level == "none":
            continue
        # pulp #1054 — `skip` levels MUST carry a non-empty
        # `reason="..."` (the level itself doesn't document intent; the
        # reason does). Mirrors the unscoped-form enforcement at
        # `_range_has_version_bump_skip_trailer()`. Per-surface
        # `<surface>=patch|minor|major` levels are explicit bump verdicts
        # and don't need a reason; the level itself is the documented
        # intent.
        if level == "skip":
            rm = re.search(r'reason\s*=\s*"([^"]+)"', v)
            if not (rm and rm.group(1).strip()):
                continue
        if level in LEVELS or level == "skip":
            return level
    return None


def assess_surfaces(
    cfg: Config,
    changed: list[str],
    base: str,
    head: str,
    repo: Path,
) -> list[Verdict]:
    trailers = _h("git_range_trailers")(base, head)
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
        # bypassing the gate.
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
            for sha, subject, body in _h("git_log_subjects_and_bodies")(base, head):
                if is_revert_commit(subject, {}):
                    continue
                # Scope to commits whose files intersect this surface's
                # trigger_paths — otherwise a feat: on the plugin can raise
                # the SDK.
                files = _h("git_commit_files")(sha)
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
