#!/usr/bin/env python3
"""Classify a PR's changed files to decide whether a native build is required.

Single source of truth for the `classify` gate in
`.github/workflows/build.yml`. Pure function: a list of changed file
paths -> `native_build_required` (bool). When false, the build.yml
`build` matrix (Linux + Windows + macOS Skia/Dawn compile) and the
`windows-msvc-release-gate` job are skipped at the job level — they
never get allocated a runner.

FAIL-CLOSED CONTRACT
--------------------
Skipping the native build is the optimization; running it is the safe
default. Any uncertainty, error, or empty input -> `native_build_required
= True`. A misclassification that wrongly *skips* a build lets a real
regression merge — so the skip-safe set is a deliberately small,
conservative allowlist. The native build is required UNLESS *every*
changed file is provably irrelevant to the C++/Swift build.

WHAT IS SKIP-SAFE
-----------------
- Any `*.md` file, anywhere (README, CHANGELOG, CONTRIBUTING, SKILL.md, ...).
- Files under `docs/`, `planning/`, `.githooks/`, `.shipyard/`,
  `.shipyard.local/` — none are CMake/build inputs.
- A short exact-name list (`.gitignore`, `.gitattributes`, `CODEOWNERS`).

EVERYTHING ELSE forces the native build — including `core/**`,
`apple/**`, `examples/**`, `test/**`, every `CMakeLists.txt`,
`tools/cmake/**`, `.github/workflows/**` (a `build.yml` change MUST get
a real run to validate itself), `tools/scripts/**` (some are
build-coupled), `*.toml`/`*.json` config, this classifier itself, and
`docs/migrations/*.md` (globbed with CONFIGURE_DEPENDS into the
generated `migration_index.cpp` by `tools/cli/CMakeLists.txt` — a
deny-list exception that overrides the `.md`/`docs/` skip-safe rules).
That is intentional: the conservative allowlist IS the fail-closed
mechanism — anything we did not explicitly reason about runs the build.

Modes:
  --mode=diff --base <ref>   diff `<ref>...HEAD` for the changed-file set
  --mode=diff --comparison=trees --base <ref>
                             diff the exact `<ref>` and `HEAD` trees without
                             requiring their intervening history
  --mode=files <path> ...    classify an explicit file list (tests + CI)

Output:
  Writes `native_build_required=true|false` to $GITHUB_OUTPUT when set
  (GitHub Actions job output), and always prints a human-readable
  summary to stderr. `--json` prints a JSON object to stdout instead.

Exit code is always 0 unless invoked incorrectly (exit 2) — the
classification is data, not a pass/fail signal.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import subprocess
import sys
import tomllib
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
CHANGED_SURFACE_CONFIG = REPO_ROOT / ".shipyard" / "config.toml"

# Prefixes whose files never feed the C++/Swift build.
SKIP_SAFE_PREFIXES = (
    "docs/",
    "planning/",
    ".githooks/",
    ".shipyard/",
    ".shipyard.local/",
)

# Exact paths that never feed the build.
SKIP_SAFE_EXACT = {
    ".gitignore",
    ".gitattributes",
    "CODEOWNERS",
}

# Paths that LOOK skip-safe (e.g. a `.md` file under `docs/`) but are in
# fact native build inputs. Checked FIRST so they override every
# skip-safe rule below. `docs/migrations/*.md` is globbed with
# CONFIGURE_DEPENDS by tools/cli/CMakeLists.txt and compiled into pulp-cli
# as `migration_index.cpp` — editing one genuinely changes compiled C++.
FORCE_BUILD_PREFIXES = (
    "docs/migrations/",
)


def is_skip_safe(path: str) -> bool:
    """True if this single file provably does not affect the native build."""
    if not path:
        return False
    # Deny-list wins over every skip-safe rule: some docs feed codegen.
    if any(path.startswith(prefix) for prefix in FORCE_BUILD_PREFIXES):
        return False
    # Markdown anywhere — docs, never compiled, never embedded.
    if path.endswith(".md"):
        return True
    if path in SKIP_SAFE_EXACT:
        return True
    return any(path.startswith(prefix) for prefix in SKIP_SAFE_PREFIXES)


def native_build_required(files: list[str]) -> bool:
    """Fail-closed: native build required unless EVERY file is skip-safe.

    Empty input -> True (we could not determine the diff; build to be safe).
    """
    if not files:
        return True
    return not all(is_skip_safe(f) for f in files)


def _matches(path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def _mobile_skip_authorized(policy: dict, path: str) -> bool:
    """Accept only paths with independent, explicit mobile-skip review.

    Changed-surface ``families`` authorize bounded macOS tests, not omission
    of a mobile compile. Keeping a separate allowlist prevents a future family
    expansion from silently acquiring this second authorization meaning.
    """
    if path == ".shipyard/config.toml" or _matches(path, policy["policy_paths"]):
        return False
    if _matches(path, policy["test_topology_paths"]):
        return False
    if _matches(path, policy["full_required_paths"]):
        return False
    return _matches(path, policy["ios_compile_skip_safe_paths"])


def ios_compile_required(
    files: list[str], *, config_path: Path = CHANGED_SURFACE_CONFIG
) -> bool:
    """Fail closed unless every path has reviewed non-mobile authorization."""
    if not files:
        return True
    try:
        with config_path.open("rb") as config_file:
            policy = tomllib.load(config_file)["targets"]["mac"][
                "changed_surface_selection"
            ]
        return not all(_mobile_skip_authorized(policy, path) for path in files)
    except (OSError, KeyError, TypeError, tomllib.TOMLDecodeError):
        return True


def _changed_files_from_diff(
    base: str, *, comparison: str = "merge-base"
) -> list[str] | None:
    """Return changed files for the requested exact comparison.

    ``merge-base`` preserves the historical local-caller contract. ``trees``
    compares the two exact commit trees and is appropriate when CI already
    received an immutable event-pinned base SHA. It lets a shallow checkout
    remain shallow instead of downloading the repository's full history merely
    to rediscover a base GitHub already supplied.
    """
    # `--no-renames`: with rename detection on, a rename such as
    # `core/x.cpp -> docs/x.md` collapses to only the new path and would
    # wrongly classify skip-safe. Disabling it reports the rename as a
    # delete of the old path + an add of the new — so `core/x.cpp`
    # surfaces and forces the native build (fail-closed).
    separator = "..." if comparison == "merge-base" else ".."
    proc = subprocess.run(
        ["git", "diff", "--no-renames", "--name-only", f"{base}{separator}HEAD"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(
            f"[classify] git diff failed (exit {proc.returncode}): "
            f"{(proc.stderr or '').strip()[:200]}\n"
        )
        return None
    return [line.strip() for line in proc.stdout.splitlines() if line.strip()]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--mode", choices=("diff", "files"), default="diff")
    parser.add_argument(
        "--base",
        default="origin/main",
        help="for --mode=diff, the base ref (default origin/main)",
    )
    parser.add_argument(
        "--comparison",
        choices=("merge-base", "trees"),
        default="merge-base",
        help="for --mode=diff, compare from a merge base or two exact trees",
    )
    parser.add_argument(
        "--json", action="store_true", help="print a JSON object to stdout"
    )
    parser.add_argument("files", nargs="*", help="for --mode=files, the file list")
    args = parser.parse_args(argv)

    if args.mode == "files":
        files: list[str] | None = args.files
    else:
        files = _changed_files_from_diff(args.base, comparison=args.comparison)

    # Fail-closed: a None (git error) is treated as "unknown" -> build.
    resolved_files = files or []
    mobile_required = ios_compile_required(resolved_files)
    if files is None:
        required = True
        files = []
        reason = "git diff failed — defaulting to native build (fail-closed)"
    elif not files:
        required = True
        reason = "no changed files resolved — defaulting to native build (fail-closed)"
    else:
        required = native_build_required(files)
        non_safe = [f for f in files if not is_skip_safe(f)]
        if required:
            shown = ", ".join(non_safe[:8])
            extra = f" (+{len(non_safe) - 8} more)" if len(non_safe) > 8 else ""
            reason = f"native build inputs changed: {shown}{extra}"
        else:
            reason = f"all {len(files)} changed file(s) are skip-safe (docs/config only)"

    sys.stderr.write(
        f"[classify] native_build_required={str(required).lower()} — {reason}\n"
    )

    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as fh:
            fh.write(f"native_build_required={str(required).lower()}\n")
            fh.write(f"ios_compile_required={str(mobile_required).lower()}\n")

    if args.json:
        print(
            json.dumps(
                {
                    "native_build_required": required,
                    "ios_compile_required": mobile_required,
                    "changed_file_count": len(files),
                    "reason": reason,
                }
            )
        )

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
