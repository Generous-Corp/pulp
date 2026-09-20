#!/usr/bin/env python3
"""Assert that a Pulp D15 consumer resolves Dawn through vellum-gpu only."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def defined_symbols(path: Path) -> set[str]:
    """Return externally defined Mach-O symbols, excluding undefined references."""
    return subprocess.run(["nm", "-gU", str(path)], check=True, text=True,
                          capture_output=True).stdout.splitlines()


def is_defined(line: str) -> bool:
    # `nm -gU` writes undefined references as `                 U _symbol`.
    # The D15 consumer is expected to retain such references while vellum-gpu
    # supplies their definitions, so only an actual definition is forbidden.
    fields = line.split()
    return len(fields) >= 2 and fields[-2] != "U"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify_gpu_dawn_vellum_provider_topology.py <pulp-host> <vellum-gpu>")
    host, provider = map(Path, sys.argv[1:])
    host_symbols = {line.split()[-1] for line in defined_symbols(host) if is_defined(line)}
    provider_symbols = {line.split()[-1] for line in defined_symbols(provider) if is_defined(line)}
    for symbol in ("_dawnProcSetProcs", "_dawnProcGetVersion"):
        require(symbol not in host_symbols,
                f"Pulp D15 host unexpectedly defines its own Dawn symbol: {symbol}")
        require(symbol in provider_symbols,
                f"vellum-gpu does not own required Dawn symbol: {symbol}")
    require(not any("GetProcs" in symbol for symbol in host_symbols),
            "Pulp D15 host unexpectedly defines dawn::native::GetProcs")
    require(any("GetProcs" in symbol for symbol in provider_symbols),
            "vellum-gpu does not own dawn::native::GetProcs")
    require(not any("register_dawn_bootstrap" in symbol for symbol in host_symbols),
            "Pulp D15 host unexpectedly defines a bootstrap coordinator")
    require(any("register_dawn_bootstrap" in symbol for symbol in provider_symbols),
            "vellum-gpu does not own the bootstrap coordinator")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
