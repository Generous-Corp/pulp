#!/usr/bin/env python3
"""Verify an installed Pulp SDK is internally consistent with its source tree.

The design importer is a BINARY plus a `browser_capture-v1/` JavaScript runtime
that ships beside it. They are two halves of one tool: the binary drives the
browser, the runtime defines the capture protocol and every gate that runs
inside the page. Refreshing one without the other produces an SDK that looks
updated, links fine, and silently runs an old protocol.

That is not hypothetical. A capture gate added to the runtime was never copied
into an SDK whose binary HAD been refreshed, so a panel that the gate rejects
locally installed anyway on the target machine, truncated. The failure surfaced
as a rendering bug in a shipped app, hours after the "fix" was believed
delivered.

Checked here rather than at build time because this is about an INSTALLED tree:
a developer SDK is assembled by copying, and copying is where halves drift.

    tools/scripts/check_sdk_complete.py <sdk-prefix> [--source <pulp-checkout>]

Exit 0 when consistent, 1 when not, 2 on bad usage.
"""

from __future__ import annotations

import argparse
import filecmp
import sys
from pathlib import Path


def check(prefix: Path, source: Path) -> list[str]:
    """Return a list of problems; empty means the SDK is consistent."""
    problems: list[str] = []

    importer = prefix / "bin" / "pulp-import-design"
    runtime = prefix / "bin" / "browser_capture-v1"
    src_runtime = source / "tools" / "import-design" / "browser_capture"

    if not importer.exists():
        problems.append(f"missing importer binary: {importer}")
    if not runtime.is_dir():
        problems.append(f"missing capture runtime directory: {runtime}")
    if problems:
        return problems

    if not src_runtime.is_dir():
        problems.append(f"source capture runtime not found: {src_runtime}")
        return problems

    # Only the runtime modules matter. Test files (*.test.mjs) are not shipped,
    # so their absence is correct and must not read as drift.
    shipped = sorted(
        p for p in src_runtime.glob("*.mjs") if not p.name.endswith(".test.mjs"))
    if not shipped:
        problems.append(f"no runtime modules found in {src_runtime}")
        return problems

    for src in shipped:
        installed = runtime / src.name
        if not installed.exists():
            problems.append(f"runtime module missing from the SDK: {src.name}")
        elif not filecmp.cmp(src, installed, shallow=False):
            # The message names the fix, because the instinct on seeing this is
            # to rebuild the binary — which is already current and is not what
            # drifted.
            problems.append(
                f"runtime module is STALE in the SDK: {src.name} "
                f"(copy {src} -> {installed}; rebuilding the binary will not "
                "fix it)")

    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path, help="installed SDK prefix")
    parser.add_argument(
        "--source", type=Path, default=Path(__file__).resolve().parents[2],
        help="Pulp source checkout the SDK was built from")
    args = parser.parse_args(argv)

    if not args.prefix.is_dir():
        print(f"error: no such SDK prefix: {args.prefix}", file=sys.stderr)
        return 2

    problems = check(args.prefix, args.source)
    if problems:
        print(f"SDK is inconsistent with {args.source}:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print(f"OK: {args.prefix} carries a complete, current design importer")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
