#!/usr/bin/env python3
"""Keep the Swift C bridge's parameter/state shims free of view headers.

apple/Sources/PulpSwift/PulpBridge.cpp holds the pulp_param_*, pulp_state_* and
plug-in info shims. They need only the format and state headers; the motion
shims live in the pulp-vellum integration. This check fails when PulpBridge.cpp
includes any pulp/view/ header, which would drag the view layer back into
every consumer of the parameter bridge.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]pulp/view/[^>"]*[>"]')


def violations(path: Path) -> list[str]:
    found = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if INCLUDE.match(line):
            found.append(f"{path}:{number}: {line.strip()}")
    return found


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--file",
        type=Path,
        default=root / "apple" / "Sources" / "PulpSwift" / "PulpBridge.cpp",
    )
    args = parser.parse_args()
    if not args.file.is_file():
        print(f"swift-bridge-split-guard: {args.file} not found", file=sys.stderr)
        return 2
    found = violations(args.file)
    for item in found:
        print(f"swift-bridge-split-guard: view include in parameter bridge: {item}")
    if found:
        return 1
    print(f"swift-bridge-split-guard: OK ({args.file.name} has no pulp/view/ include)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
