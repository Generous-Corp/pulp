#!/usr/bin/env python3
"""Check that pulp-render compiles nothing that reaches above the render layer.

core/render holds two kinds of source. Most of it is the renderer: it includes
only render, canvas, runtime and scene headers. The Android app shell
(gpu_surface_android.cpp and its JNI bridge) lives under
core/render/platform/android but drives the view hierarchy, the widget bridge,
the state store and the demo synth, so it is compiled into the pulp-jni shared
library instead. A render-layer extraction (a target closure manifest) must
never pick up that shell.

Android sources only reach a target in an Android configure, so a desktop
configure's SOURCES property cannot see this edge. The check is therefore
static and covers every platform at once:

  1. every source under core/render outside the shell allowlist includes only
     pulp/{render,canvas,runtime,scene} headers and no path that climbs out of
     its own directory tree with `../`;
  2. no CMake file adds a shell source to pulp-render;
  3. every shell source is still compiled into pulp-jni, so the shell cannot
     silently disappear from the Android library.

Exit codes: 0 all rules hold, 1 a rule is violated, 2 the tree could not be
read (a missing directory or shell file is reported, never treated as clean).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

RENDER_DIR = "core/render"

# Sources under core/render that belong to the Android app shell, not to the
# renderer. They may include anything; they must never be part of pulp-render.
SHELL_SOURCES = (
    "core/render/platform/android/gpu_surface_android.cpp",
    "core/render/platform/android/gpu_surface_android_jni.cpp",
)
# Shell-private headers: excluded from the include rule, never compiled alone.
SHELL_HEADERS = (
    "core/render/platform/android/gpu_surface_android_internal.hpp",
    "core/render/platform/android/synth_ui.js.h",
)

ALLOWED_PULP_ROOTS = frozenset({"render", "canvas", "runtime", "scene"})
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".mm", ".m"}
CMAKE_GLOBS = ("CMakeLists.txt", "core/**/CMakeLists.txt", "tools/cmake/*.cmake")

INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s*([<"])([^>"]+)[>"]', re.MULTILINE)
CALL_RE = re.compile(r"\btarget_sources\s*\(\s*([A-Za-z0-9_:${}.-]+)", re.IGNORECASE)


def strip_cmake_comments(text: str) -> str:
    return re.sub(r"(?m)#(?!\[).*$", "", re.sub(r"#\[(=*)\[.*?\]\1\]", "", text, flags=re.DOTALL))


def target_sources_calls(text: str) -> list[tuple[str, str]]:
    """(target, argument text) for every target_sources() call."""
    calls = []
    for match in CALL_RE.finditer(text):
        depth, index = 1, match.end()
        while index < len(text) and depth:
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
            index += 1
        calls.append((match.group(1), text[match.end():index - 1]))
    return calls


def check(repo: Path) -> tuple[list[str], int]:
    """Return (violations, renderer sources scanned); raise OSError when unreadable."""
    render = repo / RENDER_DIR
    if not render.is_dir():
        raise OSError(f"{render} is not a directory")
    for rel in SHELL_SOURCES + SHELL_HEADERS:
        if not (repo / rel).is_file():
            raise OSError(f"shell file {rel} is missing; update this check if it moved")

    violations: list[str] = []
    exempt = set(SHELL_SOURCES + SHELL_HEADERS)
    scanned = 0
    for path in sorted(render.rglob("*")):
        rel = path.relative_to(repo).as_posix()
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file() or rel in exempt:
            continue
        scanned += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        for quote, include in INCLUDE_RE.findall(text):
            if include.startswith("pulp/"):
                root = include.split("/")[1]
                if root not in ALLOWED_PULP_ROOTS:
                    violations.append(
                        f"render-includes-upward: {rel} includes {include}; the renderer may "
                        f"include only pulp/{{{','.join(sorted(ALLOWED_PULP_ROOTS))}}}")
            elif quote == '"' and include.startswith("../"):
                target = (path.parent / include).resolve()
                if render.resolve() not in target.parents:
                    violations.append(
                        f"render-includes-outside: {rel} includes {include}, which leaves core/render")

    cmake_files: list[Path] = []
    for pattern in CMAKE_GLOBS:
        cmake_files.extend(sorted(repo.glob(pattern)))
    shell_names = {Path(rel).name for rel in SHELL_SOURCES}
    jni_sources: set[str] = set()
    for cmake in cmake_files:
        text = strip_cmake_comments(cmake.read_text(encoding="utf-8", errors="replace"))
        for target, arguments in target_sources_calls(text):
            named = {name for name in shell_names if name in arguments}
            if target in {"pulp-render", "pulp::render"} and named:
                violations.append(
                    f"shell-in-render: {cmake.relative_to(repo).as_posix()} adds "
                    f"{sorted(named)} to {target}; the Android shell belongs in pulp-jni")
            if target == "pulp-jni":
                jni_sources |= named
    for name in sorted(shell_names - jni_sources):
        violations.append(
            f"shell-not-in-jni: no CMake file adds {name} to pulp-jni, so libpulp.so would "
            "lose the Android shell")
    return violations, scanned


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    try:
        violations, scanned = check(args.repo)
    except OSError as error:
        print(f"render-upward-edge-check: INCONCLUSIVE: {error}", file=sys.stderr)
        return 2
    if violations:
        for violation in violations:
            print(f"render-upward-edge-check: FAIL {violation}", file=sys.stderr)
        return 1
    if scanned == 0:
        print("render-upward-edge-check: INCONCLUSIVE: no renderer sources found", file=sys.stderr)
        return 2
    print(f"render-upward-edge-check: PASS ({scanned} renderer sources, "
          f"{len(SHELL_SOURCES)} shell sources in pulp-jni)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
