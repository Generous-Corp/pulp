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

The parser and the crossing rules are shared with every other layer boundary in
``include_boundary.py`` (boundary ``foundation``); this entry point keeps the
foundation gate's name and allowlist. Known edges that are tracked for removal
are listed in ``ALLOWED`` with their
reason. An allowlist entry that no longer matches anything is itself a failure,
so a removed edge cannot leave a stale exemption behind.

Exit codes: 0 = clean, 1 = violation found, 2 = scan error (including a scan
that found no files, which means the instrument looked in the wrong place).
"""
from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import include_boundary  # noqa: E402

ROOTS = include_boundary.BOUNDARIES["foundation"].roots

# (file, include) -> reason. Every entry must still match, see module doc.
ALLOWED: dict[tuple[str, str], str] = include_boundary.ALLOWED["foundation"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args(argv)
    return include_boundary.run(include_boundary.BOUNDARIES["foundation"], args.root, ALLOWED)


if __name__ == "__main__":
    raise SystemExit(main())
