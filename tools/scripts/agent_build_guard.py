#!/usr/bin/env python3
"""Reject raw agent cmake builds on hosts reserving capacity for gates."""
from __future__ import annotations
import argparse, json, os, re, shutil, subprocess, sys


def gate_reserved_cores() -> int:
    if os.environ.get("PULP_SETUP_ALLOW_FULL") == "1":
        return 0
    tartci = os.environ.get("PULP_TARTCI_BIN") or shutil.which("tartci")
    if not tartci:
        return 0
    try:
        p = subprocess.run([tartci, "host-profile", "--json"], capture_output=True, text=True, encoding="utf-8", timeout=5)
        if p.returncode:
            return 0
        value = json.loads(p.stdout).get("reserved_gate_cores", 0)
        return int(value) if isinstance(value, (int, float, str)) else 0
    except (OSError, ValueError, TypeError, subprocess.TimeoutExpired, json.JSONDecodeError):
        return 0


def raw_cmake_build(command: str) -> bool:
    compact = re.sub(r"\s+", " ", command).strip()
    # Only a cmake executable at the start of a shell segment is a build.
    # Mentions in grep, echo, or commit messages are ordinary text.
    return bool(re.search(r"(?:^|[;&|]\s*)(?:\S*/)?cmake\s+--build(?:\s|$)", compact))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--command", required=True)
    args = parser.parse_args()
    if raw_cmake_build(args.command) and gate_reserved_cores() > 0:
        print("agent-build-guard: refusing raw cmake --build on a gate host; use tools/ci/governed-build.sh cmake --build ...", file=sys.stderr)
        return 75
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
