#!/usr/bin/env python3
"""Corpus for the geometry contract the preview consumes.

The contract exists so a source change cannot silently reshape what the
preview reads. That only holds if something checks the shape, so this asserts
the fields, their types and the invariants that make them usable -- and
pointedly asserts the two degradation flags, since those are what stop the
preview drawing a cable into a jack it cannot know the position of.

    python3 tools/rack/test_rack_geometry.py
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rack_geometry as G  # noqa: E402

REQUIRED = {"slug": str, "hp": int, "width": float, "height": float,
            "mapped": bool, "ports": list}


def main():
    doc = G.geometry()
    bad = 0

    if doc.get("version") != G.CONTRACT_VERSION:
        print(f"  WRONG  version is {doc.get('version')}, expected "
              f"{G.CONTRACT_VERSION}")
        bad += 1

    mods = doc.get("modules") or []
    if not mods:
        print("  SKIP   no port map recorded — run the MAP module in Rack first")
        return 0
    print(f"  ok     {len(mods)} modules, contract v{doc['version']}")

    for m in mods:
        for field, typ in REQUIRED.items():
            if field not in m:
                print(f"  WRONG  {m.get('slug', '?')} has no '{field}'")
                bad += 1
            elif not isinstance(m[field], typ):
                print(f"  WRONG  {m['slug']}.{field} is "
                      f"{type(m[field]).__name__}, expected {typ.__name__}")
                bad += 1
        if "image" not in m:
            print(f"  WRONG  {m.get('slug', '?')} has no 'image' key — it must "
                  f"be present and null rather than absent, so a consumer can "
                  f"tell 'no picture' from 'field missing'")
            bad += 1

    # Every panel is a 3U panel. A height that is not 380 means the recording
    # came from something other than a standard rack row, and the preview's
    # whole layout assumes otherwise.
    odd = [m["slug"] for m in mods if abs(m["height"] - 380.0) > 0.5]
    if odd:
        print(f"  WRONG  not 380 px tall: {odd}")
        bad += 1
    else:
        print(f"  ok     every panel is 380 px tall")

    # hp is derived from width, so they must agree or one of them is a lie.
    for m in mods:
        if abs(m["hp"] * 15.0 - m["width"]) > 0.6:
            print(f"  WRONG  {m['slug']}: {m['hp']} HP but {m['width']} px "
                  f"(15 px per HP)")
            bad += 1
            break
    else:
        print("  ok     hp and width agree")

    # x is a fraction so the preview can draw at any scale. A value outside
    # 0..1 means somebody exported pixels and the cables will land off-panel.
    for m in mods:
        for p in m["ports"]:
            if not (0.0 <= p["x"] <= 1.0):
                print(f"  WRONG  {m['slug']} port {p['index']} x={p['x']} is "
                      f"not a fraction of panel width")
                bad += 1
                break
        else:
            continue
        break
    else:
        print("  ok     port x is a fraction everywhere")

    # The degradations must be consistent: claiming ports while unmapped, or
    # claiming mapped with none, would both mislead the preview.
    for m in mods:
        if m["mapped"] != bool(m["ports"]):
            print(f"  WRONG  {m['slug']}: mapped={m['mapped']} but "
                  f"{len(m['ports'])} ports")
            bad += 1
            break
    else:
        print("  ok     'mapped' agrees with whether ports are present")

    unmapped = [m["slug"] for m in mods if not m["mapped"]]
    noimage = [m["slug"] for m in mods if not m.get("image")]
    print(f"  --     {len(unmapped)} unmapped, {len(noimage)} without an image "
          f"— both must render as placeholders, not as nothing")

    print(f"\n{'FAIL' if bad else 'ok'}: {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
