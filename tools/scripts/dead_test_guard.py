#!/usr/bin/env python3
"""Fail when a test source file is not referenced by any CMake file.

A ``.cpp`` file under ``test/`` or under a ``test`` or ``tests``
directory elsewhere in the tree that no CMake target lists is never compiled,
so it silently stops testing anything. This guard reads every tracked
``CMakeLists.txt`` and ``*.cmake`` file, ignores comments, and requires each
test source to appear in one of them:

* by a path relative to the CMake file's own directory or to the repository
  root (``${CMAKE_SOURCE_DIR}/test/foo.cpp`` and ``../test/foo.cpp`` count), or
* by a bare file name listed in a CMake file that sits in the same directory.

A glob pattern in a CMake file (``file(GLOB ...)``) counts as a reference to
every file it matches. Fixture inputs that are compiled at test time rather
than by a target are listed in ``tools/scripts/dead_test_guard.json`` under
``allow`` with a reason; an entry whose file is referenced or gone is stale and
also fails.

Usage:
    python3 tools/scripts/dead_test_guard.py [--repo-root PATH]
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

REPO_ROOT = Path(__file__).resolve().parents[2]
ALLOW_FILE = Path(__file__).with_name("dead_test_guard.json")
SOURCE_SUFFIXES = (".cpp",)
TEST_DIR_NAMES = ("test", "tests")
MIN_TEMPLATE_LITERAL = 8
# Entry points that pulp_add_plugin() and pulp_add_app() pick up from their own
# source directory by name (see tools/cmake/PulpPluginFormats.cmake and
# PulpAppTargets.cmake), so no CMake file spells them out.
CONVENTION_ENTRY = re.compile(r"(?:main|[a-z0-9_]+_entry|[a-z0-9_]+_bundle_entry)\.cpp")
CONVENTION_CALL = re.compile(r"\bpulp_add_(?:plugin|plugin_bundle|app)\s*\(", re.IGNORECASE)

_COMMENT = re.compile(r"(?m)(?<!\\)#.*$")
# A source path or a glob that ends in a source suffix, for example
# ``test_foo.cpp``, ``${CMAKE_SOURCE_DIR}/test/foo.cpp`` or ``fixtures/*.cpp``.
_TOKEN = re.compile(r"[\w$@{}./+\-*?\[\]]+\.cpp\b")
# Targets declared by a helper that derives the source from the target name:
# ``pulp_add_test_suite(pulp-test-foo-bar ...)`` compiles ``test_foo_bar.cpp``
# unless the call passes SOURCES.
_SUITE_CALL = re.compile(r"\bpulp_add_test_suite\s*\(\s*(pulp-test-[\w\-]+)([^)]*)\)", re.IGNORECASE)


def tracked_files(root: Path) -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True,
                         text=True, encoding="utf-8", check=True).stdout
    return [f for f in out.split("\0") if f]


def is_cmake(path: str) -> bool:
    name = PurePosixPath(path).name
    return name == "CMakeLists.txt" or name.endswith(".cmake")


def is_test_source(path: str) -> bool:
    """A ``.cpp`` under the top-level ``test/`` tree or a ``core/<module>/test(s)/`` tree.

    Other trees that carry their own ``test`` directory (a self-contained
    example, or a tree mirrored from another project) are built by their own
    project and are out of scope here.
    """
    p = PurePosixPath(path)
    if p.suffix not in SOURCE_SUFFIXES:
        return False
    parts = p.parts
    if parts[0] == "test":
        return True
    return parts[0] == "core" and len(parts) > 3 and parts[2] in TEST_DIR_NAMES


def _strip_variables(token: str) -> str:
    """Mark a leading ``${VAR}/`` with a ``$/`` prefix.

    ``${_slice_dir}/a.cpp`` becomes ``$/a.cpp``: the variable names some
    directory under the CMake file's tree that this scan cannot evaluate, so
    the remainder is matched as a path suffix instead of from a fixed base.
    """
    if re.match(r"^(?:\$\{[^}]*\}|@[^@]*@)/?", token):
        token = "$/" + re.sub(r"^(?:\$\{[^}]*\}|@[^@]*@)/?", "", token)
    return _glob_templated(token)


def _glob_templated(token: str) -> str:
    """Turn ``ui_package_${pkg}.cpp`` into ``ui_package_*.cpp``.

    Only when the literal text before the first variable is distinctive. A
    pattern such as ``test_*.cpp`` would otherwise vouch for every test.
    """
    m = re.search(r"\$\{[^}]*\}", token)
    if not m:
        return token
    literal = token[:m.start()].rsplit("/", 1)[-1]
    if len(literal) < MIN_TEMPLATE_LITERAL:
        return token
    return re.sub(r"\$\{[^}]*\}", "*", token)


def cmake_tokens(text: str) -> set[str]:
    text = _COMMENT.sub("", text)
    return {_strip_variables(t) for t in _TOKEN.findall(text)}


def derived_suite_sources(text: str) -> set[str]:
    """Source file names implied by suite calls that pass no SOURCES argument."""
    text = _COMMENT.sub("", text)
    out = set()
    for name, rest in _SUITE_CALL.findall(text):
        if re.search(r"\bSOURCES\b", rest):
            continue
        stem = name[len("pulp-test-"):].replace("-", "_")
        out.add(f"test_{stem}.cpp")
    return out


def build_index(root: Path, files: list[str]) -> tuple[list[tuple[str, set[str], str]], set[str]]:
    index = []
    derived: set[str] = set()
    for f in files:
        if is_cmake(f):
            try:
                text = (root / f).read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            index.append((str(PurePosixPath(f).parent), cmake_tokens(text), _COMMENT.sub("", text)))
            derived |= derived_suite_sources(text)
    return index, derived


def _norm(path: str) -> str:
    return str(PurePosixPath(path))


def _glob_match(pattern: str, path: str) -> bool:
    """CMake-style glob: ``*`` stays inside one path segment, ``**`` crosses them."""
    out, i = [], 0
    while i < len(pattern):
        c = pattern[i]
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
            continue
        if c == "*":
            out.append("[^/]*")
        elif c == "?":
            out.append("[^/]")
        else:
            out.append(re.escape(c))
        i += 1
    return re.fullmatch("".join(out), path) is not None


def _resolve(base: str, tok: str) -> str:
    """Join ``tok`` onto ``base`` and fold ``.`` and ``..`` segments."""
    parts: list[str] = []
    joined = tok if base in ("", ".") else f"{base}/{tok}"
    for seg in joined.split("/"):
        if seg == "..":
            if parts:
                parts.pop()
        elif seg not in ("", "."):
            parts.append(seg)
    return "/".join(parts)


def _ancestors(directory: str) -> list[str]:
    """The directory itself, then each parent up to the repository root."""
    out = []
    parts = [] if directory in ("", ".") else directory.split("/")
    while True:
        out.append("/".join(parts) or ".")
        if not parts:
            return out
        parts.pop()


def referenced(path: str, index: list[tuple[str, set[str], str]], derived: set[str]) -> bool:
    """True when some CMake file names ``path``.

    A CMake file can be included from a parent directory (``test/cmake/x.cmake``
    listing sources that live beside ``test/CMakeLists.txt``), so a token is
    resolved against the file's own directory and each directory above it.
    """
    if PurePosixPath(path).name in derived:
        return True
    p = PurePosixPath(path)
    if CONVENTION_ENTRY.fullmatch(p.name):
        for cmake_dir, _tokens, text in index:
            if cmake_dir == str(p.parent) and CONVENTION_CALL.search(text):
                return True
    for cmake_dir, tokens, text in index:
        bases = _ancestors(cmake_dir)
        for tok in tokens:
            if not tok:
                continue
            is_glob = any(ch in tok for ch in "*?[")
            if tok.startswith("$/"):
                tail = tok[2:]
                if not tail:
                    continue
                # ${CMAKE_CURRENT_SOURCE_DIR}/x.cpp and friends name the CMake
                # file's own directory or one above it.
                for base in bases:
                    resolved = _resolve(base, tail)
                    if (_glob_match(resolved, path) if is_glob else resolved == path):
                        return True
                # ${_slice_dir}/x.cpp: the variable holds a subdirectory this
                # scan cannot evaluate. Accept the path's end matching the tail
                # when the file's own directory is spelled out in the same CMake
                # file, so a bare "main.cpp" cannot vouch for every main.cpp.
                parent = PurePosixPath(path).parent
                matches_tail = (_glob_match("**/" + tail, path) if is_glob
                                else path.endswith("/" + tail))
                if matches_tail:
                    for base in bases:
                        if base == "." or not path.startswith(base + "/"):
                            continue
                        rel_dir = str(parent.relative_to(base))
                        if rel_dir != "." and (rel_dir in text or parent.name in text):
                            return True
                continue
            for base in bases:
                resolved = _resolve(base, tok)
                if (_glob_match(resolved, path) if is_glob else resolved == path):
                    return True
    return False


def find_dead(root: Path) -> tuple[list[str], list[str]]:
    files = tracked_files(root)
    index, derived = build_index(root, files)
    allow = {}
    if ALLOW_FILE.is_file():
        allow = {e["path"]: e["reason"] for e in json.loads(ALLOW_FILE.read_text(encoding="utf-8"))["allow"]}
    dead, stale = [], []
    seen = set()
    for f in files:
        if not is_test_source(f):
            continue
        seen.add(f)
        is_ref = referenced(f, index, derived)
        if f in allow:
            if is_ref:
                stale.append(f"{f}: allow-listed but a CMake file now references it; remove the entry")
            continue
        if not is_ref:
            dead.append(f)
    for p in allow:
        if p not in seen:
            stale.append(f"{p}: allow-listed but the file no longer exists; remove the entry")
    return sorted(dead), sorted(stale)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    args = ap.parse_args(argv)
    dead, stale = find_dead(args.repo_root.resolve())
    for f in dead:
        print(f"dead-test-guard: {f} is not referenced by any CMake target. "
              "Register it in a test target or delete it.", file=sys.stderr)
    for s in stale:
        print(f"dead-test-guard: {s}", file=sys.stderr)
    if dead or stale:
        return 1
    print("dead-test-guard: every test source is referenced by a CMake file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
