#!/usr/bin/env python3
"""Verify a Cobertura XML is non-empty and structurally valid.

Pulp #605: a well-formed but structurally EMPTY Cobertura XML (`lines-
valid=0` on the root element) gets rejected by Codecov v5 as "Unusable
report". CI passed, upload succeeded, dashboard stayed empty — silent
data loss.

This script is the assertion gate. Two checks:
    1. File exists and is non-empty (size > 0).
    2. Root attribute `lines-valid` is parseable as int and > 0.

Invocation:
    python3 tools/scripts/verify_cobertura_xml.py <path/to/coverage.xml>

Optional flags:
    --label <name>   Label used in error messages (default: filename).
    --hint <text>    Extra ::error:: line appended on failure with
                     remediation guidance.

Exits 0 on success. Exits 1 with a `::error::`-prefixed message on
failure (workflow-friendly).

Replaces the inline `python3 -c 'import xml.etree.ElementTree as E ...'`
lines that appeared in `.github/workflows/coverage.yml` for the
native and Python Cobertura assertions (and is reusable for any
future Cobertura artifact verification). B1's classify-subject
extraction is the same shape — script over inline-Python.
"""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("xml", help="Path to the Cobertura XML to verify.")
    parser.add_argument(
        "--label",
        default=None,
        help=(
            "Human-readable label used in error messages. Defaults to the "
            "file's basename. Useful when a workflow has multiple "
            "Cobertura artifacts to distinguish (`coverage.cobertura.xml` "
            "vs `coverage.python.xml`)."
        ),
    )
    parser.add_argument(
        "--hint",
        default=None,
        help=(
            "Optional remediation hint appended on failure as a second "
            "`::error::` line. Use this to point readers at the specific "
            "upstream step that probably broke."
        ),
    )
    args = parser.parse_args(argv)

    path = Path(args.xml)
    label = args.label or path.name

    if not path.exists() or path.stat().st_size == 0:
        print(
            f"::error::{label} is missing or empty.",
            file=sys.stderr,
        )
        if args.hint:
            print(f"::error::{args.hint}", file=sys.stderr)
        return 1

    print(f"{label} size: {path.stat().st_size} bytes")

    try:
        root = ET.parse(str(path)).getroot()
    except ET.ParseError as e:
        print(f"::error::{label} failed to parse: {e}", file=sys.stderr)
        return 1

    try:
        lines_valid = int(root.attrib.get("lines-valid", "0"))
    except ValueError:
        lines_valid = 0

    if lines_valid == 0:
        print(
            f"::error::{label} is structurally empty (lines-valid=0).",
            file=sys.stderr,
        )
        if args.hint:
            print(f"::error::{args.hint}", file=sys.stderr)
        return 1

    print(f"{label} has lines-valid={lines_valid}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
