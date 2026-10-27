#!/usr/bin/env python3
"""Read `nm` listings: which external symbols an archive defines or needs.

One reader for the archive gates, so a type letter means the same thing in
each of them.

`nm -g -P` listings come in two spellings: `nm -A -P` prefixes every line with
`archive[member]:`, plain `nm -P` does not. The spelling is decided once per
listing, not per line, because a symbol name may itself end in ":"
(Objective-C selector stubs such as `_objc_msgSend$initWithUTF8String:`),
which a per-line guess would read as a prefix. Member headers, blank lines,
warnings and prose are skipped.

There are two notions of "defined", and each caller picks one on purpose:

* defined_strong(): T D B R S C G. A dependency gate uses this. Weak
  definitions (W/V) are left out, because two archives that both emit the same
  inline function or template instantiation do not depend on each other.
* defined_any(): every type except the undefined ones. A contamination floor
  uses this. A class's typeinfo and vtable are often weak, and a library that
  needs another's typeinfo needs that library even though the definition is
  weak.

undefined() is U plus GNU/LLVM's weak undefined references w/v, which are
still relocations that may resolve from another archive.

The W/V letters only exist on ELF. Apple's nm prints a Mach-O weak definition
as S, T or D, so on macOS defined_strong() and defined_any() read the same set
and the strong/weak split above is effectively ELF-only. To tell weak from
strong on Mach-O, read `nm -g -m` (or `llvm-nm --format=darwin`) with
darwin_entries(); strength() picks whichever source the archive supports.
"""

from __future__ import annotations

import re
from typing import Iterator

STRONG_DEFINED = frozenset("TDBRSCG")
WEAK_DEFINED = frozenset("WV")
UNDEFINED = frozenset("Uwv")
_HEX = re.compile(r"[0-9a-fA-F]+")
# `nm -m`: "<value> (<segment,section>) <attributes> <name>", or
# "(undefined) <attributes> <name>" with the value column left blank.
_DARWIN_LINE = re.compile(r"^\s*(?:[0-9a-fA-F]+\s+)?\((?P<section>[^)]*)\)\s+(?P<attrs>.*?)\s*(?P<name>\S+)$")


def _is_symbol_line(kind: str, rest: list[str]) -> bool:
    # `-P` writes "name type [value [size]]" with hexadecimal value and size,
    # so anything else (a header, a warning, prose) is not a symbol line.
    return len(kind) == 1 and kind.isalpha() and len(rest) <= 2 and all(_HEX.fullmatch(f) for f in rest)


def _prefixed(fields: list[str]) -> tuple[str | None, str, str] | None:
    if len(fields) >= 3 and fields[0].endswith(":") and _is_symbol_line(fields[2], fields[3:]):
        prefix = fields[0][:-1]
        member = prefix[prefix.index("[") + 1:-1] if prefix.endswith("]") and "[" in prefix else None
        return member, fields[1], fields[2]
    return None


def _plain(fields: list[str]) -> tuple[None, str, str] | None:
    if len(fields) >= 2 and _is_symbol_line(fields[1], fields[2:]):
        return None, fields[0], fields[1]
    return None


def entries(listing: str) -> Iterator[tuple[str | None, str, str]]:
    """(member or None, symbol, type letter) for every symbol line."""
    rows = [line.split() for line in listing.splitlines()]
    # The `nm -A` spelling, unless some line only reads as plain: an `-A`
    # listing has no such line, while a plain line whose name ends in ":"
    # does not read as `-A` (its "type" would be the real type's value).
    plain = any(_plain(f) and not _prefixed(f) for f in rows)
    read = _plain if plain else _prefixed
    for fields in rows:
        entry = read(fields)
        if entry:
            yield entry


def defined_strong(listing: str) -> set[str]:
    """Symbols with a strong definition (T D B R S C G)."""
    return {name for _, name, kind in entries(listing) if kind in STRONG_DEFINED}


def defined_any(listing: str) -> set[str]:
    """Symbols with any definition, weak (W/V) included: every type but U/w/v."""
    return {name for _, name, kind in entries(listing) if kind not in UNDEFINED}


def undefined(listing: str) -> set[str]:
    """Symbols some member references, weak references included."""
    return {name for _, name, kind in entries(listing) if kind in UNDEFINED}


def weak_defined(listing: str) -> set[str]:
    """Symbols a `-P` listing defines only weakly (W/V and no strong copy).

    Always empty for Apple's nm on Mach-O; use darwin_entries() there.
    """
    weak = {name for _, name, kind in entries(listing) if kind in WEAK_DEFINED}
    return weak - defined_strong(listing)


def darwin_entries(listing: str) -> Iterator[tuple[str, str]]:
    """(symbol, "strong" | "weak" | "undefined") from `nm -g -m` output.

    "weak external", "weak private external" and "weak external automatically
    hidden" are weak definitions; "external" and "private external" are strong
    ones; "(undefined)" lines are references. Member headers ("x.o:") and
    non-external symbols are skipped.
    """
    for line in listing.splitlines():
        match = _DARWIN_LINE.match(line)
        if not match:
            continue
        words = match["attrs"].split()
        if "external" not in words or "non-external" in words:
            continue
        if match["section"] == "undefined":
            yield match["name"], "undefined"
        else:
            yield match["name"], "weak" if words[0] == "weak" else "strong"


def is_darwin(listing: str) -> bool:
    """Whether a listing is `nm -m` darwin-format output."""
    return any(True for _ in darwin_entries(listing))


def looks_mach_o(listing: str) -> bool:
    """Whether a `-P` listing reads as Apple nm on a Mach-O archive: it defines
    a C++ symbol with Mach-O's extra leading underscore (`__Z...`) and prints
    no W/V letter, which Apple's nm never uses. Its weak definitions are then
    invisible to the letters and need `nm -m`."""
    defined = [(name, kind) for _, name, kind in entries(listing) if kind not in UNDEFINED]
    return (any(name.startswith("__Z") for name, _ in defined)
            and not any(kind in WEAK_DEFINED for _, kind in defined))


def strength(listing: str, darwin_listing: str | None = None) -> tuple[set[str], set[str]]:
    """(strong, weak-only) definitions of one archive.

    `listing` is its `nm -g -P` text. When `darwin_listing` is darwin-format
    `nm -g -m` text (Mach-O), weakness comes from there; otherwise from the
    W/V letters (ELF).
    """
    if darwin_listing is not None and is_darwin(darwin_listing):
        strong: set[str] = set()
        weak: set[str] = set()
        for name, kind in darwin_entries(darwin_listing):
            if kind == "strong":
                strong.add(name)
            elif kind == "weak":
                weak.add(name)
        return strong, weak - strong
    return defined_strong(listing), weak_defined(listing)
