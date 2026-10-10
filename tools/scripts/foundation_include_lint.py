#!/usr/bin/env python3
"""foundation_include_lint.py — keep the foundation layer free of upper layers.

The foundation layer is ``core/platform``, ``core/runtime`` and ``core/simd``
(aggregated as ``pulp::foundation``). Everything else in Pulp builds on it, so
it must never include headers from a higher subsystem: an audio, view or
format include here drags that subsystem into every consumer of the
foundation, including audio-free UI code.

The scan is lexical over every C/C++/Objective-C source and header under the
three roots, so it needs no configured build and also covers files that are
compiled into other targets (the Android JNI sources under
``core/platform/src/android`` are linked into ``libpulp.so``, not into
``pulp-platform``, yet they still ship as part of the foundation tree).

A file violates the boundary if it includes

* ``<pulp/<area>/...>`` or ``"pulp/<area>/..."`` where ``<area>`` is not one of
  the foundation areas, or
* a relative path that resolves (from the including file, then from the
  foundation include directories, as the compiler would) outside the three
  foundation roots, or does not resolve at all.

Known edges that are tracked for removal are listed in ``ALLOWED`` with their
reason. An allowlist entry that no longer matches anything is itself a failure,
so a removed edge cannot leave a stale exemption behind.

Exit codes: 0 = clean, 1 = violation found, 2 = scan error (including a scan
that found no files, which means the instrument looked in the wrong place).
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOTS = ("core/platform", "core/runtime", "core/simd")
FOUNDATION_AREAS = {"platform", "runtime", "simd", "foundation"}
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl",
            ".ipp", ".m", ".mm"}
INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s*([<"])([^>"]+)[>"]')

# (file, include) -> reason. Every entry must still match, see module doc.
ALLOWED: dict[tuple[str, str], str] = {
    ("core/platform/src/android/jni_motion.cpp", "pulp/view/motion.hpp"):
        "Android motion JNI shim; compiled into pulp-jni, not pulp-platform. "
        "Moves next to the view sources with the view-core carve.",
}


def scan_file(path: pathlib.Path, rel: str, root: pathlib.Path) -> list[tuple[str, int, str]]:
    """Return (rel, line, include) for every boundary-crossing include."""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        raise RuntimeError(f"cannot read {rel}: {exc}") from exc
    found: list[tuple[str, int, str]] = []
    foundation_dirs = [(root / r).resolve() for r in ROOTS]
    for n, line in enumerate(lines, 1):
        m = INCLUDE_RE.match(line)
        if not m:
            continue
        delim, target = m.groups()
        parts = target.split("/")
        if len(parts) >= 2 and parts[0] == "pulp":
            if parts[1] not in FOUNDATION_AREAS:
                found.append((rel, n, target))
            continue
        if delim == '"' and (target.startswith("../") or "/../" in target):
            # Mirror the compiler: the including file's directory first, then
            # the foundation include directories. An include that resolves
            # nowhere cannot be proven to stay inside, so it counts as a crossing.
            bases = [path.parent] + [d / "include" for d in foundation_dirs]
            resolved = next((c for c in ((b / target).resolve() for b in bases) if c.exists()), None)
            if resolved is None or not any(d in resolved.parents for d in foundation_dirs):
                found.append((rel, n, target))
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args(argv)
    root = args.root.resolve()

    files: list[pathlib.Path] = []
    for r in ROOTS:
        base = root / r
        if base.is_dir():
            files.extend(p for p in sorted(base.rglob("*"))
                         if p.is_file() and p.suffix in SUFFIXES)
    if not files:
        print(f"foundation-include-lint: no sources found under {', '.join(ROOTS)} "
              f"in {root}; refusing to report a clean tree", file=sys.stderr)
        return 2

    hits: list[tuple[str, int, str]] = []
    try:
        for f in files:
            hits.extend(scan_file(f, f.relative_to(root).as_posix(), root))
    except RuntimeError as exc:
        print(f"foundation-include-lint: {exc}", file=sys.stderr)
        return 2

    used: set[tuple[str, str]] = set()
    violations = []
    for rel, n, target in hits:
        key = (rel, target)
        if key in ALLOWED:
            used.add(key)
        else:
            violations.append(f"{rel}:{n}: includes '{target}' from outside the foundation layer")
    stale = [f"{rel}: allowlisted include '{target}' no longer present; remove the ALLOWED entry"
             for rel, target in sorted(set(ALLOWED) - used)]

    if violations or stale:
        print("foundation-include-lint: core/platform, core/runtime and core/simd "
              "may include only foundation headers\n")
        for v in violations + stale:
            print(f"  {v}")
        return 1
    print(f"foundation-include-lint: ok ({len(files)} files, {len(used)} allowlisted edge(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
