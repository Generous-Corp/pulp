#!/usr/bin/env python3
"""Fail when enabled PulpGain formats lack their macOS validation tests."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys


EXPECTED_BY_FEATURE = {
    "PULP_HAS_CLAP": "clap-dlopen-PulpGain",
    "PULP_HAS_AUSDK": "auval-PulpGain",
}
TRUE_CACHE_VALUES = {"1", "ON", "TRUE", "YES", "Y"}


def enabled_features(cache_text: str) -> set[str]:
    enabled: set[str] = set()
    for line in cache_text.splitlines():
        if line.startswith("//") or line.startswith("#") or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        key = key_and_type.split(":", 1)[0]
        if key in EXPECTED_BY_FEATURE and value.strip().upper() in TRUE_CACHE_VALUES:
            enabled.add(key)
    return enabled


def test_names(inventory: dict[str, object]) -> set[str]:
    tests = inventory.get("tests", [])
    if not isinstance(tests, list):
        raise ValueError("CTest JSON inventory has no tests list")
    return {
        str(test["name"])
        for test in tests
        if isinstance(test, dict) and isinstance(test.get("name"), str)
    }


def missing_expected_tests(cache_text: str, inventory: dict[str, object]) -> list[str]:
    names = test_names(inventory)
    return [
        EXPECTED_BY_FEATURE[feature]
        for feature in sorted(enabled_features(cache_text))
        if EXPECTED_BY_FEATURE[feature] not in names
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()

    cache_path = args.build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        print(f"FAIL: configured cache is missing: {cache_path}", file=sys.stderr)
        return 2

    result = subprocess.run(
        ["ctest", "--test-dir", str(args.build_dir), "--show-only=json-v1"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(result.stdout, end="", file=sys.stderr)
        print(result.stderr, end="", file=sys.stderr)
        print("FAIL: could not read the configured CTest inventory", file=sys.stderr)
        return result.returncode or 2

    try:
        inventory = json.loads(result.stdout)
        missing = missing_expected_tests(cache_path.read_text(encoding="utf-8"), inventory)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"FAIL: invalid configured validator inventory: {error}", file=sys.stderr)
        return 2

    if missing:
        print(
            "FAIL: enabled PulpGain format(s) lack configured validator(s): "
            + ", ".join(missing),
            file=sys.stderr,
        )
        return 1

    print("Configured PulpGain validator inventory is complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
