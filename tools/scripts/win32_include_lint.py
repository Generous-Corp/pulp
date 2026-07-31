#!/usr/bin/env python3
"""win32_include_lint.py — catch MSVC-only min/max macro leaks from <windows.h>.

Raw `#include <windows.h>` defines `min` and `max` as function-like MACROS. Any
later `std::max(a, b)` then fails to parse, and MSVC reports it at the *victim*:

    widgets.hpp(1802,68): error C2589: '(': illegal token on right side of '::'
    widgets.hpp(1802,68): error C2059: syntax error: ')'

`pulp/platform/win32_sane.hpp` exists to prevent exactly this: it pre-sets
NOMINMAX (and WIN32_LEAN_AND_MEAN) and only then includes <windows.h>.

The reason this must be linted rather than left to the Windows build is that
<windows.h> has an include guard, so **whichever header reaches it FIRST decides
whether NOMINMAX was set for the whole translation unit**. A public header that
includes it raw is therefore latent: it breaks a consumer only when it happens
to win that race, which depends on include order elsewhere. `widget_bridge.hpp`
carried a raw include for a long time and compiled fine, then began breaking
consumer plug-in builds against an installed SDK when the ordering shifted —
while Pulp's own required gate (macOS) stayed green throughout.

Scope is deliberately the INSTALLED HEADER surface (`core/*/include/**`). A
`.cpp` that leaks the macros breaks its own translation unit immediately and
locally; a header exports the hazard to every consumer that includes it.

A file is clean if it does not include <windows.h> at all, or defines NOMINMAX
before doing so, or is win32_sane.hpp itself.

Exit codes: 0 = clean, 1 = violation found, 2 = scan error.

Bypass: put `// win32-include-lint: skip <reason>` on the include line.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

RAW_INCLUDE = re.compile(r"^\s*#\s*include\s*<windows\.h>")
NOMINMAX_DEF = re.compile(r"^\s*#\s*define\s+NOMINMAX")
SKIP = "win32-include-lint: skip"

# The canonical wrapper is allowed to include <windows.h> raw — that is its job.
CANONICAL = "core/platform/include/pulp/platform/win32_sane.hpp"


def scan(path: pathlib.Path, rel: str) -> list[str]:
    """Return a list of violation messages for one header."""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:  # unreadable file is a scan error, not a violation
        raise RuntimeError(f"cannot read {rel}: {exc}") from exc

    if rel.replace("\\", "/") == CANONICAL:
        return []

    seen_nominmax = False
    out: list[str] = []
    for n, line in enumerate(lines, 1):
        if NOMINMAX_DEF.match(line):
            seen_nominmax = True
        elif RAW_INCLUDE.match(line):
            if seen_nominmax or SKIP in line:
                continue
            out.append(
                f"{rel}:{n}: raw '#include <windows.h>' with no NOMINMAX — "
                f"include <pulp/platform/win32_sane.hpp> instead"
            )
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args(argv)

    root = args.root.resolve()
    violations: list[str] = []
    try:
        for header in sorted(root.glob("core/*/include/**/*.hpp")):
            violations.extend(scan(header, str(header.relative_to(root))))
    except RuntimeError as exc:
        print(f"win32-include-lint: {exc}", file=sys.stderr)
        return 2

    if violations:
        print("win32-include-lint: raw <windows.h> in an installed header\n")
        for v in violations:
            print(f"  {v}")
        print(
            "\n<windows.h> defines min/max as macros, so the first header to "
            "include it decides\nNOMINMAX for the whole translation unit. A raw "
            "include here breaks consumers\nonly when it wins that race — which "
            "is why the macOS gate does not catch it."
        )
        return 1

    print("win32-include-lint: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
