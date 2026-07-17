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
        When set, asserts that PRs whose title OR live commit-derived
        signals carry a Conventional Commits `fix:` or `feat:` prefix
        include either an accepted bump-marker commit subject prefix
        (`chore: bump versions` canonical, or legacy
        `chore(versions): bump`) in the diff range OR a
        `Version-Bump: skip reason="..."` trailer in that range.
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

Module layout: the Surface / heuristics / apply / render clusters live
in focused sibling modules and are re-exported here so this file remains
the stable CLI and import entrypoint. External importers
(`skill_sync_check.py` and the test suite) keep using
`from version_bump_check import ...` unchanged.

    version_bump_surfaces.py    Surface domain model, config loading,
                                version-file I/O
    version_bump_heuristics.py  git-diff helpers, conv-commit
                                classification, assess_surfaces pipeline
    version_bump_apply.py       bump arithmetic + apply_bumps writer
    version_bump_render.py      render_report
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess  # noqa: F401  (re-exported for external importers)
import sys
from pathlib import Path

# Shared gate helpers. `_strip_meta` is the version-bump-specific alias
# for `strip_meta`; keep the public alias so callers in this file and any
# external imports don't break.
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

# ── Re-exported cluster symbols ─────────────────────────────────────────
# Keep every public name re-exported so `from version_bump_check import X`
# keeps working for skill_sync_check.py, test_gates.py,
# test_version_bump_check_extra.py and any external caller.

from version_bump_surfaces import (
    LEVELS,
    VersionFile,
    Surface,
    Config,
    Verdict,
    load_config,
    _CMAKE_PROJECT_VERSION_RE,
    read_version,
    _json_walk_get,
    _json_walk_set,
    write_version,
    _extract_version_from_text,
    version_at_base,
    already_bumped,
)
from version_bump_heuristics import (
    git_diff_ignore_whitespace_nonempty,
    git_log_subjects_and_bodies,
    git_commit_files,
    filter_generated,
    is_revert_commit,
    classify_conventional,
    max_level,
    heuristic_for_surface,
    surface_trailer_override,
    assess_surfaces,
)
from version_bump_apply import (
    bump_version,
    apply_bumps,
)
from version_bump_render import (
    render_report,
)


# ── PR/commit fix/feat-needs-bump check ────────────────────────────────


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


def _release_subjects(subject: str, body: str) -> list[str]:
    """Release-bearing subjects represented by a landed commit.

    GitHub's multi-commit ``COMMIT_MESSAGES`` squash format keeps source
    subjects as ``* <subject>`` lines, then appends a ``---------`` co-author
    footer. Those source commits are no longer reachable from ``main``, so the
    post-merge backstop must recover their fix/feat signals from that stable
    message shape. A conventional landed subject remains the sole signal.
    """
    cleaned = subject.strip()
    signals = [cleaned] if _is_fix_or_feat_title(cleaned) else []
    separator = re.search(r"(?m)^---------\s*$", body)
    if separator is not None:
        source_messages = body[:separator.start()]
    elif re.search(r"\(#[0-9]+\)\s*$", cleaned):
        # A multi-commit squash without co-authors has no footer separator;
        # GitHub still appends the PR number to the landed title.
        source_messages = body
    else:
        source_messages = ""
    signals.extend(
        match.group(1).strip()
        for match in re.finditer(
            r"(?m)^\*[ \t]+((?:fix|feat)(?:\([^\r\n)]*\))?!?:[ \t]+[^\r\n]+)",
            source_messages,
        )
    )
    return list(dict.fromkeys(signals))


def _fix_feat_min_level(title: str) -> str | None:
    """Minimum bump level a `fix:` / `feat:` title demands, or None.

    `feat:` (a new user-facing capability) is a semver-minor; `fix:`
    (and the `fix!:` / `feat!:` breaking variants, which we still treat
    as their base type here — the signal alone doesn't carry the target
    surface) is a semver-patch. This mirrors `classify_conventional`'s subject
    mapping for either a PR title or commit-derived signal.
    """
    s = title.strip()
    m = _FIX_FEAT_TITLE_RE.match(s)
    if not m:
        return None
    return "minor" if m.group(1) == "feat" else "patch"


