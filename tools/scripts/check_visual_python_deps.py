#!/usr/bin/env python3
"""Fail loudly when the visual-analysis lane's declared Python deps are absent.

Seven ctest registrations in this repo import numpy, Pillow or scikit-image and
skip themselves when one of those is missing. Those skips are invisible in a
green run, so a lane that quietly lost a wheel reports success while the checks
it was built for never execute. This is the one registration that does not
skip: when the declared set is incomplete the suite fails and names the gap.

The declared set is read from the requirements file rather than restated here,
so adding a dependency there is enough to have it checked.
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_REQUIREMENTS = REPO / "tools" / "motion" / "visual" / "requirements.txt"

# Distributions whose import name differs from the name pip installs under.
# Anything absent falls back to the normalized distribution name.
IMPORT_NAMES = {
    "pillow": "PIL",
    "scikit-image": "skimage",
    "opencv-python": "cv2",
}

_DISTRIBUTION = re.compile(r"^[A-Za-z0-9._-]+")


def declared_distributions(requirements: Path) -> list[str]:
    """Return the distribution names pinned in a pip requirements file."""
    names: list[str] = []
    for raw in requirements.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith("-"):
            continue
        match = _DISTRIBUTION.match(line)
        if match:
            names.append(match.group(0))
    return names


def import_name(distribution: str) -> str:
    """Return the module name a distribution is imported under."""
    key = distribution.lower()
    return IMPORT_NAMES.get(key, key.replace("-", "_"))


def missing_distributions(distributions: list[str]) -> list[str]:
    """Return every declared distribution that cannot be imported.

    Every name is checked. The first failure does not short-circuit, so one run
    reports the whole gap instead of sending the reader round the loop once per
    missing wheel.
    """
    missing: list[str] = []
    for distribution in distributions:
        try:
            found = importlib.util.find_spec(import_name(distribution)) is not None
        except (ImportError, ValueError):
            found = False
        if not found:
            missing.append(distribution)
    return missing


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--requirements",
        type=Path,
        default=DEFAULT_REQUIREMENTS,
        help="pip requirements file declaring the dependency set to check",
    )
    args = parser.parse_args(argv)

    if not args.requirements.is_file():
        print(f"FAIL: no requirements file at {args.requirements}", file=sys.stderr)
        return 1

    declared = declared_distributions(args.requirements)
    if not declared:
        # An empty declaration would otherwise pass vacuously, which is the
        # same silent-green failure this check exists to prevent.
        print(f"FAIL: {args.requirements} declares no dependencies", file=sys.stderr)
        return 1

    missing = missing_distributions(declared)
    if missing:
        print(
            f"FAIL: {len(missing)} of {len(declared)} visual-analysis dependencies "
            f"are missing from {sys.executable}:",
            file=sys.stderr,
        )
        for distribution in missing:
            print(f"  - {distribution} (import {import_name(distribution)})", file=sys.stderr)
        print(
            "\nWithout these the visual and importer checks skip themselves and the\n"
            "suite reports green without having run them. Install with:\n"
            f"  {sys.executable} -m pip install --user -r {args.requirements}",
            file=sys.stderr,
        )
        return 1

    print(
        f"OK: {len(declared)} visual-analysis dependencies present "
        f"({', '.join(declared)}) in {sys.executable}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
