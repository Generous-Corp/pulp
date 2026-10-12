#!/usr/bin/env python3
"""include_boundary.py — lexical include-edge checks for Pulp layer boundaries.

One parser, several boundaries. Each boundary names the source roots it owns,
the ``pulp/<area>`` header areas it may (or may not) include, the include
directories its sources compile against, and any subtrees inside its roots
that belong to a different target. A source violates its boundary if it has an
include edge, in any spelling the compiler accepts, that leaves it:

* ``#include <pulp/<area>/...>``, ``#include "pulp/<area>/..."`` and the same
  with ``#import`` (Objective-C++), where ``<area>`` is outside the boundary;
* a ``pulp/...`` header that exists only in an excluded subtree's include
  directory (the compiler would reach it only through that target);
* a relative include (``"../x.hpp"``) that resolves, from the including file
  and then from the boundary's include directories as the compiler searches,
  outside the roots or into an excluded subtree, or does not resolve at all.

The scan needs no configured build. Known edges tracked for removal are
allowlisted per boundary with a reason; an allowlist entry that no longer
matches anything is itself a failure, so a removed edge leaves no exemption.

Exit codes: 0 = clean, 1 = violation found, 2 = scan error (including a scan
that found no files, which means the instrument looked in the wrong place).
"""
from __future__ import annotations

import argparse
import dataclasses
import pathlib
import re
import sys

SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl",
            ".ipp", ".m", ".mm"}
INCLUDE_RE = re.compile(r'^\s*#\s*(?:include_next|include|import)\s*([<"])([^>"]+)[>"]')


@dataclasses.dataclass(frozen=True)
class Boundary:
    name: str
    summary: str
    roots: tuple[str, ...]
    include_dirs: tuple[str, ...]
    allowed_areas: frozenset[str] | None = None
    forbidden_areas: frozenset[str] = frozenset()
    excluded: tuple[str, ...] = ()


BOUNDARIES: dict[str, Boundary] = {
    "foundation": Boundary(
        name="foundation",
        summary="core/platform, core/runtime and core/simd may include only foundation headers",
        roots=("core/platform", "core/runtime", "core/simd"),
        include_dirs=("core/platform/include", "core/runtime/include", "core/simd/include"),
        allowed_areas=frozenset({"platform", "runtime", "simd", "foundation"}),
    ),
    "view-core": Boundary(
        name="view-core",
        summary=("core/view outside core/view/audio may not include audio, signal, host, "
                 "midi or format headers, nor the audio widgets' headers"),
        roots=("core/view",),
        include_dirs=("core/view/include",),
        forbidden_areas=frozenset({"audio", "signal", "host", "midi", "format"}),
        excluded=("core/view/audio",),
    ),
}

# boundary name -> {(file, include): reason}. Every entry must still match.
ALLOWED: dict[str, dict[tuple[str, str], str]] = {name: {} for name in BOUNDARIES}


def _inside(path: pathlib.Path, dirs: list[pathlib.Path]) -> bool:
    return any(path == d or d in path.parents for d in dirs)


def include_edges(path: pathlib.Path) -> list[tuple[int, str, str]]:
    """(line, delimiter, target) for every include/import directive in a file."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    edges = []
    for n, line in enumerate(lines, 1):
        m = INCLUDE_RE.match(line)
        if m:
            edges.append((n, m.group(1), m.group(2)))
    return edges


def crossings(path: pathlib.Path, root: pathlib.Path, b: Boundary) -> list[tuple[int, str]]:
    """(line, include) for every edge of ``path`` that leaves boundary ``b``."""
    roots = [(root / r).resolve() for r in b.roots]
    excluded = [(root / e).resolve() for e in b.excluded]
    include_dirs = [(root / d).resolve() for d in b.include_dirs]
    found: list[tuple[int, str]] = []
    for n, delim, target in include_edges(path):
        parts = target.split("/")
        if len(parts) >= 2 and parts[0] == "pulp":
            area = parts[1]
            if b.allowed_areas is not None and area not in b.allowed_areas:
                found.append((n, target))
            elif area in b.forbidden_areas:
                found.append((n, target))
            elif excluded and not any((d / target).exists() for d in include_dirs) \
                    and any((e / "include" / target).exists() for e in excluded):
                found.append((n, target))
            continue
        if ".." in parts:
            # Mirror the compiler: the including file's directory first (for a
            # quoted include), then the include directories. An include that
            # resolves nowhere cannot be proven to stay inside, so it crosses.
            bases = ([path.parent] if delim == '"' else []) + include_dirs
            resolved = next((c for c in ((base / target).resolve() for base in bases)
                             if c.exists()), None)
            if resolved is None or not _inside(resolved, roots) or _inside(resolved, excluded):
                found.append((n, target))
    return found


def sources(root: pathlib.Path, b: Boundary) -> list[pathlib.Path]:
    excluded = [(root / e).resolve() for e in b.excluded]
    files: list[pathlib.Path] = []
    for r in b.roots:
        base = root / r
        if base.is_dir():
            files.extend(p for p in sorted(base.rglob("*"))
                         if p.is_file() and p.suffix in SUFFIXES
                         and not _inside(p.resolve(), excluded))
    return files


def run(b: Boundary, root: pathlib.Path, allowed: dict[tuple[str, str], str]) -> int:
    root = root.resolve()
    files = sources(root, b)
    if not files:
        print(f"{b.name}-include-boundary: no sources found under {', '.join(b.roots)} "
              f"in {root}; refusing to report a clean tree", file=sys.stderr)
        return 2
    hits: list[tuple[str, int, str]] = []
    for f in files:
        rel = f.relative_to(root).as_posix()
        try:
            hits.extend((rel, n, target) for n, target in crossings(f, root, b))
        except OSError as exc:
            print(f"{b.name}-include-boundary: cannot read {rel}: {exc}", file=sys.stderr)
            return 2

    used: set[tuple[str, str]] = set()
    violations = []
    for rel, n, target in hits:
        key = (rel, target)
        if key in allowed:
            used.add(key)
        else:
            violations.append(f"{rel}:{n}: includes '{target}' from outside the boundary")
    stale = [f"{rel}: allowlisted include '{target}' no longer present; remove the ALLOWED entry"
             for rel, target in sorted(set(allowed) - used)]
    if violations or stale:
        print(f"{b.name}-include-boundary: {b.summary}\n")
        for v in violations + stale:
            print(f"  {v}")
        return 1
    print(f"{b.name}-include-boundary: ok ({len(files)} files, {len(used)} allowlisted edge(s))")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--boundary", choices=sorted(BOUNDARIES), required=True)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args(argv)
    return run(BOUNDARIES[args.boundary], args.root, ALLOWED[args.boundary])


if __name__ == "__main__":
    raise SystemExit(main())
