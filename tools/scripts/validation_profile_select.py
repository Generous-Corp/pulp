#!/usr/bin/env python3
"""Select Shipyard validation profile based on diff scope.

A PR that only touches runtime-import parser code — the standalone
import-design tool, the import-validation scripts, the pulp-import-ir
package, parser fixtures, and the parser test surface — does not need
to pay for plugin-validator runs (auval / pluginval / clap-validator),
example builds, or the broader format-adapter smoke suite. This script
classifies a diff and prints the validation profile name that matches:

    parser   → narrow lane defined in .shipyard/config.toml
               ([validation.parser]) — parser unit tests, source
               roundtrip scripts, screenshot fixture rendering.

    default  → broad lane ([validation.default]) — full build matrix,
               examples, plugin validators.

The path set is intentionally small. Adding a new path is a deliberate
decision — if the change can reasonably affect non-parser surfaces,
keep it on the default lane.

Usage:

    # Compare HEAD against the merge base with origin/main:
    python3 tools/scripts/validation_profile_select.py

    # Different base (e.g. develop branch):
    python3 tools/scripts/validation_profile_select.py --base origin/develop

    # Operate on a literal file list (one path per line, '-' for stdin):
    python3 tools/scripts/validation_profile_select.py --paths-from -

    # JSON envelope (profile + classification + matched/unmatched paths):
    python3 tools/scripts/validation_profile_select.py --json

Exit code is always 0 when classification succeeds. A non-zero exit
means the git query failed (no base reachable, not a git repo, etc.) —
in that case the caller should fall back to the default lane.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


# Glob patterns that delimit the parser-only scope. fnmatch is used in
# the classifier; `**` is normalized to multi-segment match by walking
# each path segment. Keep this list in sync with
# docs/guides/validation-profiles.md.
PARSER_ONLY_PATTERNS: tuple[str, ...] = (
    # Standalone import-design CLI tool + catalogs
    "tools/import-design/**",
    # Import-validation scripts (label coverage, spectr-roundtrip,
    # diff_against_reference, etc.)
    "tools/import-validation/**",
    # JS/TS IR package and its tests
    "packages/pulp-import-ir/**",
    # Import fixtures consumed by parser tests
    "test/fixtures/imports/**",
    # Parser-specific C++ tests in the test/ directory
    "test/test_design_import.cpp",
    "test/test_design_import_*.cpp",
    "test/test_design_export.cpp",
    "test/test_cli_import_design.cpp",
    "test/test_cli_import_detect.cpp",
    "test/test_import_design_tool.cpp",
    "test/test_widget_promotion.cpp",
    "test/test_screenshot_compare.cpp",
    "test/test_cli_design_binding.cpp",
    "test/test_spectr_roundtrip_window_only_capture.sh",
    # Parser-side core/view code (matches the import-design skill's
    # tracked paths so the two stay aligned).
    "core/view/src/design_import*",
    "core/view/include/pulp/view/design_import.hpp",
    "core/view/include/pulp/view/design_import*",
    "core/view/js/import-runtime.js",
    # Public docs scoped to imports
    "docs/reference/imports/**",
    # Tracked skill for the import-design subsystem
    ".agents/skills/import-design/**",
)


@dataclass(frozen=True)
class Classification:
    profile: str
    matched: tuple[str, ...]
    unmatched: tuple[str, ...]


# ── Path matching ──────────────────────────────────────────────────


def _segments(pattern_or_path: str) -> list[str]:
    return [seg for seg in pattern_or_path.split("/") if seg]


def _match_recursive(pattern: str, path: str) -> bool:
    """fnmatch-style matcher that interprets `**` as multi-segment."""

    pat_parts = _segments(pattern)
    path_parts = _segments(path)
    return _match_parts(pat_parts, path_parts)


def _match_parts(pat_parts: Sequence[str], path_parts: Sequence[str]) -> bool:
    if not pat_parts:
        return not path_parts
    head, *rest = pat_parts
    if head == "**":
        # Match zero or more path segments.
        for i in range(len(path_parts) + 1):
            if _match_parts(rest, path_parts[i:]):
                return True
        return False
    if not path_parts:
        return False
    if not fnmatch.fnmatchcase(path_parts[0], head):
        return False
    return _match_parts(rest, path_parts[1:])


def is_parser_only_path(path: str) -> bool:
    """Return True iff `path` falls inside the parser-only scope."""

    for pattern in PARSER_ONLY_PATTERNS:
        if _match_recursive(pattern, path):
            return True
    return False


def classify(paths: Iterable[str]) -> Classification:
    matched: list[str] = []
    unmatched: list[str] = []
    for raw in paths:
        path = raw.strip()
        if not path:
            continue
        (matched if is_parser_only_path(path) else unmatched).append(path)
    # An empty diff still defaults to the broad lane — a no-op PR has
    # no meaningful scope to narrow, so fall through to safety.
    profile = "parser" if matched and not unmatched else "default"
    return Classification(
        profile=profile,
        matched=tuple(matched),
        unmatched=tuple(unmatched),
    )


# ── Diff source helpers ────────────────────────────────────────────


def _run_git(args: Sequence[str]) -> str:
    result = subprocess.run(
        ["git", *args],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout


def changed_paths_from_git(base: str) -> list[str]:
    """Return paths changed between `base` and HEAD.

    Uses `git diff --name-only <base>...HEAD` so the merge-base is the
    comparison anchor — matches how Shipyard / CI inspect a PR diff.
    """

    out = _run_git(["diff", "--name-only", f"{base}...HEAD"])
    return [line for line in out.splitlines() if line.strip()]


def paths_from_file(source: str) -> list[str]:
    if source == "-":
        return [line for line in sys.stdin.read().splitlines() if line.strip()]
    text = Path(source).read_text()
    return [line for line in text.splitlines() if line.strip()]


# ── CLI ────────────────────────────────────────────────────────────


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Print the Shipyard validation profile for the current diff.",
    )
    p.add_argument(
        "--base",
        default="origin/main",
        help="Git ref to diff HEAD against (default: origin/main).",
    )
    p.add_argument(
        "--paths-from",
        metavar="FILE",
        help="Read changed paths from FILE (or '-' for stdin) instead of git diff.",
    )
    p.add_argument(
        "--json",
        action="store_true",
        help="Emit a JSON envelope (profile + matched + unmatched).",
    )
    return p


def main(argv: Sequence[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)

    try:
        if args.paths_from:
            paths = paths_from_file(args.paths_from)
        else:
            paths = changed_paths_from_git(args.base)
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(
            f"validation_profile_select: git query failed ({exc.returncode}): "
            f"{exc.stderr.strip()}\n"
        )
        return 2
    except FileNotFoundError as exc:
        sys.stderr.write(f"validation_profile_select: {exc}\n")
        return 2

    result = classify(paths)

    if args.json:
        sys.stdout.write(
            json.dumps(
                {
                    "profile": result.profile,
                    "matched": list(result.matched),
                    "unmatched": list(result.unmatched),
                },
                indent=2,
            )
            + "\n"
        )
    else:
        sys.stdout.write(result.profile + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
