#!/usr/bin/env python3
"""Check the installed Pulp SDK surface required by ``--ui vellum``."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


TARGET_RE = re.compile(r"^\s*add_library\(\s*Pulp::vellum\s", re.MULTILINE)


def check(prefix: Path) -> list[str]:
    errors: list[str] = []
    if not (prefix / "include/pulp/vellum/editor.hpp").is_file():
        errors.append("include/pulp/vellum/editor.hpp is missing")

    if not (prefix / "lib/libpulp-vellum.a").is_file():
        errors.append("lib/libpulp-vellum.a is missing")

    targets = prefix / "lib/cmake/Pulp/PulpTargets.cmake"
    if not targets.is_file() or not TARGET_RE.search(targets.read_text(encoding="utf-8")):
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
