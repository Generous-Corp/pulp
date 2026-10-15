#!/usr/bin/env python3
"""Check that every host-facing format adapter keeps a tracing attachment.

The attachment is a reachability seam: when tracing is enabled, each adapter
must retain a live ``ScopedTracingAttachment`` so the linker cannot discard the
runtime attach/start/stop path.  This lexical check is intentionally build-free
and covers Apple-only sources on non-Apple CI.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

ADAPTERS = (
    "core/format/include/pulp/format/au_v2_instrument.hpp",
    "core/format/include/pulp/format/au_v2_adapter.hpp",
    "core/format/include/pulp/format/vst3_adapter.hpp",
    "core/format/include/pulp/format/clap_adapter.hpp",
    "core/format/include/pulp/format/standalone.hpp",
    "core/format/src/au_adapter.mm",
    "core/format/src/aax_runtime.cpp",
)

# A declaration, rather than a prose mention in a comment, must contain the
# type followed by an identifier and a declaration/initialization delimiter.
DECLARATION = re.compile(r"\bScopedTracingAttachment\s+[A-Za-z_]\w*\s*(?:[;=(])")
COMMENTS = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


def check(root: pathlib.Path) -> int:
    failed = False
    for relative in ADAPTERS:
        path = root / relative
        if not path.is_file():
            print(f"missing host adapter source: {relative}", file=sys.stderr)
            failed = True
            continue
        text = path.read_text(encoding="utf-8")
        if not DECLARATION.search(COMMENTS.sub("", text)):
            print(f"host adapter lacks ScopedTracingAttachment declaration: {relative}",
                  file=sys.stderr)
            failed = True
    if failed:
        return 1
    print(f"host adapter tracing contract passed ({len(ADAPTERS)} adapters)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args(argv)
    return check(args.root)


if __name__ == "__main__":
    raise SystemExit(main())
