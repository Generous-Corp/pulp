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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nm_symbols  # noqa: E402

# What "defined" means for --forbidden, chosen per gate. A dependency gate uses
# strong definitions only: an inline function emitted in both archives is not a
# dependency of one on the other. A contamination floor (--forbidden-defines
# any) also counts weak ones (W/V), because a class's typeinfo or vtable is
# often weak and still belongs to the archive that owns the class. Weak
# undefined references (w/v) always count as references.
DEFINED = {"strong": nm_symbols.defined_strong, "any": nm_symbols.defined_any}


def read_symbols(nm: str, archive: Path, rule: str = "strong") -> tuple[set[str], set[str]]:
    """(undefined, defined) external symbol names in `archive` under `rule`."""
    proc = subprocess.run([nm, "-g", "-P", str(archive)], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"{nm} failed on {archive}: {proc.stderr.strip()}")
    defined = DEFINED[rule](proc.stdout)
    # A symbol one member references and another member defines is internal.
    return nm_symbols.undefined(proc.stdout) - defined, defined


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nm", required=True)
    parser.add_argument("--subject", type=Path)
    parser.add_argument("--forbidden", action="append", default=[], type=Path)
    parser.add_argument("--control", action="append", default=[], type=Path)
    parser.add_argument("--check", action="append", default=[], metavar="NAME=ARCHIVE")
    parser.add_argument("--debt", action="append", default=[], metavar="NAME=REASON")
    parser.add_argument("--no-control", action="store_true", help="allow a pure forbidden-archive floor")
    parser.add_argument("--forbidden-defines", choices=sorted(DEFINED), default="strong",
                        help="what counts as a forbidden archive's definition: strong only (a "
                             "dependency gate) or any, weak included (a contamination floor)")
    args = parser.parse_args(argv)
    if args.check:
        checks = dict(item.split("=", 1) for item in args.check)
        debts = dict(item.split("=", 1) for item in args.debt)
        if not args.forbidden:
            print("archive-symbol-closure: INCONCLUSIVE: need --forbidden", file=sys.stderr)
            return 2
        try:
            rule = args.forbidden_defines
            forbidden_defs = set().union(*(read_symbols(args.nm, archive, rule)[1] for archive in args.forbidden))
            if not forbidden_defs:
                print("archive-symbol-closure: INCONCLUSIVE: forbidden archive defines no symbols", file=sys.stderr)
                return 2
            failed = False
            for name, archive_name in checks.items():
                undefined, _ = read_symbols(args.nm, Path(archive_name), rule)
                hits = sorted(undefined & forbidden_defs)
                if name in debts:
                    if hits:
                        print(f"archive-symbol-closure: {name}: {len(hits)} reference(s), allowed as debt: {debts[name]}")
                    else:
                        print(f"archive-symbol-closure: {name}: debt '{debts[name]}' no longer matches anything; remove it")
                        failed = True
                elif hits:
                    failed = True
                    print(f"archive-symbol-closure: {name} references {len(hits)} forbidden symbol(s):")
                    for symbol in hits[:20]: print(f"  {symbol}")
                else:
                    print(f"archive-symbol-closure: {name}: clean")
            return 1 if failed else 0
        except (OSError, RuntimeError) as exc:
            print(f"archive-symbol-closure: INCONCLUSIVE: {exc}", file=sys.stderr)
            return 2
    if not args.forbidden or (not args.control and not args.no_control):
        print("archive-symbol-closure: INCONCLUSIVE: need at least one --forbidden and one --control archive", file=sys.stderr)
        return 2
    try:
        rule = args.forbidden_defines
        undefined, _ = read_symbols(args.nm, args.subject, rule)
        controlled = 0
        for archive in args.control:
            controlled += len(undefined & read_symbols(args.nm, archive)[1])
        violations: dict[Path, list[str]] = {}
        for archive in args.forbidden:
            hit = sorted(undefined & read_symbols(args.nm, archive, rule)[1])
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
