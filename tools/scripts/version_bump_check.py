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
    # If invoked as `version_bump_check.py classify-subject <subject>`,
    # exit 0 when the subject matches the fix/feat regex and 1 otherwise.
    # This lets .github/workflows/auto-release.yml's stranded-fix detector
    # call the script for classification instead of duplicating
    # `_FIX_FEAT_TITLE_RE` inline (the duplication was a documented
    # lock-step drift risk).
    if len(argv) >= 2 and argv[0] == "classify-subject":
        return 0 if _is_fix_or_feat_title(argv[1]) else 1

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
        text, code = render_report(
            verdicts_after, mode="report", base=args.base, repo=root,
            accept_intent_trailers=args.accept_intent_trailers,
        )
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