def force_fix_feat_verdicts(
    release_signal: str,
    min_levels_by_surface: dict[str, str],
    verdicts: list,
    recorded_levels: bool = False,
) -> tuple[list, str]:
    """Reconcile `--mode=apply` with `--require-bump-for-fix-feat`.

    Called before `apply_bumps` when the PR title or a live commit signal is
    `fix:` / `feat:` and the range has no effective global opt-out. Each
    signaled surface is raised to that surface's minimum (patch for `fix:`,
    minor for `feat:`), including surfaces hidden by scoped skips while a
    different surface has a normal verdict. Explicit numeric overrides remain
    authoritative during normal PR apply. Detector-recorded recovery levels
    are already boundary-filtered and are therefore applied exactly.

    Returns `(edited, message)`:
      - the verdict list to pass through the single apply operation.
      - `message` is an advisory line for non-edit outcomes: when NO
        versioned surface was touched at all, it is an actionable
        reclassify-or-skip message (the caller surfaces it and the
        downstream `--require-bump-for-fix-feat` check still fails, so
        this never silently no-ops). Empty string when surfaces were
        forced.
    """
    signaled_names = {
        verdict.surface.name
        for verdict in verdicts
        if verdict.surface.name in min_levels_by_surface
    }
    if not signaled_names:
        # No versioned surface touched — a `fix:` / `feat:` title on a
        # diff that only changes build/CI/docs/test paths (the P5
        # `build:`-class case). We refuse to silently no-op: emit an
        # actionable message and let `--require-bump-for-fix-feat` fail
        # so the author either reclassifies the title or records a skip.
        return [], (
            "fix/feat-needs-bump: PR title "
            f"{release_signal!r} is a user-facing `fix:` / `feat:` change but "
            "the diff touches NO versioned surface's trigger paths, so "
            "there is nothing to bump. This usually means the title is "
            "mislabeled (a `build:` / `ci:` / `docs:` / `test:` /\n"
            "`refactor:` change wearing a `fix:` / `feat:` hat).\n"
            "\n"
            "Resolution — pick one:\n"
            "  • Re-title the PR with the accurate Conventional Commits "
            "type (`build:` / `ci:` / `docs:` / `chore:` / `refactor:` / "
            "`test:`) — those are not release events and need no bump.\n"
            "  • If it genuinely IS user-facing but lives outside the "
            "configured surfaces, add a top-level trailer to the tip "
            "commit:\n"
            '      Version-Bump: skip reason="<why this doesn\'t need a release>"\n'
        )

    # Raise each signaled surface before the single apply operation. This
    # preserves a stronger normal verdict while ensuring a patch write cannot
    # make a later feature-level force look already satisfied. Numeric trailer
    # levels are an explicit author verdict and remain authoritative.
    forced = []
    for v in verdicts:
        min_level = min_levels_by_surface.get(v.surface.name)
        explicit_level = (
            v.trailer_override in LEVELS
            and v.trailer_override != "none"
        )
        final_level = v.final_level
        if min_level and recorded_levels:
            final_level = min_level
        elif min_level and not explicit_level:
            final_level = max_level(final_level, min_level)
        forced.append(Verdict(
            surface=v.surface,
            heuristic=v.heuristic,
            trailer_override=v.trailer_override,
            current_version=v.current_version,
            final_level=final_level,
        ))
    return forced, ""


