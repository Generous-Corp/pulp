#!/usr/bin/env python3
"""Enforce the dependency direction between Pulp core and Vellum.

Pulp core knows nothing about Vellum; integration packages connect the two.
Two halves, both lexical, so the check needs no configured build:

* includes -- no source under core/ includes a ``vellum/`` header in any
  spelling the compiler accepts (include_boundary.py, boundary ``core-vellum``);
* links -- no CMake file under core/ finds, fetches, adds or links Vellum:
  ``find_package(Vellum``, ``FetchContent_Declare(vellum``,
  ``add_subdirectory(.../vellum``, or a ``Vellum::`` / ``vellum::`` target name.
  CMake comments are ignored, so prose that mentions Vellum stays legal.

Known edges are allowlisted with a reason and an exact occurrence count. An
entry that matches more often than allowed fails (a new reference hid behind an
old one); an entry that matches less often is stale and fails too, so a removed
edge leaves no exemption behind.

Exit codes: 0 clean, 1 violation, 2 scan error (including a scan that found no
CMake files, which means the instrument looked in the wrong place).
"""
from __future__ import annotations

import argparse
import collections
import contextlib
import io
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import include_boundary  # noqa: E402

CMAKE_ROOT = "core"
CMAKE_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("find_package(Vellum", re.compile(r"\bfind_package\s*\(\s*vellum\b", re.I)),
    ("FetchContent(vellum", re.compile(
        r"\bFetchContent_(?:Declare|MakeAvailable|Populate|GetProperties)\s*\(\s*vellum\b",
        re.I)),
    ("ExternalProject_Add(vellum", re.compile(r"\bExternalProject_Add\s*\(\s*vellum\b", re.I)),
    ("add_subdirectory(vellum", re.compile(
        r"\badd_subdirectory\s*\([^)]*\bvellum\b", re.I)),
)
TARGET_RE = re.compile(r"\b[Vv]ellum::[A-Za-z0-9_]+")

# (file, reference) -> (count, reason). The opt-in D15 GPU-audio provider
# imports Vellum's installed GPU runtime and Dawn headers. The block is removed
# when the shared GPU runtime (gpu-runtime) lands and owns the Dawn device; the
# matching include edge is allowlisted in include_boundary.py.
_D15 = "D15 provider; removed when the shared GPU runtime lands"
ALLOWED_CMAKE: dict[tuple[str, str], tuple[int, str]] = {
    ("core/gpu_audio/CMakeLists.txt", "find_package(Vellum"): (1, _D15),
    ("core/gpu_audio/CMakeLists.txt", "Vellum::Gpu"): (10, _D15),
    ("core/gpu_audio/CMakeLists.txt", "Vellum::DawnHeaders"): (4, _D15),
}


def strip_cmake_comment(line: str) -> str:
    """The line without its ``#`` comment; a ``#`` inside a quoted string stays."""
    in_quote = False
    escaped = False
    for i, ch in enumerate(line):
        if escaped:
            escaped = False
        elif ch == "\\":
            escaped = True
        elif ch == '"':
            in_quote = not in_quote
        elif ch == "#" and not in_quote:
            return line[:i]
    return line


def cmake_references(path: pathlib.Path) -> list[tuple[int, str]]:
    """(line, reference) for every Vellum dependency a CMake file names."""
    found: list[tuple[int, str]] = []
    text = path.read_text(encoding="utf-8", errors="replace")
    # Bracket comments (#[[ ... ]]) are prose too.
    text = re.sub(r"#\[(=*)\[.*?\]\1\]",
                  lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    for n, raw in enumerate(text.splitlines(), 1):
        line = strip_cmake_comment(raw)
        for label, pattern in CMAKE_PATTERNS:
            if pattern.search(line):
                found.append((n, label))
        found.extend((n, m.group(0)) for m in TARGET_RE.finditer(line))
    return found


def cmake_files(root: pathlib.Path) -> list[pathlib.Path]:
    base = root / CMAKE_ROOT
    if not base.is_dir():
        return []
    return [p for p in sorted(base.rglob("*"))
            if p.is_file() and (p.name == "CMakeLists.txt" or p.suffix == ".cmake")]


def check_cmake(root: pathlib.Path,
                allowed: dict[tuple[str, str], tuple[int, str]]) -> int:
    files = cmake_files(root)
    if not files:
        print(f"core-vellum-link-boundary: no CMake files under {CMAKE_ROOT}/ in {root}; "
              "refusing to report a clean tree", file=sys.stderr)
        return 2
    hits: dict[tuple[str, str], list[int]] = collections.defaultdict(list)
    for f in files:
        rel = f.relative_to(root).as_posix()
        try:
            for n, ref in cmake_references(f):
                hits[(rel, ref)].append(n)
        except OSError as exc:
            print(f"core-vellum-link-boundary: cannot read {rel}: {exc}", file=sys.stderr)
            return 2

    failures: list[str] = []
    for key, lines in sorted(hits.items()):
        rel, ref = key
        limit = allowed.get(key, (0, ""))[0]
        if len(lines) > limit:
            where = ", ".join(str(n) for n in lines)
            failures.append(f"{rel}:{where}: names Vellum ('{ref}', {len(lines)} "
                            f"occurrence(s), {limit} allowlisted)")
    for key, (count, _reason) in sorted(allowed.items()):
        seen = len(hits.get(key, []))
        if seen < count:
            failures.append(f"{key[0]}: allowlisted '{key[1]}' occurs {seen} time(s), "
                            f"not {count}; shrink or remove the ALLOWED_CMAKE entry")
    if failures:
        print("core-vellum-link-boundary: no CMake file under core/ may find, fetch, "
              "add or link Vellum\n")
        for failure in failures:
            print(f"  {failure}")
        return 1
    allowlisted = sum(len(v) for k, v in hits.items() if k in allowed)
    print(f"core-vellum-link-boundary: ok ({len(files)} CMake files, "
          f"{allowlisted} allowlisted reference(s))")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args(argv)
    root = args.root.resolve()
    boundary = include_boundary.BOUNDARIES["core-vellum"]
    include_rc = include_boundary.run(boundary, root, include_boundary.ALLOWED["core-vellum"])
    link_rc = check_cmake(root, ALLOWED_CMAKE)
    if 2 in (include_rc, link_rc):
        return 2
    return max(include_rc, link_rc)


if __name__ == "__main__":
    raise SystemExit(main())
