#!/usr/bin/env python3
"""Predict the configure failures a checkout is already destined for.

CMake reports these accurately and late, naming the symptom rather than the
cause, and every one of them costs a full build-and-diagnose cycle:

  * `Pulp macOS archive-floor mismatch` names the archive, not the fact that the
    Skia cache is stale relative to the manifest pin — nor which cache was used.
  * The source-contracts gate fails on missing files when the `planning`
    submodule is simply uninitialized, which reads as a contract violation.

The Skia case is the one worth the script on its own. `FindSkia.cmake` reads
`$SKIA_DIR` **before** falling back to the checkout's own `external/skia-build`,
and on a multi-worktree host that variable is often pinned in a shell rc. A
worktree can therefore hold a perfectly good cache and still build against a
different, broken one, with nothing in the configure output naming the override
as the reason. Measured on this fleet on 2026-08-16: worktrees carrying m151 and
m152 caches were all building against a stale m150 tree whose Dawn slice was
compiled at macOS 15.0 against a 13.4 floor, so every GPU-path configure died in
seconds. Both obvious remedies — "set SKIA_DIR per worktree", "fetch a good cache
locally" — cannot take effect, because the env pin wins.

So this prints WHICH Skia will be used and WHY, not just whether one exists.

Exit codes: 0 = clear, 1 = a problem that will fail configure, 2 = usage error.
Advisory notes do not fail the run.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]


def _set_root(root: pathlib.Path) -> None:
    """Point the checks at another checkout (testing, or auditing a peer tree)."""
    global REPO_ROOT, MIN_OS_JSON, MANIFEST_JSON
    REPO_ROOT = root.resolve()
    MIN_OS_JSON = REPO_ROOT / "tools" / "deps" / "min_os.json"
    MANIFEST_JSON = REPO_ROOT / "tools" / "deps" / "manifest.json"


MIN_OS_JSON = REPO_ROOT / "tools" / "deps" / "min_os.json"
MANIFEST_JSON = REPO_ROOT / "tools" / "deps" / "manifest.json"
STAMP_NAME = ".skia-asset-sha256"

MAC_LIB_RELPATH = pathlib.Path("build") / "mac-gpu" / "lib" / "Release"
# Dawn is the slice that has actually drifted above the floor in practice; skia
# is checked too so a half-updated cache cannot pass by naming only one.
FLOOR_CRITICAL_LIBS = ("libskia.a", "libdawn_combined.a")


class Report:
    def __init__(self) -> None:
        self.problems: list[tuple[str, str]] = []
        self.notes: list[str] = []
        self.facts: list[str] = []

    def problem(self, what: str, fix: str) -> None:
        self.problems.append((what, fix))

    def note(self, text: str) -> None:
        self.notes.append(text)

    def fact(self, text: str) -> None:
        self.facts.append(text)

    def render(self) -> int:
        for line in self.facts:
            print(f"preflight: {line}")
        for line in self.notes:
            print(f"preflight: note: {line}")
        if not self.problems:
            print("preflight: no configure-blocking problems found")
            return 0
        # Facts explain the problems below, so flush them first: interleaved
        # stdout/stderr buffering otherwise prints the verdict before its evidence.
        sys.stdout.flush()
        print("", file=sys.stderr)
        for what, fix in self.problems:
            print(f"preflight: PROBLEM: {what}", file=sys.stderr)
            print(f"preflight:     fix: {fix}", file=sys.stderr)
        return 1


def _version_tuple(text: str) -> tuple[int, ...]:
    return tuple(int(part) for part in re.findall(r"\d+", text))


def macos_floor() -> str | None:
    """The deployment target CMake will pin, from the same file CMake reads."""
    try:
        data = json.loads(MIN_OS_JSON.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    platforms = data.get("platforms")
    if not isinstance(platforms, dict):
        return None
    entry = platforms.get("macos-arm64")
    if isinstance(entry, dict):
        floor = entry.get("floor")
        if isinstance(floor, str):
            return floor
    return None


def manifest_skia_milestone() -> str | None:
    try:
        text = MANIFEST_JSON.read_text()
    except OSError:
        return None
    found = re.findall(r"chrome/m\d+", text)
    if not found:
        return None
    # The manifest names the milestone many times; they are expected to agree,
    # and a disagreement is itself worth surfacing rather than silently picking.
    unique = sorted(set(found))
    return unique[0] if len(unique) == 1 else "/".join(unique)


def archive_floor(path: pathlib.Path) -> str | None:
    """`minos` from the archive's LC_BUILD_VERSION, or None if unreadable."""
    try:
        out = subprocess.run(
            ["otool", "-l", str(path)], text=True, capture_output=True, timeout=120
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    match = re.search(r"^\s*minos\s+([0-9]+(?:\.[0-9]+)+)", out.stdout, re.M)
    return match.group(1) if match else None


def resolve_skia(report: Report) -> pathlib.Path | None:
    """Mirror FindSkia.cmake's precedence, and say which branch won.

    Reporting the source is the point. A worktree holding a good cache while an
    inherited env var silently redirects the build elsewhere is invisible in
    every other output, and it is the failure that wasted a night.
    """
    env_dir = os.environ.get("SKIA_DIR")
    local_dir = REPO_ROOT / "external" / "skia-build"
    if env_dir:
        resolved = pathlib.Path(env_dir).expanduser()
        report.fact(f"Skia source: $SKIA_DIR -> {resolved}")
        if resolved.resolve() != local_dir.resolve():
            report.fact(
                f"  NOTE: this OVERRIDES this checkout's own {local_dir}. "
                "FindSkia reads $SKIA_DIR first, so a good local cache here "
                "would be ignored."
            )
        return resolved
    if local_dir.exists():
        report.fact(f"Skia source: this checkout -> {local_dir} ($SKIA_DIR unset)")
        return local_dir
    report.fact("Skia source: none found ($SKIA_DIR unset, no external/skia-build)")
    return None


def check_skia(report: Report) -> None:
    if platform.system() != "Darwin":
        report.note("Skia archive-floor check runs on macOS only; skipped")
        return
    skia_dir = resolve_skia(report)
    if skia_dir is None:
        report.problem(
            "no Skia cache resolves for this checkout; GPU/examples configure "
            "will fail the PULP_HAS_SKIA gate",
            "python3 tools/scripts/fetch_skia_for_release.py darwin-arm64",
        )
        return
    if not skia_dir.is_dir():
        report.problem(
            f"$SKIA_DIR points at {skia_dir}, which does not exist",
            "unset SKIA_DIR, or point it at a populated skia-build directory",
        )
        return

    lib_dir = skia_dir / MAC_LIB_RELPATH
    missing = [name for name in FLOOR_CRITICAL_LIBS if not (lib_dir / name).is_file()]
    if missing:
        report.problem(
            f"{skia_dir} is present but not populated (missing {', '.join(missing)}); "
            "a fresh worktree carries headers only",
            f"python3 tools/scripts/fetch_skia_for_release.py darwin-arm64 --dest {skia_dir}",
        )
        return

    floor = macos_floor()
    if floor is None:
        report.note("could not read the deployment floor from tools/deps/min_os.json")
    else:
        report.fact(f"deployment floor: macOS {floor} (tools/deps/min_os.json)")
        for name in FLOOR_CRITICAL_LIBS:
            measured = archive_floor(lib_dir / name)
            if measured is None:
                report.note(f"{name}: no LC_BUILD_VERSION readable; floor unchecked")
                continue
            if _version_tuple(measured) > _version_tuple(floor):
                report.problem(
                    f"{name} is built for macOS {measured}, above this build's "
                    f"{floor} floor — configure WILL fail in "
                    "PulpMacosArchiveFloor.cmake. Raising the compile flag cannot "
                    "lower an archive's embedded LC_BUILD_VERSION; the cache is "
                    "the thing that is wrong.",
                    "refresh the cache the build actually uses: python3 "
                    "tools/scripts/fetch_skia_for_release.py darwin-arm64 "
                    f"--dest {skia_dir}",
                )
            else:
                report.fact(f"  {name}: macOS {measured} <= {floor} ok")

    pinned = manifest_skia_milestone()
    stamp_path = skia_dir / STAMP_NAME
    if pinned:
        report.fact(f"manifest pin: {pinned}")
    if stamp_path.is_file() and pinned:
        # The stamp is the reliable oracle. VERSION.md is a tracked doc that can
        # legitimately disagree with the bytes on a branch that has not caught up,
        # so it is deliberately not consulted here.
        report.note(
            "cache identity is stamped in .skia-asset-sha256; VERSION.md can "
            "disagree with the bytes on a stale branch and is not authoritative"
        )


def check_freshness(report: Report) -> None:
    """Is this checkout a trustworthy place to READ a pinned value from?

    Added after making the same mistake twice in one session, hours apart: I
    read `tools/deps/manifest.json` and then `tools/shipyard.toml` out of the
    primary checkout, reported both values confidently, and both were wrong —
    that tree sits on a stale branch with an unfinished merge. Once I nearly
    overrode a peer's correct diagnosis with it; once I told the user a
    dependency pin was 26 versions behind when it was two.

    Nothing in `grep`'s output names the branch it answered from, and the primary
    checkout is the most natural place to look on a host with many worktrees. So
    the file is not the instrument here — the checkout is, and it does not
    announce its own staleness.
    """
    def git(*args: str) -> str | None:
        try:
            out = subprocess.run(
                ["git", *args], cwd=REPO_ROOT, text=True, capture_output=True, timeout=60
            )
        except (OSError, subprocess.SubprocessError):
            return None
        return out.stdout.strip() if out.returncode == 0 else None

    if git("rev-parse", "--is-inside-work-tree") != "true":
        return

    # Check the merge state FIRST and unconditionally. An unfinished merge makes
    # a tree untrustworthy to read whether or not origin/main is resolvable, and
    # gating this behind that lookup would stay silent on exactly the trees most
    # likely to be mid-surgery.
    unmerged = git("diff", "--name-only", "--diff-filter=U") or ""
    behind = git("rev-list", "--count", "HEAD..origin/main")
    count = int(behind) if behind and behind.isdigit() else None
    if unmerged.strip():
        distance = (
            f" and is {count} commits behind origin/main" if count else ""
        )
        report.problem(
            f"this checkout has an UNFINISHED MERGE ({len(unmerged.split())} "
            f"conflicted paths){distance}. Values read from it — dependency "
            "pins, version files, manifests — may not be what the repo "
            "actually declares.",
            "read pinned values with `git show origin/main:<path>` instead, or "
            "resolve the merge before trusting this tree",
        )
        return
    if count is None:
        report.note("no origin/main to compare against; checkout freshness unknown")
        return
    if count >= 200:
        report.note(
            f"this checkout is {count} commits behind origin/main; prefer "
            "`git show origin/main:<path>` when reading a pinned value from it"
        )
    else:
        report.fact(f"checkout freshness: {count} commits behind origin/main")


def check_planning(report: Report) -> None:
    """An uninitialized submodule reads as a contract violation downstream."""
    planning = REPO_ROOT / "planning"
    gitfile = planning / ".git"
    if not planning.exists():
        report.note("no planning/ entry in this checkout; nothing to verify")
        return
    populated = gitfile.exists() and any(
        child.name != ".git" for child in planning.iterdir()
    )
    if populated:
        report.fact("planning submodule: initialized")
        return
    report.problem(
        "the planning submodule is not initialized; the source-contracts gate "
        "fails on missing files and reports it as a contract violation rather "
        "than a provisioning gap",
        "git submodule update --init planning",
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--skip-skia", action="store_true", help="Skip the Skia cache checks."
    )
    parser.add_argument(
        "--root", type=pathlib.Path,
        help="Check another checkout instead of this script's own.",
    )
    args = parser.parse_args(argv[1:])
    if args.root is not None:
        if not args.root.is_dir():
            print(f"preflight: {args.root} is not a directory", file=sys.stderr)
            return 2
        _set_root(args.root)

    report = Report()
    report.fact(f"checkout: {REPO_ROOT}")
    if not args.skip_skia:
        check_skia(report)
    check_freshness(report)
    check_planning(report)
    return report.render()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
