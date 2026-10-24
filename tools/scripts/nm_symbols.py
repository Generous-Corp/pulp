#!/usr/bin/env python3
"""Read `nm -g -P` listings: which external symbols an archive defines or needs.

One reader for the archive gates, so a type letter means the same thing in
each of them. Both spellings are accepted: `nm -A -P` prefixes every line with
`archive[member]:`, plain `nm -P` does not; member headers and blank lines are
skipped. Type letters are POSIX `-P` output, identical for macOS and GNU/LLVM
nm.

There are two notions of "defined", and each caller picks one on purpose:

* defined_strong(): T D B R S C G. A dependency gate uses this. Weak
  definitions (W/V) are left out, because two archives that both emit the same
  inline function or template instantiation do not depend on each other.
* defined_any(): every type except the undefined ones. A contamination floor
  uses this. A class's typeinfo and vtable are often weak (W/V), and a library
  that needs another's typeinfo needs that library even though the definition
  is weak.

undefined() is U plus GNU/LLVM's weak undefined references w/v, which are
still relocations that may resolve from another archive.
"""

from __future__ import annotations

import re
from typing import Iterator

STRONG_DEFINED = frozenset("TDBRSCG")
UNDEFINED = frozenset("Uwv")
_HEX = re.compile(r"[0-9a-fA-F]+")


def _is_symbol_line(kind: str, rest: list[str]) -> bool:
    # `-P` writes "name type [value [size]]" with hexadecimal value and size,
    # so anything else (a header, a warning, prose) is not a symbol line. The
    # name itself may end in ":" (Objective-C selector stubs such as
    # `_objc_msgSend$initWithUTF8String:`), so the name is not checked.
    return len(kind) == 1 and kind.isalpha() and len(rest) <= 2 and all(_HEX.fullmatch(f) for f in rest)


def entries(listing: str) -> Iterator[tuple[str | None, str, str]]:
    """(member or None, symbol, type letter) for every symbol line."""
    for line in listing.splitlines():
        fields = line.split()
        # `nm -A` spelling first: "archive[member]: name type ...".
        if len(fields) >= 3 and fields[0].endswith(":") and _is_symbol_line(fields[2], fields[3:]):
            prefix = fields[0][:-1]
            member = prefix[prefix.index("[") + 1:-1] if prefix.endswith("]") and "[" in prefix else None
            yield member, fields[1], fields[2]
        # Plain spelling, including a name that ends in ":".
        elif len(fields) >= 2 and _is_symbol_line(fields[1], fields[2:]):
            yield None, fields[0], fields[1]


def defined_strong(listing: str) -> set[str]:
    """Symbols with a strong definition (T D B R S C G)."""
    return {name for _, name, kind in entries(listing) if kind in STRONG_DEFINED}


def defined_any(listing: str) -> set[str]:
    """Symbols with any definition, weak (W/V) included: every type but U/w/v."""
    return {name for _, name, kind in entries(listing) if kind not in UNDEFINED}


def undefined(listing: str) -> set[str]:
    """Symbols some member references, weak references included."""
    return {name for _, name, kind in entries(listing) if kind in UNDEFINED}
