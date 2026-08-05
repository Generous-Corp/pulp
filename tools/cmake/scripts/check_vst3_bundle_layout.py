#!/usr/bin/env python3
"""Validate the on-disk layout of built VST3 bundles.

`moduleinfo.json` belongs in `Contents/Resources/`, and putting it directly
under `Contents/` breaks two things at once, neither of which shows up until
far too late:

1. **The bundle cannot be code-signed.** A loose non-code file directly under
   `Contents/` makes codesign treat it as an unsigned nested code object:

       code object is not signed at all
       In subcomponent: .../Contents/moduleinfo.json

   An unsignable bundle cannot be notarized, so it cannot ship. Nothing before
   packaging notices — the plugin builds, loads and validates happily unsigned.

2. **Pulp's own scanner cannot read it.** `core/host/src/scanner.cpp` reads
   `Contents/Resources/moduleinfo.json` to recover stable FUIDs. A bundle with
   the file in the wrong place silently falls back, so the plugin still works
   and the metadata is simply never found.

Both failures are quiet, which is why this is a build-time check rather than a
line in a document. Run over a build tree; skips cleanly when no VST3 bundles
have been built.
"""

import argparse
import pathlib
import sys


def check(bundle: pathlib.Path) -> list[str]:
    """Return a list of problems with one bundle (empty when it is fine)."""
    problems = []
    stray = bundle / "Contents" / "moduleinfo.json"
    correct = bundle / "Contents" / "Resources" / "moduleinfo.json"

    if stray.exists():
        problems.append(
            f"{bundle.name}: moduleinfo.json is at Contents/moduleinfo.json; "
            f"it must be at Contents/Resources/moduleinfo.json or the bundle "
            f"cannot be code-signed")
    # Only require the file when the project ships one at all: plenty of
    # plugins have no moduleinfo.json, and demanding one would fail them for a
    # file they never authored.
    if stray.exists() and not correct.exists():
        problems.append(f"{bundle.name}: moduleinfo.json is missing from Contents/Resources/")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("build_dir", help="build tree to scan for *.vst3 bundles")
    args = ap.parse_args()

    root = pathlib.Path(args.build_dir)
    if not root.is_dir():
        print(f"check_vst3_bundle_layout: no such directory: {root}")
        return 0

    bundles = sorted(root.rglob("*.vst3"))
    bundles = [b for b in bundles if (b / "Contents").is_dir()]
    if not bundles:
        print("check_vst3_bundle_layout: no VST3 bundles built — skipping")
        return 0

    problems = []
    for b in bundles:
        problems += check(b)

    if problems:
        for p in problems:
            print(f"check_vst3_bundle_layout: {p}")
        return 1

    print(f"check_vst3_bundle_layout: {len(bundles)} bundle(s) OK ✅")
    return 0


if __name__ == "__main__":
    sys.exit(main())
