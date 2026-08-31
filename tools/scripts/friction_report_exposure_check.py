#!/usr/bin/env python3
"""Validate a friction report's public/orchestrator exposure classification.

The checker is intentionally tiny, stdlib-only, and independent of Shipyard. It
validates the report contract; it does not attempt to prove that a named runtime
fix works.

Exit codes:
    0  valid classification
    1  report contract violation
    2  usage/read error
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


HEADING = "## Orchestrator independence"
FIELDS = (
    "Public exposure",
    "Evidence",
    "Public mechanism",
    "Public proof",
    "Orchestrator role",
)
FIELD_RE = re.compile(r"^\*\*(?P<name>[^*]+):\*\*\s*(?P<value>.*)\s*$", re.MULTILINE)
PLACEHOLDERS = {"", "todo", "tbd", "unknown", "<reason>", "<evidence>"}


def _section(text: str) -> str | None:
    start = text.find(HEADING)
    if start < 0:
        return None
    body_start = start + len(HEADING)
    next_heading = re.search(r"^##\s+", text[body_start:], re.MULTILINE)
    if next_heading is None:
        return text[body_start:]
    return text[body_start : body_start + next_heading.start()]


def _has_evidenced_na(value: str) -> bool:
    return bool(re.match(r"^N/A\s+[—-]\s+\S.+$", value, re.IGNORECASE))


def validate_text(text: str) -> list[str]:
    section = _section(text)
    if section is None:
        return [f"missing required heading: {HEADING}"]

    found: dict[str, str] = {}
    duplicates: set[str] = set()
    for match in FIELD_RE.finditer(section):
        name = match.group("name").strip()
        if name in found:
            duplicates.add(name)
        found[name] = match.group("value").strip()
    errors: list[str] = []
    for field in sorted(duplicates):
        errors.append(f"duplicate field: {field}")
    incomplete = False
    for field in FIELDS:
        value = found.get(field)
        if value is None:
            errors.append(f"missing field: {field}")
            incomplete = True
        elif value.casefold() in PLACEHOLDERS or value.startswith("<"):
            errors.append(f"field has no evidence: {field}")
            incomplete = True
    if incomplete:
        return errors

    exposure = found["Public exposure"].upper()
    if exposure not in {"EXPOSED", "NOT EXPOSED"}:
        errors.append("Public exposure must be EXPOSED or NOT EXPOSED")
        return errors

    if found["Evidence"].upper().startswith("N/A"):
        errors.append("Evidence must identify the lowest independently reproducing layer")

    mechanism = found["Public mechanism"]
    proof = found["Public proof"]
    role = found["Orchestrator role"]
    if exposure == "EXPOSED":
        if mechanism.upper().startswith("N/A"):
            errors.append("EXPOSED requires a public Pulp/Forge mechanism")
        if proof.upper().startswith("N/A"):
            errors.append("EXPOSED requires a focused public-path proof")
        if not (_has_evidenced_na(role) or "defense-in-depth" in role.casefold()):
            errors.append(
                "EXPOSED orchestrator role must be evidenced N/A or explicitly optional defense-in-depth"
            )
    else:
        if not _has_evidenced_na(mechanism):
            errors.append("NOT EXPOSED Public mechanism must be 'N/A — <reason>'")
        if not _has_evidenced_na(proof):
            errors.append("NOT EXPOSED Public proof must be 'N/A — <reason>'")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        text = args.report.read_text(encoding="utf-8")
    except OSError as exc:
        message = f"cannot read {args.report}: {exc}"
        if args.json:
            print(json.dumps({"ok": False, "error": message}))
        else:
            print(f"friction-report-exposure: ERROR: {message}", file=sys.stderr)
        return 2

    errors = validate_text(text)
    if args.json:
        print(json.dumps({"ok": not errors, "errors": errors}))
    elif errors:
        for error in errors:
            print(f"friction-report-exposure: {error}", file=sys.stderr)
    else:
        print("friction-report-exposure: OK")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
