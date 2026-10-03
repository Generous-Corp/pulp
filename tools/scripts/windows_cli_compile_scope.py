#!/usr/bin/env python3
"""Decide whether a change can break the MSVC compile of the release CLI.

Used by .github/workflows/windows-cli-compile.yml. Every required build gate
compiles with Clang (macOS) and the PR-time Linux lanes use Clang or GCC, so
code inside `#ifdef _WIN32` is compiled by nothing until release-cli.yml's
Windows legs build a tag. A Windows-only compile error therefore passes every
pull-request check and surfaces only as a release whose Windows legs fail and
whose `release` job is skipped.

The workflow builds the CLI targets with MSVC only when this classifier says the
diff can reach them, so the hosted Windows runner is spent on a small fraction
of changes rather than on every pull request. A path is relevant when it is:

  * under tools/cli/ or tools/mcp/ (the CLI and MCP sources and their CMake);
  * under tools/cmake/ (build inputs every target is configured from);
  * this classifier or the workflow that runs it;
  * a C/C++ source outside test/, examples/ and external/ whose path names a
    Windows platform directory or file, or whose content carries a Windows or
    MSVC preprocessor marker; or
  * a CMakeLists.txt outside those trees whose content branches on WIN32/MSVC.

Deliberately NOT relevant: the root CMakeLists.txt (its VERSION line moves on
every release bump, and release-path-pr-gate.yml already builds it), and C/C++
files with no Windows marker (the Clang and GCC lanes compile those, so an error
there is caught without a Windows runner). This is a scope filter, not a proof:
MSVC can reject portable code that Clang and GCC accept, and that class reaches
this lane only when the file also carries a Windows marker.

Run:
    python3 tools/scripts/windows_cli_compile_scope.py --base <sha> --head <sha>
    python3 tools/scripts/windows_cli_compile_scope.py --paths a.cpp b.hpp

Exit status is 0 whether or not the change is relevant; the verdict is printed
and, under GitHub Actions, written to $GITHUB_OUTPUT as `relevant=true|false`.
Exit 3 means the diff could not be read; the verdict is then `relevant=true`
so the workflow compiles rather than skips. (Exit 2 stays argparse's usage error.)
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Callable, Iterable, Optional

ALWAYS_RELEVANT_PREFIXES = (
    "tools/cli/",
    "tools/mcp/",
    "tools/cmake/",
)

ALWAYS_RELEVANT_FILES = frozenset(
    {
        ".github/workflows/windows-cli-compile.yml",
        "tools/scripts/windows_cli_compile_scope.py",
    }
)

# Trees whose C/C++ is never part of the release CLI build.
EXCLUDED_PREFIXES = ("test/", "examples/", "external/", "planning/")

CXX_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}
)

# A path segment or file stem that names a Windows platform implementation.
WINDOWS_PATH = re.compile(
    r"(?:^|/)(?:win|win32|win64|windows)(?:/|$)"
    r"|(?:^|[/_-])(?:win|win32|windows)[_.-][^/]*$",
    re.IGNORECASE,
)

WINDOWS_SOURCE_MARKERS = re.compile(
    r"\b(?:_WIN32|_WIN64|_MSC_VER|WIN32_LEAN_AND_MEAN|NOMINMAX)\b"
)
WINDOWS_CMAKE_MARKERS = re.compile(r"\b(?:WIN32|MSVC|CMAKE_SYSTEM_NAME\s+STREQUAL\s+\"?Windows)\b")


def relevance(path: str, read_text: Callable[[str], Optional[str]]) -> Optional[str]:
    """Return why `path` can affect the MSVC CLI compile, or None if it cannot.

    `read_text` returns the file's content at the head revision, or None when the
    file does not exist there (a deletion).
    """
    if path in ALWAYS_RELEVANT_FILES:
        return "this lane's own workflow or classifier"
    if path.startswith(ALWAYS_RELEVANT_PREFIXES):
        return "CLI, MCP, or shared CMake input"
    if path.startswith(EXCLUDED_PREFIXES):
        return None

    name = path.rsplit("/", 1)[-1]
    suffix = Path(name).suffix.lower()

    if name == "CMakeLists.txt" and "/" in path:
        text = read_text(path)
        if text is not None and WINDOWS_CMAKE_MARKERS.search(text):
            return "CMakeLists.txt that branches on WIN32/MSVC"
        return None

    if suffix not in CXX_SUFFIXES:
        return None
    if WINDOWS_PATH.search(path):
        return "Windows platform source"
    text = read_text(path)
    if text is not None and WINDOWS_SOURCE_MARKERS.search(text):
        return "source with a Windows/MSVC preprocessor branch"
    return None


def classify(
    paths: Iterable[str], read_text: Callable[[str], Optional[str]]
) -> list[tuple[str, str]]:
    """Return (path, reason) for every relevant path, in input order."""
    hits: list[tuple[str, str]] = []
    for path in paths:
        reason = relevance(path, read_text)
        if reason:
            hits.append((path, reason))
    return hits


def _git(args: list[str], cwd: Path) -> str:
    return subprocess.run(
        ["git", *args], cwd=cwd, check=True, capture_output=True, text=True
    ).stdout


def changed_paths(base: str, head: str, cwd: Path, *, merge_base: bool) -> list[str]:
    spec = f"{base}...{head}" if merge_base else f"{base}..{head}"
    out = _git(["diff", "--name-only", "--no-renames", spec], cwd)
    return [line for line in out.splitlines() if line.strip()]


def git_reader(head: str, cwd: Path) -> Callable[[str], Optional[str]]:
    def read(path: str) -> Optional[str]:
        result = subprocess.run(
            ["git", "show", f"{head}:{path}"],
            cwd=cwd,
            capture_output=True,
            text=True,
            errors="replace",
        )
        return result.stdout if result.returncode == 0 else None

    return read


def _write_output(relevant: bool) -> None:
    target = os.environ.get("GITHUB_OUTPUT")
    if target:
        with open(target, "a", encoding="utf-8") as handle:
            handle.write(f"relevant={'true' if relevant else 'false'}\n")


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--base", help="base revision")
    parser.add_argument("--head", default="HEAD", help="head revision")
    parser.add_argument(
        "--two-dot",
        action="store_true",
        help="diff base..head (a push range) instead of the merge base",
    )
    parser.add_argument(
        "--paths", nargs="*", help="classify these working-tree paths instead"
    )
    args = parser.parse_args(argv)
    repo = Path(
        _git(["rev-parse", "--show-toplevel"], Path.cwd()).strip()
    ) if args.paths is None else Path.cwd()

    if args.paths is not None:
        paths = list(args.paths)

        def reader(path: str) -> Optional[str]:
            candidate = repo / path
            try:
                return candidate.read_text(encoding="utf-8", errors="replace")
            except OSError:
                return None
    else:
        if not args.base:
            parser.error("--base is required unless --paths is given")
        try:
            paths = changed_paths(
                args.base, args.head, repo, merge_base=not args.two_dot
            )
        except subprocess.CalledProcessError as error:
            print(
                f"could not diff {args.base} against {args.head}: "
                f"{(error.stderr or '').strip()}; treating the change as relevant",
                file=sys.stderr,
            )
            _write_output(True)
            return 3
        reader = git_reader(args.head, repo)

    hits = classify(paths, reader)
    print(f"{len(paths)} changed path(s); {len(hits)} can affect the MSVC CLI compile")
    for path, reason in hits:
        print(f"  {path}: {reason}")
    _write_output(bool(hits))
    print("relevant" if hits else "not relevant: no Windows CLI compile needed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
