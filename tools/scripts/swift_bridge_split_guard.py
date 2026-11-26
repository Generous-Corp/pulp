#!/usr/bin/env python3
"""Keep the Swift C bridges in their owning layers.

apple/Sources/PulpSwift/PulpBridge.cpp holds the pulp_param_*, pulp_state_* and
plug-in info shims. They need only the format and state headers; the motion
shims live in the pulp-vellum integration. This check fails when PulpBridge.cpp
includes any pulp/view/ header, which would drag the view layer back into
every consumer of the parameter bridge. The motion bridge must remain in
the pulp-vellum integration and include Vellum's motion API.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

PULP_VIEW_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]pulp/view/[^>"]*[>"]')
VELLUM_MOTION_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]vellum/view/motion\.hpp[>"]')


PULP_VIEW_MENTION = re.compile(r"pulp/view/|import\s+PulpView\b")
MOTION_DEFINITION = re.compile(r"\bpulp_motion_\w+|\bmotion::")
BRIDGE_SUFFIXES = {".swift", ".cpp", ".cc", ".mm", ".m", ".h", ".hpp"}
IMPLEMENTATION_SUFFIXES = {".cpp", ".cc", ".mm", ".m"}


def violations(path: Path) -> list[str]:
    found = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if PULP_VIEW_INCLUDE.match(line):
            found.append(f"{path}:{number}: {line.strip()}")
    return found


def apple_violations(apple_sources: Path) -> list[str]:
    """Every bridge source under apple/Sources: no pulp/view reference, and no
    motion shim implementation (the C ABI is declared in PulpBridge.h and
    defined in the pulp-vellum integration)."""
    found = []
    for path in sorted(apple_sources.rglob("*")):
        if path.suffix not in BRIDGE_SUFFIXES or not path.is_file():
            continue
        if ".build" in path.parts:
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        for number, line in enumerate(lines, 1):
            if PULP_VIEW_MENTION.search(line):
                found.append(f"{path}:{number}: view reference: {line.strip()}")
            elif path.suffix in IMPLEMENTATION_SUFFIXES and MOTION_DEFINITION.search(line):
                found.append(f"{path}:{number}: motion shim in core bridge: {line.strip()}")
    return found


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", type=Path,
                        default=root / "apple" / "Sources" / "PulpSwift" / "PulpBridge.cpp")
    parser.add_argument("--root", type=Path, default=root)
    args = parser.parse_args()
    root = args.root
    if args.file == parser.get_default("file"):
        args.file = root / "apple" / "Sources" / "PulpSwift" / "PulpBridge.cpp"
    if not args.file.is_file():
        print(f"swift-bridge-split-guard: {args.file} not found", file=sys.stderr)
        return 2
    found = violations(args.file)
    for item in found:
        print(f"swift-bridge-split-guard: view include in parameter bridge: {item}")
    motion_bridge = root / "integrations" / "pulp-vellum" / "src" / "swift_motion_bridge.cpp"
    if not motion_bridge.is_file():
        print(f"swift-bridge-split-guard: motion bridge not found: {motion_bridge}")
        return 1
    motion_text = motion_bridge.read_text(encoding="utf-8").splitlines()
    if not any(VELLUM_MOTION_INCLUDE.match(line) for line in motion_text):
        print(f"swift-bridge-split-guard: motion bridge lacks Vellum motion include: {motion_bridge}")
        return 1
    motion_violations = violations(motion_bridge)
    for item in motion_violations:
        print(f"swift-bridge-split-guard: view include in Vellum motion bridge: {item}")
    sweep = apple_violations(root / "apple" / "Sources")
    for item in sweep:
        print(f"swift-bridge-split-guard: {item}")
    if found or motion_violations or sweep:
        return 1
    print(f"swift-bridge-split-guard: OK ({args.file.name} and {motion_bridge.name} are split)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
