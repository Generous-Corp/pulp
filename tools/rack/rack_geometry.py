#!/usr/bin/env python3
"""The shape the patch preview consumes, and the only thing it should consume.

The preview needs to know how wide a module is, where its jacks sit and
whether we have a picture of it. Today all of that comes from a port map
recorded inside Rack plus a directory of PNGs Rack rendered. Neither is
guaranteed to stay the source: the browser-solved import work is still
settling its capture envelope, and a future Rack version could describe its
own modules.

So the preview should not read either of those directly. It reads this, and
when a source changes only the adapter below moves.

The contract, version 1. A list of:

    {
      "slug":   "Fundamental/VCO",   # plugin/model, the patch's own naming
      "hp":     9,                   # width in HP
      "width":  135.0,               # px, authoritative -- hp is derived
      "height": 380.0,               # px, always 380 for a 3U panel
      "image":  "/abs/path.png"      # or None if we have no picture
      "mapped": true,                # have we ever seen this module's ports?
      "ports": [
        {"index": 0, "dir": "in", "name": "1V/octave pitch",
         "x": 0.3852, "y": 286.0}    # x is a FRACTION of width, y is px
      ]
    }

Two fields exist because the preview has to degrade honestly rather than
guess. `image` is null for a module Rack has never been asked to render, and
the preview must draw a placeholder at the correct width rather than nothing.
`mapped` is false for a module nobody has placed in a rack, so its ports are
unknown and cables to it have to terminate at the panel edge rather than on a
jack that may not be there. A cable drawn confidently into the wrong hole
teaches somebody something false, which is the one thing a teaching surface
cannot do.

    rack_geometry.py            # the contract, as JSON
    rack_geometry.py --js       # the same, as a JS module
"""
from __future__ import annotations

import json
import os
import sys

CONTRACT_VERSION = 1

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def from_portmap() -> list:
    """Adapt the recorded port map into the contract.

    This is the whole seam. A different source -- a capture envelope, a future
    Rack that describes itself -- becomes another function of this shape, and
    nothing downstream notices.
    """
    import export_design_data as src
    out = []
    for m in src.collect():
        w, h = m["px"]
        out.append({
            "slug": m["slug"],
            "hp": m["hp"],
            "width": w,
            "height": h,
            "image": os.path.join(src.SHOTS, m["plugin"], m["model"] + ".png")
                     if m["img"] else None,
            "mapped": m["placed"],
            "ports": [{"index": p["index"], "dir": p["dir"],
                       "name": p["name"], "x": p["x"], "y": p["y"]}
                      for p in m["ports"]],
        })
    return out


def from_manifests() -> list:
    """Our own modules, straight from the manifests that generated them.

    The port map only knows what somebody has placed in a rack, and our own
    modules are precisely the ones a Forge Modular user has not placed yet --
    they were built seconds ago. Running the layout against a real patch showed
    eight of nine modules falling back to a placeholder for exactly this
    reason.

    We do not need the scan for these: the manifest already carries every
    port's position in millimetres, because that is what drew the panel. This
    converts it to the same shape the contract uses, so ours are exact from
    the moment they are generated.
    """
    import glob
    mdir = os.path.normpath(os.path.join(HERE, "..", "..", "examples",
                                         "forge-modular", "modules"))
    res = os.path.normpath(os.path.join(mdir, "..", "res"))
    hp_px, mm_px = 15.0, 75.0 / 25.4
    out = []
    for f in sorted(glob.glob(os.path.join(mdir, "*.json"))):
        if os.path.basename(f).startswith("_"):
            continue
        try:
            doc = json.load(open(f))
        except Exception:
            continue
        for m in doc.get("modules", []):
            slug = m.get("slug")
            if not slug:
                continue
            w = int(m.get("hp", 0)) * hp_px
            if w <= 0:
                continue
            ports = []
            for kind, is_in in (("inputs", True), ("outputs", False)):
                for p in m.get(kind, []) or []:
                    ports.append({
                        "index": p.get("id", 0),
                        "dir": "in" if is_in else "out",
                        "name": p.get("name") or p.get("label") or "",
                        "x": round((p.get("x_mm", 0.0) * mm_px) / w, 4),
                        "y": round(p.get("y_mm", 0.0) * mm_px, 1),
                    })
            png = os.path.join(res, slug + ".png")
            out.append({
                "slug": f"ForgeModular/{slug}",
                "hp": int(m["hp"]),
                "width": w,
                "height": 380.0,
                # Our panels are emitted as SVG; a PNG only exists if Rack has
                # rendered one, so this is honest either way.
                "image": png if os.path.exists(png) else None,
                "mapped": bool(ports),
                "ports": ports,
            })
    return out


def geometry() -> dict:
    """The contract, versioned so a consumer can refuse a shape it predates."""
    # Ours first, so a scan of an older build cannot overwrite geometry we
    # derived from the manifest that actually drew the panel.
    mods = from_manifests()
    have = {m["slug"] for m in mods}
    mods += [m for m in from_portmap() if m["slug"] not in have]
    mods.sort(key=lambda m: m["slug"])
    return {
        "version": CONTRACT_VERSION,
        "modules": mods,
        "counts": {
            "modules": len(mods),
            "with_image": sum(1 for m in mods if m["image"]),
            "mapped": sum(1 for m in mods if m["mapped"]),
            "ports": sum(len(m["ports"]) for m in mods),
        },
    }


def as_js(doc: dict) -> str:
    return ("// Module geometry for the patch preview.\n"
            "// Generated by tools/rack/rack_geometry.py — do not hand-edit.\n"
            "// x is a fraction of panel width; y is px from the panel top.\n"
            f"export const GEOMETRY_VERSION = {doc['version']};\n"
            f"export const MODULES = {json.dumps(doc['modules'], indent=2)};\n")


def main(argv):
    doc = geometry()
    if "--js" in argv:
        print(as_js(doc))
    else:
        print(json.dumps(doc, indent=2))
    c = doc["counts"]
    print(f"\n// v{doc['version']}: {c['modules']} modules · "
          f"{c['with_image']} with an image · {c['mapped']} mapped · "
          f"{c['ports']} ports", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
