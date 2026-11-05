#!/usr/bin/env python3
"""Compare an installed-SDK G-5 consumer observation to the frozen trace."""

import json
import sys
from pathlib import Path


EXPECTED = Path(__file__).with_name("expected.json")


def check(observed: dict) -> list[str]:
    expected = json.loads(EXPECTED.read_text())
    errors: list[str] = []
    if observed.get("schema") != expected["schema"]:
        errors.append("schema mismatch")
    partitions = observed.get("partitions")
    if not isinstance(partitions, list) or len(partitions) != 2:
        return errors + ["expected exactly two partitions"]
    for index, (actual, frozen) in enumerate(zip(partitions, expected["partitions"])):
        if not isinstance(actual, dict):
            errors.append(f"partition {index}: invalid object")
            continue
        for field in ("blocks", "events", "sample_zero", "sample_one"):
            if actual.get(field) != frozen[field]:
                errors.append(f"partition {index}: {field} differs from frozen oracle")
    return errors


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_trace.py OBSERVATIONS.json", file=sys.stderr)
        return 2
    try:
        observed = json.loads(Path(sys.argv[1]).read_text())
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2
    errors = check(observed)
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