def _signal_files(sha: str) -> list[str]:
    """Files introduced by a release-signal commit.

    A merge commit's ordinary ``git show --name-only`` result is not a stable
    description of what the merge introduced. Compare its first-parent tree to
    the merge tree so a later, unrelated commit in the pushed range cannot be
    attributed to the merge subject.
    """
    parent_line = subprocess.run(
        ["git", "rev-list", "--parents", "-n", "1", sha],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.split()
    if len(parent_line) <= 2:
        return git_commit_files(sha)
    result = subprocess.run(
        ["git", "diff", "--name-only", f"{parent_line[1]}..{sha}"],
        check=True,
        capture_output=True,
        text=True,
    )
    return [line for line in result.stdout.splitlines() if line.strip()]


def _signal_is_embedded(sha: str, signal_subject: str) -> bool:
    landed_subject = subprocess.run(
        ["git", "show", "-s", "--format=%s", sha],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    return signal_subject.strip() != landed_subject


def _fix_feat_levels_by_surface(
    base: str,
    head: str,
    cfg: Config,
    changed: list[str],
    pr_title: str,
) -> dict[str, str]:
    """Return the strongest live fix/feat level owed by each surface."""
    levels: dict[str, str] = {}

    def add_signal(signal: str, files: list[str]) -> None:
        level = _fix_feat_min_level(signal)
        if level is None:
            return
        for surface in cfg.surfaces:
            if any(_matches_any(path, surface.trigger_paths) for path in files):
                levels[surface.name] = max_level(
                    levels.get(surface.name, "none"),
                    level,
                )

    for sha, subject in _range_fix_feat_signals(base, head):
        # Embedded COMMIT_MESSAGES subjects share the landed squash SHA but not
        # its aggregate file set. Recovery receives detector-recorded levels;
        # normal PR apply sees the original commits and never needs this unsafe
        # aggregate fallback.
        files = [] if _signal_is_embedded(sha, subject) else _signal_files(sha)
        add_signal(subject, files)
    if _is_fix_or_feat_title(pr_title):
        add_signal(pr_title, changed)
    return levels


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


def _range_fix_feat_signals(base: str, head: str) -> list[tuple[str, str]]:
    """Return live `(sha, subject)` signals contributed by ``base..head``.

    GitHub's ``COMMIT_OR_PR_TITLE`` squash policy uses the sole commit's
    subject for a one-commit PR, but the required pre-merge gate receives the
    separately editable PR title. Looking at only that title lets a plain-
    language title hide the exact Conventional Commit subject that will land
    on ``main``. Scanning the range also covers rebase and merge-commit flows
    without reproducing one repository's current squash settings here.

    Explicit reverts cancel the signal introduced by their target commit. For
    merge targets, that signal is measured against the mainline parent recorded
    by standard `git revert` metadata (with the first parent as the fallback for
    a trailer-only revert). Reverting the revert restores the same signal, even
    when the original `fix:` / `feat:` commit predates ``base``. This follows
    commit relationships instead of path names, which remain reliable across
    later renames and unrelated edits to the same file.
    """
    records: dict[str, tuple[str, str, list[str]]] = {}
    trailers_by_sha: dict[str, dict[str, list[str]]] = {}
    release_subjects_by_sha: dict[str, list[str]] = {}
    target_by_sha: dict[str, tuple[bool, str | None]] = {}
    commit_delta_cache: dict[str, dict[str, int]] = {}
    range_delta_cache: dict[tuple[str, str], dict[str, int]] = {}
    resolving: set[str] = set()

    def resolve_commit(ref: str) -> str | None:
        result = subprocess.run(
            ["git", "rev-parse", "--verify", "--end-of-options", f"{ref}^{{commit}}"],
            capture_output=True,
            text=True,
        )
        return result.stdout.strip() if result.returncode == 0 else None

    def commit_record(ref: str) -> tuple[str, str, list[str]] | None:
        sha = resolve_commit(ref)
        if sha is None:
            return None
        if sha not in records:
            result = subprocess.run(
                ["git", "show", "-s", "--format=%P%x00%s%x00%B", sha],
                check=True,
                capture_output=True,
                text=True,
            )
            parents, subject, body = result.stdout.split("\x00", 2)
            records[sha] = (subject, body, parents.split())
        return records[sha]

    def revert_target(sha: str) -> tuple[bool, str | None]:
        if sha in target_by_sha:
            return target_by_sha[sha]

        record = commit_record(sha)
        if record is None:  # pragma: no cover - sha came from git log/show
            return False, None
        subject, body, _parents = record
        trailers = trailers_by_sha.setdefault(sha, git_commit_trailers(sha))
        body_match = re.search(
            r"(?im)^This reverts commit ([0-9a-f]{7,64})(?:[.,]|\s|$)",
            body,
        )
        declared_revert = (
            is_revert_commit(subject, trailers) or body_match is not None
        )
        if not declared_revert:
            target_by_sha[sha] = (False, None)
            return target_by_sha[sha]

        candidates = trailers.get("revert-of", [])
        if body_match:
            candidates = [*candidates, body_match.group(1)]

        target = None
        for candidate in candidates:
            match = re.search(r"\b[0-9a-fA-F]{7,64}\b", candidate)
            if match:
                target = resolve_commit(match.group(0))
            if target is not None:
                break
        # Invalid or stale revert metadata must not hide a real release signal.
        # A conventional Revert subject remains non-release-bearing, but a
        # `fix:` / `feat:` subject only gains revert semantics after its target
        # resolves in the fetched history.
        is_revert = target is not None or not _is_fix_or_feat_title(subject)
        target_by_sha[sha] = (is_revert, target)
        return target_by_sha[sha]

    def release_subjects(sha: str) -> list[str]:
        if sha not in release_subjects_by_sha:
            record = commit_record(sha)
            if record is None:
                release_subjects_by_sha[sha] = []
            else:
                release_subjects_by_sha[sha] = _release_subjects(
                    record[0],
                    record[1],
                )
        return release_subjects_by_sha[sha]

    def add_delta(
        destination: dict[str, int],
        source: dict[str, int],
        multiplier: int = 1,
    ) -> None:
        for candidate_sha, value in source.items():
            total = destination.get(candidate_sha, 0) + multiplier * value
            if total:
                destination[candidate_sha] = total
            else:
                destination.pop(candidate_sha, None)

    def range_delta(range_base: str, range_head: str) -> dict[str, int]:
        key = (range_base, range_head)
        if key in range_delta_cache:
            return dict(range_delta_cache[key])

        delta: dict[str, int] = {}
        for sha, subject, body in git_log_subjects_and_bodies(
            range_base,
            range_head,
        ):
            record = commit_record(sha)
            if record is not None:
                records[sha] = (subject, body, record[2])
            is_revert, _target = revert_target(sha)
            if is_revert:
                add_delta(delta, commit_delta(sha))
            elif release_subjects(sha):
                add_delta(delta, {sha: 1})

        range_delta_cache[key] = dict(delta)
        return delta

    def reverted_delta(revert_sha: str, target_sha: str) -> dict[str, int]:
        target_record = commit_record(target_sha)
        if target_record is None:
            return {}
        _subject, _body, target_parents = target_record
        if len(target_parents) <= 1:
            return commit_delta(target_sha)

        revert_record = commit_record(revert_sha)
        revert_body = revert_record[1] if revert_record is not None else ""
        mainline_match = re.search(
            r"(?im)reversing\s+changes made to ([0-9a-f]{7,64})",
            revert_body,
        )
        mainline = None
        if mainline_match:
            resolved = resolve_commit(mainline_match.group(1))
            if resolved in target_parents:
                mainline = resolved
        if mainline is None:
            mainline = target_parents[0]
        return range_delta(mainline, target_sha)

    def commit_delta(sha: str) -> dict[str, int]:
        if sha in commit_delta_cache:
            return dict(commit_delta_cache[sha])
        if sha in resolving:
            return {}

        resolving.add(sha)
        try:
            record = commit_record(sha)
            if record is None:
                delta: dict[str, int] = {}
            else:
                subject, _body, parents = record
                is_revert, target = revert_target(sha)
                if is_revert:
                    delta = {}
                    if target is not None:
                        add_delta(delta, reverted_delta(sha, target), -1)
                elif len(parents) > 1:
                    delta = range_delta(parents[0], sha)
                elif release_subjects(sha):
                    delta = {sha: 1}
                else:
                    delta = {}
            commit_delta_cache[sha] = dict(delta)
            return delta
        finally:
            resolving.remove(sha)

    commits = git_log_subjects_and_bodies(base, head)
    if not commits:
        return []
    live = range_delta(base, head)
    signals: list[tuple[str, str]] = []
    for sha, value in live.items():
        if value <= 0:
            continue
        record = commit_record(sha)
        if record is None:  # pragma: no cover - live keys resolve from git
            continue
        is_revert, _target = revert_target(sha)
        if not is_revert:
            signals.extend((sha, signal) for signal in release_subjects(sha))
    return signals


def _range_fix_feat_subjects(base: str, head: str) -> list[str]:
    return [subject for _sha, subject in _range_fix_feat_signals(base, head)]


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


def _range_unreleased_fix_feat_subjects(
    base: str,
    head: str,
    cfg: Config,
    covered_surfaces: dict[str, bool],
    coverage_boundaries: dict[str, str] | None = None,
    source_range: tuple[str, str] | None = None,
) -> tuple[list[tuple[str, list[str]]], dict[str, str]]:
    """Return live signals and surfaces not covered by a tag or skip boundary."""
    if _range_has_version_bump_skip_trailer(base, head):
        return [], {}
    if source_range and _range_has_version_bump_skip_trailer(*source_range):
        return [], {}

    boundaries = coverage_boundaries or {}
    by_signal: dict[tuple[str, str], list[str]] = {}
    levels: dict[str, str] = {}
    for surface in cfg.surfaces:
        if covered_surfaces.get(surface.name, False):
            continue
        signal_base = base
        boundary = boundaries.get(surface.name)
        if boundary:
            # A sticky skip only narrows this push when its bump lies inside
            # base..head. If it predates base, restarting at the old bump would
            # rediscover already-tracked fixes on every unrelated later push.
            boundary_in_range = subprocess.run(
                ["git", "merge-base", "--is-ancestor", base, boundary],
                capture_output=True,
                text=True,
            )
            boundary_reaches_head = subprocess.run(
                ["git", "merge-base", "--is-ancestor", boundary, head],
                capture_output=True,
                text=True,
            )
            if (
                boundary_in_range.returncode == 0
                and boundary_reaches_head.returncode == 0
            ):
                signal_base = boundary
        analysis_base = signal_base
        analysis_head = head
        if source_range and signal_base == base:
            analysis_base, analysis_head = source_range
        trailers = git_range_trailers(analysis_base, analysis_head)
        explicit_level = surface_trailer_override(
            trailers,
            cfg.trailer_version_bump,
            surface.name,
        )
        boundary_changed = filter_generated(
            git_diff_names(analysis_base, analysis_head),
            cfg.generated_globs,
        )
        boundary_heuristic = heuristic_for_surface(
            surface,
            boundary_changed,
            analysis_base,
            analysis_head,
        )
        for sha, subject in _range_fix_feat_signals(analysis_base, analysis_head):
            files = _signal_files(sha)
            if not source_range and _signal_is_embedded(sha, subject):
                touched_surfaces = [
                    candidate.name
                    for candidate in cfg.surfaces
                    if any(
                        _matches_any(path, candidate.trigger_paths)
                        for path in files
                    )
                ]
                if touched_surfaces != [surface.name]:
                    continue
            if not any(
                _matches_any(path, surface.trigger_paths)
                for path in files
            ):
                continue
            by_signal.setdefault((sha, subject), []).append(surface.name)
            signal_level = _fix_feat_min_level(subject) or "none"
            if explicit_level in ("patch", "minor", "major"):
                levels[surface.name] = explicit_level
            else:
                levels[surface.name] = max_level(
                    levels.get(surface.name, "none"),
                    signal_level,
                    boundary_heuristic,
                )
    signals = [
        (subject, surfaces)
        for (_sha, subject), surfaces in by_signal.items()
    ]
    return signals, levels


def check_fix_feat_requires_bump(
    pr_title: str,
    base: str,
    head: str,
) -> tuple[bool, str]:
    """Returns (passed, message). `passed=True` means either:

    - neither the PR title nor a live commit-derived signal is a
      `fix:` / `feat:` (no requirement), OR
    - a release signal exists and the diff range contains a bump commit, OR
    - a release signal exists and the range carries a top-level
      `Version-Bump: skip reason="..."` trailer.

    Otherwise returns (False, error-with-suggestions).
    """
    title_matches = _is_fix_or_feat_title(pr_title) if pr_title else False
    commit_subjects = [] if title_matches else _range_fix_feat_subjects(base, head)

    if title_matches:
        release_signal = f"PR title {pr_title!r}"
    elif commit_subjects:
        release_signal = f"commit subject {commit_subjects[0]!r}"
    elif not pr_title or not pr_title.strip():
        # Defensive: no title supplied and no classifiable subject means the
        # workflow has no release signal. The per-surface verdict remains
        # authoritative on push events and workflow_dispatch.
        return True, (
            "fix/feat-needs-bump: PR title not provided and no `fix:` / "
            "`feat:` commit subject found; skipping check (this is normal on "
            "push events and workflow_dispatch)."
        )
    else:
        return True, (
            f"fix/feat-needs-bump: PR title {pr_title!r} is not a `fix:` or "
            "`feat:` user-facing change, and no commit subject is one — no "
            "bump required."
        )

    if _range_has_bump_commit(base, head):
        return True, (
            f"fix/feat-needs-bump: {release_signal} matches; "
            "found `chore: bump versions` commit in the diff range — OK."
        )

    if _range_has_version_bump_skip_trailer(base, head):
        return True, (
            f"fix/feat-needs-bump: {release_signal} matches; "
            'no bump commit found, but a `Version-Bump: skip reason="..."` '
            "trailer is present in the range — bypass honored."
        )

    return False, (
        f"fix/feat-needs-bump: {release_signal} is a user-facing "
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
        "instead), add a top-level trailer to a commit in the range:\n"
        '      Version-Bump: skip reason="<why this fix doesn\'t need a release>"\n'
        "  • Branch protection on `main` SHOULD make this an enforced "
        "required check; see docs/guides/release-watchdog.md."
    )


def verify_applied_bumps(
    verdicts: list[Verdict],
    base: str,
    repo: Path,
    accept_intent_trailers: bool = False,
) -> str | None:
    """Post-apply proof: every surface that owed a bump is now ahead of `base`.

    `--mode=apply` computes verdicts, then asks `apply_bumps` to write. Every
    step in between can decline to write while returning normally: the
    already-bumped short-circuit, an unreadable `current_version`, or a
    `write_version` whose pattern no longer matches a reformatted file. None of
    those raise, so apply could print `✓ bumped` and exit 0 having written
    nothing — the gate meant to enforce the bump silently not enforcing it.

    So don't infer the write from the code path that requested it; re-read the
    files and check. This is the apply-mode counterpart of the no-surface exit
    below (issue #4679 acceptance (b)): apply must never report success on a
    bump it did not actually land. Returns an actionable message, or None when
    every owed bump is on disk.

    `patch` matters most here — `render_report` scores an unbumped patch as an
    advisory `?` and still exits 0, so a silently-dropped patch write is
    invisible to the report alone.
    """
    stranded: list[str] = []
    for v in verdicts:
        if v.final_level == "none":
            continue
        # An accepted intent trailer defers the write to merge-time
        # automation by design; the files are legitimately unmoved.
        if (
            accept_intent_trailers
            and v.trailer_override in LEVELS
            and v.trailer_override != "none"
        ):
            continue
        for vf in v.surface.version_files:
            if already_bumped(base, vf, repo):
                continue
            base_ver = version_at_base(base, vf)
            if base_ver is None:
                # No version at base — the file is new in this branch (a
                # surface this PR adds), or the base ref doesn't resolve.
                # Either way there's no ordering to compare, so absence of
                # "ahead of base" is not evidence of a dropped write.
                continue
            stranded.append(
                f"  [{v.surface.name}] {vf.path}: "
                f"base={base_ver} head={read_version(repo, vf) or '?'} "
                f"(wanted a {v.final_level} bump)"
            )
    if not stranded:
        return None
    return (
        "version-bump apply: refusing to report success — apply ran but these "
        "version files are NOT ahead of the base:\n"
        + "\n".join(stranded)
        + "\n\nThe bump was requested and never landed on disk, so the "
        "`chore: bump versions` marker the caller is about to write would be "
        "a lie and the CI fix/feat gate would fail downstream with no "
        "explanation.\n"
        "\nMost likely: this branch's base has moved on and the version line "
        f"is stale. Rebase, then re-run:\n"
        f"    git rebase {base}\n"
        "    python3 tools/scripts/version_bump_check.py --mode=apply\n"
        "If a rebase does not resolve it, the version file's pattern in "
        "tools/scripts/versioning.json no longer matches the file's contents."
    )


# ── Main ────────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    # If invoked as `version_bump_check.py classify-subject <subject>`,
    # exit 0 when the subject matches the fix/feat regex and 1 otherwise.
    # This lets .github/workflows/auto-release.yml's stranded-fix detector
    # call the script for classification instead of duplicating
    # `_FIX_FEAT_TITLE_RE` inline (the duplication was a documented
    # lock-step drift risk).
    if len(argv) >= 2 and argv[0] == "classify-subject":
        return 0 if _is_fix_or_feat_title(argv[1]) else 1
    if len(argv) >= 3 and argv[0] == "classify-range":
        if _range_has_version_bump_skip_trailer(argv[1], argv[2]):
            return 1
        signals = _range_fix_feat_subjects(argv[1], argv[2])
        if not signals:
            return 1
        print(signals[0])
        return 0
    if len(argv) >= 5 and argv[0] == "classify-unreleased-range":
        if argv[3] not in ("0", "1") or argv[4] not in ("0", "1"):
            return 2
        root = repo_root()
        config_path = Path(argv[5]) if len(argv) >= 6 else (
            root / "tools" / "scripts" / "versioning.json"
        )
        sdk_boundary = argv[6] if len(argv) >= 7 and argv[6] != "-" else ""
        plugin_boundary = argv[7] if len(argv) >= 8 and argv[7] != "-" else ""
        source_base = argv[8] if len(argv) >= 9 and argv[8] != "-" else ""
        source_head = argv[9] if len(argv) >= 10 and argv[9] != "-" else ""
        cfg = load_config(config_path)
        signals, levels = _range_unreleased_fix_feat_subjects(
            argv[1],
            argv[2],
            cfg,
            {"sdk": argv[3] == "1", "plugin": argv[4] == "1"},
            {"sdk": sdk_boundary, "plugin": plugin_boundary},
            (source_base, source_head) if source_base and source_head else None,
        )
        if not signals:
            return 1
        subject = signals[0][0]
        signaled_surfaces = {
            surface for _subject, surfaces in signals for surface in surfaces
        }
        surfaces = [
            surface.name
            for surface in cfg.surfaces
            if surface.name in signaled_surfaces
        ]
        print(json.dumps({
            "subject": subject,
            "surfaces": surfaces,
            "levels": levels,
        }))
        return 0

    parser = argparse.ArgumentParser(description="Version-bump gate")
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--config", default=None)
    parser.add_argument("--mode", choices=("report", "hint", "apply"), default="report")
    parser.add_argument("--repo-root", default=None)
    parser.add_argument(
        "--apply-version-base",
        default=None,
        help=(
            "In apply mode, compute new versions from this ref while retaining "
            "--base/--head for changed-path and release-signal analysis. "
            "Defaults to --base."
        ),
    )
    parser.add_argument(
        "--recover-stranded-release",
        action="store_true",
        help=(
            "In apply mode, force every touched surface to at least the live "
            "fix/feat signal's minimum level even when the historical range "
            "already contains a bump-marker subject."
        ),
    )
    parser.add_argument(
        "--recover-surfaces",
        default="",
        help=(
            "Comma-separated surface names to edit during stranded-release "
            "recovery. Requires --recover-stranded-release."
        ),
    )
    parser.add_argument(
        "--recover-levels",
        default="",
        help=(
            "Comma-separated <surface>=<patch|minor|major> levels recorded by "
            "the stranded-release detector. Valid only with "
            "--recover-stranded-release."
        ),
    )
    parser.add_argument(
        "--require-bump-for-fix-feat",
        action="store_true",
        help=(
            "Additively require that a `fix:`/`feat:` PR title (read "
            "from $GITHUB_PR_TITLE or --pr-title) or commit subject "
            "include either a "
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
            "Defaults to $GITHUB_PR_TITLE. An empty / unset title still "
            "allows commit-subject classification."
        ),
    )
    parser.add_argument(
        "--accept-intent-trailers",
        action="store_true",
        help=(
            "Intent-trailer model (C3): when set, the gate accepts an "
            "explicit `Version-Bump: <surface>=<patch|minor|major>` "
            "trailer in lieu of actually bumping the version files. The "
            "trailer declares INTENT; merge-time automation rewrites "
            "files on merge using the next-available version from main. "
            "Two PRs both declaring `sdk=minor` then don't race on the "
            "exact number — each one's exact target is computed at "
            "merge time, eliminating the force-push tax. Off by "
            "default until Shipyard / merge automation supports the "
            "rewrite step."
        ),
    )
    args = parser.parse_args(argv)

    if args.recover_stranded_release and args.mode != "apply":
        sys.stderr.write(
            "version_bump_check: --recover-stranded-release requires --mode=apply\n"
        )
        return 2

    root = Path(args.repo_root) if args.repo_root else repo_root()
    cfg_path = Path(args.config) if args.config else root / "tools" / "scripts" / "versioning.json"
    if not cfg_path.exists():
        sys.stderr.write(f"version_bump_check: config not found: {cfg_path}\n")
        return 2

    cfg = load_config(cfg_path)
    recover_surfaces = {
        name.strip() for name in args.recover_surfaces.split(",") if name.strip()
    }
    recover_levels: dict[str, str] = {}
    for item in (part.strip() for part in args.recover_levels.split(",")):
        if not item:
            continue
        if "=" not in item:
            sys.stderr.write(
                "version_bump_check: --recover-levels entries must be "
                "<surface>=<patch|minor|major>\n"
            )
            return 2
        name, level = (part.strip() for part in item.split("=", 1))
        if level not in ("patch", "minor", "major"):
            sys.stderr.write(
                f"version_bump_check: invalid recovery level for {name}: {level}\n"
            )
            return 2
        recover_levels[name] = level
    if recover_surfaces and not args.recover_stranded_release:
        sys.stderr.write(
            "version_bump_check: --recover-surfaces requires "
            "--recover-stranded-release\n"
        )
        return 2
    if recover_levels and not args.recover_stranded_release:
        sys.stderr.write(
            "version_bump_check: --recover-levels requires "
            "--recover-stranded-release\n"
        )
        return 2
    known_surfaces = {surface.name for surface in cfg.surfaces}
    unknown_surfaces = (recover_surfaces | set(recover_levels)) - known_surfaces
    if unknown_surfaces:
        sys.stderr.write(
            "version_bump_check: unknown recovery surface(s): "
            + ", ".join(sorted(unknown_surfaces))
            + "\n"
        )
        return 2
    if recover_levels and recover_surfaces != set(recover_levels):
        sys.stderr.write(
            "version_bump_check: --recover-levels must name exactly the "
            "--recover-surfaces set\n"
        )
        return 2

    changed = git_diff_names(args.base, args.head)
    changed = filter_generated(changed, cfg.generated_globs)

    verdicts = assess_surfaces(cfg, changed, args.base, args.head, root)

    if args.mode == "apply":
        apply_base = args.apply_version_base or args.base
        apply_verdicts = [
            verdict
            for verdict in verdicts
            if not recover_surfaces or verdict.surface.name in recover_surfaces
        ]

        # Reconcile apply with `--require-bump-for-fix-feat` (issue #4679).
        # A branch cut from a recent merge inherits a version EQUAL to its
        # merge-base; the per-surface heuristic then finds no delta →
        # `apply_bumps` writes nothing → CI's fix/feat gate hard-fails the
        # `fix:` / `feat:` PR for lacking a `chore: bump versions` commit.
        # When the title or a live commit subject IS `fix:` / `feat:`, raise
        # every touched surface to that signal's minimum. Run this even when
        # normal apply edited a different surface, so one valid bump cannot
        # mask a scoped skip on the surface carrying the fix.
        forced_no_surface_msg = ""
        forced_signal = ""
        pr_title = (
            args.pr_title if args.pr_title is not None
            else os.environ.get("GITHUB_PR_TITLE", "")
        )
        force_signals = _range_fix_feat_subjects(args.base, args.head)
        if _is_fix_or_feat_title(pr_title):
            force_signals.append(pr_title)
        force_levels = recover_levels or _fix_feat_levels_by_surface(
            args.base,
            args.head,
            cfg,
            changed,
            "" if args.recover_stranded_release else pr_title,
        )
        if force_signals:
            forced_signal = max(
                force_signals,
                key=lambda signal: LEVELS.index(
                    _fix_feat_min_level(signal) or "none"
                ),
            )

        # Normal apply honors an existing marker/global skip. Recovery ignores
        # a historical marker because the tracker proves no usable version
        # movement resulted, but it still honors an explicit global skip.
        ff_would_pass, _ff_pre_msg = check_fix_feat_requires_bump(
            pr_title, args.base, args.head,
        )
        global_skip = _range_has_version_bump_skip_trailer(args.base, args.head)
        should_force = forced_signal and (
            not ff_would_pass
            or (args.recover_stranded_release and not global_skip)
        )
        if should_force:
            apply_verdicts, forced_no_surface_msg = force_fix_feat_verdicts(
                forced_signal,
                force_levels,
                apply_verdicts,
                recorded_levels=bool(recover_levels),
            )
        edited = apply_bumps(apply_verdicts, apply_base, root)

        # Re-assess after editing: re-read current versions and re-check.
        verdicts_after = assess_surfaces(cfg, changed, args.base, args.head, root)
        if recover_surfaces:
            verdicts_after = [
                verdict
                for verdict in verdicts_after
                if verdict.surface.name in recover_surfaces
            ]
        text, code = render_report(
            verdicts_after, mode="report", base=apply_base, repo=root,
            accept_intent_trailers=args.accept_intent_trailers,
        )
        if edited:
            print("Edited files:")
            for e in edited:
                print(f"  {e}")
        if text:
            print(text)
        # Prove the writes landed before anyone acts on this exit code.
        stranded_msg = verify_applied_bumps(
            verdicts_after, apply_base, root,
            accept_intent_trailers=args.accept_intent_trailers,
        )
        if stranded_msg:
            sys.stderr.write(stranded_msg + "\n")
            if code == 0:
                code = 1
        if forced_no_surface_msg:
            # No versioned surface was touched — print the actionable
            # reclassify/skip message so the caller (and shipyard pr) sees
            # *why* nothing bumped instead of a silent no-op. Force a
            # non-zero exit even without `--require-bump-for-fix-feat`:
            # apply mode must not silently succeed on a `fix:` / `feat:`
            # title it could not reconcile (issue #4679 acceptance (b)).
            sys.stderr.write(forced_no_surface_msg + "\n")
            if code == 0:
                code = 1
        if args.recover_stranded_release and not edited:
            sys.stderr.write(
                "fix/feat-needs-bump: recovery produced no version edit; "
                "refusing to report success.\n"
            )
            return 1
        # `--require-bump-for-fix-feat` is meaningful in apply mode too:
        # if `pulp pr` ran apply and STILL couldn't produce a bump (no
        # surface touched, or the author opted out without a trailer),
        # the check should still flag it. After the force-bump above the
        # common branch==base case now passes here instead of failing at
        # the CI gate.
        if args.require_bump_for_fix_feat:
            if edited:
                # We just wrote version files this run; the caller appends the
                # `chore: bump versions` commit AFTER apply, so that marker is
                # not in the immutable input range yet. Do not fail on the
                # marker we are in the middle of creating.
                print(
                    "fix/feat-needs-bump: applied a pending bump for "
                    f"{(forced_signal or pr_title)!r}; the caller will append the "
                    "`chore: bump versions` commit — OK."
                )
            else:
                ff_passed, ff_msg = check_fix_feat_requires_bump(
                    pr_title,
                    args.base, args.head,
                )
                print(ff_msg)
                if not ff_passed:
                    return 1
        return code

    text, code = render_report(
        verdicts, args.mode, args.base, root,
        accept_intent_trailers=args.accept_intent_trailers,
    )
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
