#!/usr/bin/env python3
"""Fail/warn when a built binary bakes in build-tree absolute paths.

The footgun this catches (TRIAZ 2026-06-18): an app read its design assets from
an absolute build-tree path at runtime — e.g. a compile definition like
  TRIAZ_FRAME_SVG="${CMAKE_CURRENT_SOURCE_DIR}/import/frames/mixer.frame.svg"
used in std::ifstream(path). That path only exists on the build machine, so a
copied / shared / distributed .app finds nothing and silently degrades (Pulp's
standalone fell back to the generic auto-Parameters panel). It built, signed,
notarized, and ran fine on the build box — so nothing flagged it until a user on
another Mac hit it.

This scanner reads the binary's printable strings and flags any that contain a
forbidden absolute prefix (the source / build dir, passed by the CMake wiring).
A shipped binary should never contain its own source/build path as a string —
assets belong embedded (pulp_embed_files / pulp_add_binary_data) or bundled into
Resources and loaded by an executable-relative path, never read from an absolute
build-tree path at runtime.

Usage:
  check_portable_binary.py <binary> --forbid-prefix <abs1> [--forbid-prefix <abs2> ...]
                           [--strict] [--label <name>]

Exit codes: 0 = clean (or warn-only with findings), 1 = findings in --strict mode,
2 = bad usage / unreadable binary.
"""
import argparse
import re
import sys

# Printable-ASCII runs of length >= 8 (path-like). Mirrors `strings -n 8`.
_STRINGS = re.compile(rb"[\x20-\x7e]{8,}")


def find_baked_paths(binary_path, forbid_prefixes):
    with open(binary_path, "rb") as f:
        blob = f.read()
    hits = []
    seen = set()
    for m in _STRINGS.finditer(blob):
        s = m.group().decode("ascii", "replace")
        for pre in forbid_prefixes:
            if pre and pre in s and s not in seen:
                seen.add(s)
                hits.append(s)
                break
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--forbid-prefix", action="append", default=[],
                    help="absolute path prefix that must NOT appear baked in the binary")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero on findings (default: warn only)")
    ap.add_argument("--label", default=None, help="friendly name for messages")
    a = ap.parse_args()

    prefixes = [p for p in a.forbid_prefix if p]
    if not prefixes:
        # Nothing to check against — succeed quietly rather than guess with a
        # heuristic that could false-positive on legitimate strings.
        return 0

    label = a.label or a.binary
    try:
        hits = find_baked_paths(a.binary, prefixes)
    except OSError as e:
        print(f"check_portable_binary: cannot read {a.binary}: {e}", file=sys.stderr)
        return 2

    if not hits:
        return 0

    tag = "ERROR" if a.strict else "WARNING"
    print(f"[{tag}] {label} bakes in build-tree absolute path(s) — NOT portable:",
          file=sys.stderr)
    for h in hits[:12]:
        print(f"    {h}", file=sys.stderr)
    if len(hits) > 12:
        print(f"    … and {len(hits) - 12} more", file=sys.stderr)
    print("  A shipped binary that references its build/source dir at runtime", file=sys.stderr)
    print("  will break on any machine but the build box (missing file → degraded UI).", file=sys.stderr)
    print("  Fix: EMBED the asset (pulp_embed_files / pulp_add_binary_data) or bundle", file=sys.stderr)
    print("  it into Resources and load via an executable-relative path — never read", file=sys.stderr)
    print("  an absolute build-tree path at runtime.", file=sys.stderr)
    return 1 if a.strict else 0


if __name__ == "__main__":
    sys.exit(main())
