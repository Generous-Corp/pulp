#!/usr/bin/env python3
"""Every Forge-exposed catalog pack must carry semantic descriptors.

The catalog index proves a pack is reachable. That is not the same as a pack
being *understandable*: a Forge agent choosing a node needs labels, units,
stepped states and descriptions, and when those live downstream they drift
silently. This gate fails closed so a newly indexed pack cannot land without
them.

Packs still awaiting migration are named in PENDING below. The list may only
shrink — a pack that is neither described nor pending is a failure, which is
what makes new DSP unable to land undescribed.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HOST_INCLUDE = Path("core/host/include/pulp/host")
INDEX = HOST_INCLUDE / "forge_catalog_index.hpp"

# Packs not yet carrying descriptor(). Shrinks as families migrate; never grows.
# A new pack must ship descriptors rather than be added here.
PENDING = {
    "forge_analog_vcf_catalog.hpp",
    "forge_character_delay_catalog.hpp",
    "forge_distortion_catalog.hpp",
    "forge_drum_catalog.hpp",
    "forge_dynamics_catalog.hpp",
    "forge_effect_modulation_catalog.hpp",
    "forge_fdn_reverb_catalog.hpp",
    "forge_lofi_catalog.hpp",
    "forge_modulation_catalog.hpp",
    "forge_pitch_catalog.hpp",
    "forge_saturator_catalog.hpp",
    "forge_sequencing_catalog.hpp",
    "forge_space_catalog.hpp",
    "forge_synthesis_catalog.hpp",
    "forge_tape_catalog.hpp",
}

DESCRIPTOR_RE = re.compile(r"^\s*inline\s+ForgeNodeDescriptor\s+\w+\s*\(", re.M)
INCLUDE_RE = re.compile(r'^\s*#include\s+<pulp/host/(forge_\w+\.hpp)>', re.M)


def indexed_packs(root: Path) -> list[str]:
    text = (root / INDEX).read_text(encoding="utf-8")
    # Comments name the deliberately-excluded detail/ headers; only real
    # #include lines are index entries.
    return [m.group(1) for m in INCLUDE_RE.finditer(text)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".", help="repository root")
    args = ap.parse_args()
    root = Path(args.root).resolve()

    if not (root / INDEX).exists():
        print(f"forge-descriptor-coverage: cannot read {INDEX}", file=sys.stderr)
        return 2

    packs = indexed_packs(root)
    if not packs:
        print("forge-descriptor-coverage: index parsed to zero packs", file=sys.stderr)
        return 2

    described, pending, missing = [], [], []
    for pack in packs:
        path = root / HOST_INCLUDE / pack
        if not path.exists():
            print(f"  indexed pack does not exist: {pack}", file=sys.stderr)
            missing.append(pack)
            continue
        if DESCRIPTOR_RE.search(path.read_text(encoding="utf-8")):
            described.append(pack)
        elif pack in PENDING:
            pending.append(pack)
        else:
            missing.append(pack)

    # A pack that gained descriptors must leave PENDING, or the list stops
    # meaning anything.
    stale = sorted(set(PENDING) & set(described))
    # An entry naming a pack the index no longer carries is equally stale.
    orphan = sorted(set(PENDING) - set(packs))

    print(f"forge-descriptor-coverage: {len(described)} described, "
          f"{len(pending)} pending, {len(packs)} indexed")

    ok = True
    if missing:
        ok = False
        print("\n  packs with no descriptors and no PENDING entry:", file=sys.stderr)
        for pack in sorted(missing):
            print(f"    {pack}", file=sys.stderr)
        print("\n  Add `inline ForgeNodeDescriptor descriptor()` to the pack.\n"
              "  Do NOT add it to PENDING — that list only shrinks.", file=sys.stderr)
    if stale:
        ok = False
        print("\n  packs that now have descriptors but are still listed PENDING:",
              file=sys.stderr)
        for pack in stale:
            print(f"    {pack}  → remove from PENDING", file=sys.stderr)
    if orphan:
        ok = False
        print("\n  PENDING entries naming a pack the index no longer carries:",
              file=sys.stderr)
        for pack in orphan:
            print(f"    {pack}  → remove from PENDING", file=sys.stderr)

    if not ok:
        print("\nforge-descriptor-coverage: FAILED", file=sys.stderr)
        return 1

    print("forge-descriptor-coverage: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
