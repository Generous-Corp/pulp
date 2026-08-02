#!/usr/bin/env python3
"""Select a Python new enough for Pulp's CI-only TOML contract checks."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys


MINIMUM = (3, 11)


def compatible_executable(candidate: str) -> str | None:
    try:
        result = subprocess.run(
            [
                candidate,
                "-c",
                "import sys; raise SystemExit(0 if sys.version_info >= (3, 11) else 1)",
            ],
            capture_output=True,
            text=True,
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    try:
        resolved = subprocess.run(
            [candidate, "-c", "import sys; print(sys.executable)"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    return resolved or None


def uv_managed_python() -> str | None:
    uv = shutil.which("uv")
    if not uv:
        return None
    try:
        result = subprocess.run(
            [uv, "python", "find", "3.12"], capture_output=True, text=True
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def main() -> int:
    override = os.environ.get("PULP_CI_PYTHON")
    if override:
        resolved = compatible_executable(override)
        if resolved:
            print(resolved)
            return 0
        print("PULP_CI_PYTHON must be Python 3.11 or newer", file=sys.stderr)
        return 1

    candidates = [
        path
        for name in ("python3.13", "python3.12", "python3.11", "python3")
        if (path := shutil.which(name))
    ]
    managed = uv_managed_python()
    if managed:
        candidates.append(managed)
    for candidate in dict.fromkeys(candidates):
        resolved = compatible_executable(candidate)
        if resolved:
            print(resolved)
            return 0
    print("Pulp CI requires Python 3.11 or newer (or an installed uv 3.12 runtime)", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
