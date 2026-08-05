#!/usr/bin/env python3
"""What the shipped ranges must do, and must never do.

The machine that generates a seed already has every range in its own scan, so
a count taken there is identical whether the seed works or is ignored
entirely. Every case below therefore supplies its own paths and tests the
machine that has NOT scanned -- which is the only machine the seed exists for.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import portmap_seed  # noqa: E402

FAILED = 0


def ok(msg: str) -> None:
    print(f"  ok     {msg}")


def wrong(msg: str) -> None:
    global FAILED
    FAILED += 1
    print(f"  WRONG  {msg}")


def check(cond: bool, msg: str) -> None:
    ok(msg) if cond else wrong(msg)


def write(path: str, entries: list) -> str:
    with open(path, "w") as f:
        json.dump({"modules": entries}, f)
    return path


def entry(plugin, model, version, scan, lo=None):
    params = [{"index": 0, "name": "Freq", "kind": "knob"}]
    if lo is not None:
        params[0]["minValue"] = lo
        params[0]["maxValue"] = -lo
    return {"plugin": plugin, "model": model, "pluginVersion": version,
            "scan": scan, "params": params}


def inv_of(slug: str, version: str) -> dict:
    return {slug: {"version": version, "modules": {}}}


def main() -> int:
    tmp = tempfile.mkdtemp()
    seed = os.path.join(tmp, "seed.json")
    local = os.path.join(tmp, "local.json")
    absent = os.path.join(tmp, "nothing.json")

    # The case the whole file exists for: a machine that has never scanned.
    write(seed, [entry("Befaco", "EvenVCO", "2.4", 4, -54.0)])
    got = portmap_seed.entries(inv_of("Befaco", "2.4"), seed, absent)
    check(len(got) == 1 and portmap_seed.has_ranges(got[0]),
          "a machine with no scan of its own still gets shipped ranges")

    # A local scan is this machine's own truth and outranks what we shipped.
    write(local, [entry("Befaco", "EvenVCO", "2.4", 4, -12.0)])
    got = portmap_seed.entries(inv_of("Befaco", "2.4"), seed, local)
    check(len(got) == 1 and got[0]["params"][0]["minValue"] == -12.0,
          "a local scan wins over the shipped entry for the same module")

    # Wrong bounds are worse than none: a vendor update can renumber params,
    # so a version that does not match exactly is ignored, not approximated.
    got = portmap_seed.entries(inv_of("Befaco", "2.5"), seed, absent)
    check(got == [],
          "a shipped entry is dropped when the installed version differs")

    # Ranges only started being recorded at scan 4. A local block written by
    # an older scanner has none, so preferring it would discard real bounds.
    write(local, [entry("Befaco", "EvenVCO", "2.4", 3)])
    got = portmap_seed.entries(inv_of("Befaco", "2.4"), seed, local)
    check(len(got) == 1 and portmap_seed.has_ranges(got[0]),
          "a shipped scan-4 entry outranks a local scan-3 one that has none")

    # A module the seed has never heard of must still arrive from the scan.
    write(local, [entry("Bogaudio", "VCO", "2.0", 4, -30.0)])
    got = portmap_seed.entries(inv_of("Bogaudio", "2.0"), seed, local)
    check(len(got) == 1 and got[0]["plugin"] == "Bogaudio",
          "a locally scanned module absent from the seed still arrives")

    # A fresh checkout has no seed and a fresh machine has no scan; neither is
    # an error, and neither may raise.
    check(portmap_seed.entries(inv_of("Befaco", "2.4"), absent, absent) == [],
          "no seed and no scan yields nothing rather than an exception")

    # Without an inventory there is nothing to check a version against, so the
    # gate must not silently pass everything through as if it had matched.
    got = portmap_seed.entries(None, seed, absent)
    check(len(got) == 1,
          "with no inventory to check against, shipped entries are still read")

    # export must not ship entries that carry no ranges: they would match on
    # version and deliver nothing while shadowing nothing.
    write(local, [entry("A", "WithRange", "1.0", 4, -5.0),
                  entry("A", "NoRange", "1.0", 4)])
    out = os.path.join(tmp, "exported.json")
    got = portmap_seed.export(out, local)
    kept = json.load(open(out))["modules"]
    check(got["modules"] == 1 and len(kept) == 1
          and kept[0]["model"] == "WithRange",
          "export drops entries that carry no ranges")

    # A corrupt map is a reason to have no shipped ranges, never a crash in
    # the middle of building an inventory.
    with open(seed, "w") as f:
        f.write("{not json")
    check(portmap_seed.entries(inv_of("Befaco", "2.4"), seed, absent) == [],
          "an unreadable seed is ignored rather than raising")

    print()
    print("all good" if not FAILED else f"{FAILED} wrong")
    return 1 if FAILED else 0


if __name__ == "__main__":
    raise SystemExit(main())
