#!/usr/bin/env python3
"""Reject mutable refs for third-party GitHub Actions.

GitHub-owned actions remain on their documented major tags. Every other
``uses: owner/name@ref`` in a workflow must use a 40-character commit SHA and
carry a human-readable ``# v...`` release comment.
"""
from __future__ import annotations

import pathlib
import re
import sys

USES = re.compile(r"^\s*(?:-\s*)?uses:\s*([^\s#]+)(?:\s+#\s*(.*))?\s*$")
SHA = re.compile(r"^[0-9a-f]{40}$")
VERSION = re.compile(r"^v\d+(?:\.\d+){0,3}$")
FIRST_PARTY = ("actions/", "github/")


def violations(paths: list[pathlib.Path]) -> list[str]:
    errors: list[str] = []
    for path in paths:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = USES.match(line)
            if not match:
                continue
            ref = match.group(1)
            if ref.startswith("./") or ref.startswith(FIRST_PARTY):
                continue
            if "@" not in ref:
                errors.append(f"{path}:{number}: third-party action has no ref: {ref}")
                continue
            action, pin = ref.rsplit("@", 1)
            comment = (match.group(2) or "").strip()
            if not SHA.fullmatch(pin):
                errors.append(f"{path}:{number}: {action} must use a full commit SHA, got {pin!r}")
            if not comment.startswith("v") or not VERSION.fullmatch(comment.split()[0]):
                errors.append(f"{path}:{number}: {action} SHA must carry a # vX.Y.Z release comment")
    return errors


def main(argv: list[str]) -> int:
    paths = [pathlib.Path(arg) for arg in argv] or sorted(pathlib.Path(".github/workflows").glob("*.y*ml"))
    errors = violations(paths)
    if errors:
        print("third-party action pin lint: violations found:", file=sys.stderr)
        print("\n".join(f"  ✗ {error}" for error in errors), file=sys.stderr)
        return 1
    print(f"third-party action pin lint: ok ({len(paths)} workflow files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
