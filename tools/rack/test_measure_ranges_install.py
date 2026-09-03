#!/usr/bin/env python3
"""A measured map is installed whole, or the previous one survives.

A library-wide run does hundreds of launches, and copying onto the live path
writes in place. That produced a map truncated mid-token with another record
spliced in behind it, which nothing downstream reported because an unparseable
map reads the same as a machine that never scanned.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_ranges  # noqa: E402

FAILED = 0


def check(cond: bool, msg: str) -> None:
    global FAILED
    print(("  ok     " if cond else "  WRONG  ") + msg)
    if not cond:
        FAILED += 1


def main() -> int:
    tmp = tempfile.mkdtemp()
    dest = os.path.join(tmp, "live.json")
    with open(dest, "w") as f:
        json.dump({"modules": [{"plugin": "Old"}]}, f)

    whole = os.path.join(tmp, "whole.json")
    with open(whole, "w") as f:
        json.dump({"modules": [{"plugin": "New"}]}, f)
    measure_ranges._install_map(whole, dest)
    check(json.load(open(dest))["modules"][0]["plugin"] == "New",
          "a whole map replaces the previous one")

    torn = os.path.join(tmp, "torn.json")
    with open(torn, "w") as f:
        f.write('{"modules": [{"plugin": "Half", "')
    raised = False
    try:
        measure_ranges._install_map(torn, dest)
    except Exception:                                       # noqa: BLE001
        raised = True
    check(raised, "a torn map raises instead of installing")
    try:
        survived = json.load(open(dest))["modules"][0]["plugin"] == "New"
    except Exception:                                       # noqa: BLE001
        survived = False          # the torn bytes reached the live path
    check(survived, "the previous map survives a torn write")
    check(not os.path.exists(dest + ".incoming"),
          "no holding file is left behind")

    print()
    print("all good" if not FAILED else f"{FAILED} wrong")
    return 1 if FAILED else 0


if __name__ == "__main__":
    raise SystemExit(main())
