#!/usr/bin/env python3
"""Check the installed Pulp SDK surface required by ``--ui vellum``."""

from __future__ import annotations

import argparse
from pathlib import Path


def check(prefix: Path) -> list[str]:
    errors: list[str] = []
    if not (prefix / "include/pulp/vellum").is_dir():
        errors.append("include/pulp/vellum is missing")

    libraries = list((prefix / "lib").glob("libpulp-vellum.*"))
    if not libraries:
        errors.append("the pulp-vellum library is missing from lib")

    targets = prefix / "lib/cmake/Pulp/PulpTargets.cmake"
    if not targets.is_file() or "Pulp::vellum" not in targets.read_text(encoding="utf-8"):
        errors.append("the installed CMake package has no Pulp::vellum target")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("prefix", type=Path)
    args = parser.parse_args()
    errors = check(args.prefix)
    if errors:
        for error in errors:
            print(f"vellum-sdk-staging: ERROR: {error}")
        return 1
    print("vellum-sdk-staging: required Pulp::vellum surface present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
