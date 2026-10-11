#!/usr/bin/env python3
"""Symbol-level closure check for a static archive.

A CMake link-closure check reads target edges, and an edge is only a claim: a
static archive can drop a link while one of its objects still calls into the
library it dropped. The archive is then edge-closed but not symbol-closed, and
the missing library resurfaces wherever a consumer happens to pull that object.
This reads the archives themselves.

    archive_symbol_closure_check.py --nm <nm> --subject <lib.a>
        --forbidden <lib.a> [--forbidden ...] --control <lib.a> [--control ...]

The subject's undefined global symbols must not be defined by any forbidden
archive. The check also requires the subject to reference at least one symbol
that a control archive defines (a dependency it is allowed to have). If that
count is zero, nm read nothing, so an empty forbidden intersection proves
nothing and the check reports INCONCLUSIVE rather than PASS.

Exit status: 0 pass, 1 forbidden reference, 2 inconclusive or unusable input.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# POSIX nm type letters for symbols a member DEFINES with external linkage. Weak
# definitions (W/V) are left out: an inline function emitted in both archives
# is not a dependency of one on the other.
DEFINED_TYPES = set("TDBRSCG")
UNDEFINED_TYPES = set("U")


def read_symbols(nm: str, archive: Path) -> tuple[set[str], set[str]]:
    """(undefined, defined) external symbol names in `archive`."""
    proc = subprocess.run([nm, "-g", "-P", str(archive)], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"{nm} failed on {archive}: {proc.stderr.strip()}")
    undefined: set[str] = set()
    defined: set[str] = set()
    for line in proc.stdout.splitlines():
        fields = line.split()
        # Member headers ("lib.a[x.o]:") and blank lines carry no type field.
        if len(fields) < 2 or len(fields[1]) != 1:
            continue
        name, kind = fields[0], fields[1]
        if kind in UNDEFINED_TYPES:
            undefined.add(name)
        elif kind in DEFINED_TYPES:
            defined.add(name)
    # A symbol one member references and another member defines is internal.
    return undefined - defined, defined


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nm", required=True)
    parser.add_argument("--subject", required=True, type=Path)
    parser.add_argument("--forbidden", action="append", default=[], type=Path)
    parser.add_argument("--control", action="append", default=[], type=Path)
    args = parser.parse_args(argv)
    if not args.forbidden or not args.control:
        print("archive-symbol-closure: INCONCLUSIVE: need at least one --forbidden and one "
              "--control archive", file=sys.stderr)
        return 2
    try:
        undefined, _ = read_symbols(args.nm, args.subject)
        controlled = 0
        for archive in args.control:
            controlled += len(undefined & read_symbols(args.nm, archive)[1])
        violations: dict[Path, list[str]] = {}
        for archive in args.forbidden:
            hit = sorted(undefined & read_symbols(args.nm, archive)[1])
            if hit:
                violations[archive] = hit
    except (OSError, RuntimeError) as exc:
        print(f"archive-symbol-closure: INCONCLUSIVE: {exc}", file=sys.stderr)
        return 2

    name = args.subject.name
    if violations:
        for archive, symbols in violations.items():
            print(f"archive-symbol-closure: FAIL: {name} references {len(symbols)} symbol(s) "
                  f"defined in {archive.name}:", file=sys.stderr)
            for symbol in symbols[:20]:
                print(f"  {symbol}", file=sys.stderr)
        return 1
    if controlled == 0:
        print(f"archive-symbol-closure: INCONCLUSIVE: {name} references no symbol of any control "
              f"archive ({len(undefined)} undefined read); the instrument saw nothing",
              file=sys.stderr)
        return 2
    print(f"archive-symbol-closure: PASS: {name}: {len(undefined)} undefined symbol(s), "
          f"{controlled} resolved by control archive(s), 0 by "
          f"{', '.join(a.name for a in args.forbidden)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
