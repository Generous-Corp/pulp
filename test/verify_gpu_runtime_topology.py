#!/usr/bin/env python3
"""Assert one Dawn proc-table installer per image, in either device-layer mode.

A statically linked GPU consumer must carry exactly one Dawn proc setter and
exactly one installer of it:

  gpu-runtime mode (--mode gpu-runtime): gpu-runtime's coordinator
      (gpu_runtime::register_dawn_bootstrap) is defined once, and Pulp's own
      installer (pulp::render::ensure_dawn_procs and its registry) is absent,
      because the Pulp names forward to gpu-runtime's;
  Pulp mode (--mode pulp): Pulp's installer is defined once and no gpu_runtime
      symbol is linked at all. A provider image (--provider), which links
      gpu_audio without the render device layer, installs the table from
      gpu_audio's own exact-provider bootstrap instead, so in Pulp mode it
      carries neither Pulp's render installer nor any gpu_runtime symbol.

In both, _dawnProcSetProcs is defined exactly once. Symbols are read with
`nm -U -C` (defined only, demangled), which lists local symbols too, since a
static link with hidden visibility makes the coordinator local.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def defined(path: Path) -> list[str]:
    result = subprocess.run(["nm", "-U", "-C", str(path)], capture_output=True, text=True,
                            encoding="utf-8", check=False)
    if result.returncode != 0:
        raise SystemExit(f"nm failed on {path}: {result.stderr.strip()}")
    return [line.split(" ", 2)[-1] for line in result.stdout.splitlines() if line.strip()]


def count(symbols: list[str], needle: str) -> int:
    # The compiler may split a function's cold path into a second local
    # symbol (`... (.cold.1)`); that is the same definition, not a second one.
    return sum(1 for symbol in symbols if symbol.startswith(needle) and "(.cold." not in symbol)


def check(symbols: list[str], mode: str, provider: bool = False) -> list[str]:
    problems = []
    setters = sum(1 for symbol in symbols if symbol == "_dawnProcSetProcs")
    if setters != 1:
        problems.append(f"_dawnProcSetProcs is defined {setters} times, not once")
    coordinator = count(symbols, "gpu_runtime::register_dawn_bootstrap(")
    pulp_installer = count(symbols, "pulp::render::ensure_dawn_procs(")
    any_runtime = sum(1 for symbol in symbols if symbol.startswith("gpu_runtime::"))
    if mode == "gpu-runtime":
        if coordinator != 1:
            problems.append(f"gpu-runtime's coordinator is defined {coordinator} times, not once")
        if pulp_installer:
            problems.append("Pulp's own installer (pulp::render::ensure_dawn_procs) is linked "
                            "beside gpu-runtime's")
        if count(symbols, "pulp::render::detail::DawnProcInstallerRegistry"):
            problems.append("Pulp's installer registry is linked beside gpu-runtime's")
    else:
        expected = 0 if provider else 1
        if pulp_installer != expected:
            problems.append(f"Pulp's installer is defined {pulp_installer} times, not {expected}")
        if any_runtime:
            problems.append(f"{any_runtime} gpu_runtime symbols are linked in Pulp mode")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--mode", choices=("gpu-runtime", "pulp"), required=True)
    parser.add_argument("--provider", type=Path, action="append", default=[],
                        help="a provider image: gpu_audio without the render device layer")
    parser.add_argument("binary", type=Path, nargs="*")
    args = parser.parse_args(argv)
    if not args.binary and not args.provider:
        parser.error("no binaries to check")
    failed = False
    images = [(binary, False) for binary in args.binary] + [(b, True) for b in args.provider]
    for binary, provider in images:
        problems = check(defined(binary), args.mode, provider)
        for problem in problems:
            print(f"{binary.name}: {problem}", file=sys.stderr)
        failed |= bool(problems)
        if not problems:
            print(f"{binary.name}: one Dawn proc-table installer ({args.mode})")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
