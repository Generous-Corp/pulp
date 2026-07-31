#!/usr/bin/env python3
"""Install the canonical Linux packages needed to compile native Pulp targets."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import time
from collections.abc import Callable, Sequence

MANIFEST = pathlib.Path(__file__).with_name("linux_build_deps.json")
PACKAGE_RE = re.compile(r"^[a-z0-9][a-z0-9+.-]*$")
MAX_ATTEMPTS = 3


def load_profiles(path: pathlib.Path = MANIFEST) -> dict[str, list[str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1:
        raise ValueError(f"{path}: unsupported schema_version")
    profiles = data.get("profiles")
    if not isinstance(profiles, dict) or not profiles:
        raise ValueError(f"{path}: profiles must be a non-empty object")

    definitions: dict[str, tuple[list[str], list[str]]] = {}
    for name, definition in profiles.items():
        if not isinstance(name, str) or not name:
            raise ValueError(f"{path}: profile names must be non-empty strings")
        if not isinstance(definition, dict):
            raise TypeError(f"{path}: profile {name!r} must be an object")
        unknown_keys = set(definition) - {"extends", "packages"}
        if unknown_keys:
            raise ValueError(
                f"{path}: profile {name!r} has unknown keys: {sorted(unknown_keys)}"
            )
        parents = definition.get("extends", [])
        packages = definition.get("packages", [])
        if not isinstance(parents, list) or not all(
            isinstance(parent, str) and parent for parent in parents
        ):
            raise ValueError(f"{path}: profile {name!r} extends must be a string list")
        if parents != sorted(set(parents)):
            raise ValueError(
                f"{path}: profile {name!r} parents must be sorted and unique"
            )
        if not isinstance(packages, list) or not packages:
            raise ValueError(f"{path}: profile {name!r} must contain packages")
        if packages != sorted(set(packages)):
            raise ValueError(
                f"{path}: profile {name!r} packages must be sorted and unique"
            )
        for package in packages:
            validate_package(package)
        definitions[name] = (parents, packages)

    resolved: dict[str, list[str]] = {}

    def resolve(name: str, stack: tuple[str, ...] = ()) -> list[str]:
        if name in resolved:
            return resolved[name]
        if name not in definitions:
            raise ValueError(f"{path}: unknown parent profile {name!r}")
        if name in stack:
            cycle = " -> ".join((*stack, name))
            raise ValueError(f"{path}: profile inheritance cycle: {cycle}")
        parents, packages = definitions[name]
        combined = set(packages)
        for parent in parents:
            combined.update(resolve(parent, (*stack, name)))
        resolved[name] = sorted(combined)
        return resolved[name]

    for name in definitions:
        resolve(name)
    return resolved


def validate_package(package: object) -> str:
    if not isinstance(package, str) or not PACKAGE_RE.fullmatch(package):
        raise ValueError(f"invalid apt package name: {package!r}")
    return package


def resolve_packages(
    profile_names: Sequence[str],
    extras: Sequence[str],
    *,
    profiles: dict[str, list[str]] | None = None,
) -> list[str]:
    available = profiles if profiles is not None else load_profiles()
    if not profile_names:
        raise ValueError("at least one --profile is required")

    packages: set[str] = set()
    for name in profile_names:
        if name not in available:
            choices = ", ".join(sorted(available))
            raise ValueError(f"unknown profile {name!r}; choose from: {choices}")
        packages.update(available[name])
    packages.update(validate_package(package) for package in extras)
    return sorted(packages)


def run_with_retry(
    command: Sequence[str],
    *,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    sleeper: Callable[[float], None] = time.sleep,
    attempts: int = MAX_ATTEMPTS,
) -> None:
    for attempt in range(1, attempts + 1):
        result = runner(command, check=False, text=True)
        if result.returncode == 0:
            return
        if attempt == attempts:
            raise subprocess.CalledProcessError(result.returncode, command)
        delay = 5 * attempt
        print(
            f"apt command failed (attempt {attempt}/{attempts}); "
            f"retrying in {delay}s",
            file=sys.stderr,
        )
        sleeper(delay)


def install(packages: Sequence[str]) -> None:
    if sys.platform != "linux":
        raise RuntimeError("Linux dependency installation is supported only on Linux")
    prefix = [] if os.geteuid() == 0 else ["sudo"]
    env = os.environ.copy()
    env["DEBIAN_FRONTEND"] = "noninteractive"

    def runner(
        command: Sequence[str], *, check: bool, text: bool
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(command, check=check, text=text, env=env)

    run_with_retry([*prefix, "apt-get", "update"], runner=runner)
    run_with_retry([*prefix, "apt-get", "install", "-y", *packages], runner=runner)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", action="append", default=[])
    parser.add_argument(
        "--extra-packages",
        action="append",
        default=[],
        metavar="PACKAGES",
        help="Whitespace-separated lane-specific packages; may be repeated.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the resolved package set as JSON without running apt.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    extras = [
        package
        for value in args.extra_packages
        for package in shlex.split(value)
    ]
    try:
        packages = resolve_packages(args.profile, extras)
        if args.dry_run:
            print(json.dumps(packages, indent=2))
        else:
            install(packages)
    except (TypeError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"install-linux-build-deps: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
