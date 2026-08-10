#!/usr/bin/env python3
"""Ship measured parameter ranges so no machine measures them twice.

A parameter's min, max and default come from `ParamQuantity`, which is
compiled into the module. `Fundamental/VCO Frequency: -54 -> 54` is true on
every machine that has that plugin version, forever. Measuring it needs Rack
running locally with the module placed; the RESULT does not. So the
measurement is machine-local and the knowledge is not, and making every
machine re-derive a universal fact is the tax this file removes.

WHY A SEPARATE FILE FROM THE AFFORDANCE SEED. Affordances deliberately do not
live in the port map: `portmap_merge.hpp` folds a fresh scan in by replacing a
re-measured module's whole block, so a classification stored there would be
erased by the next scan of the modules somebody is actively working with.
Ranges are the opposite case -- they ARE the scanner's fact, so the port map is
their right home and "a fresh local scan replaces what was shipped" is exactly
the behaviour wanted.

WHY THE VERSION MUST MATCH EXACTLY. A vendor update can add, remove or
renumber parameters. A shipped range attached to the wrong index is worse than
no range at all: absent bounds leave a value unconstrained, where wrong bounds
constrain it confidently to nonsense. So a shipped entry is used only where the
installed plugin is the exact version it was measured from, and is otherwise
ignored rather than approximated.
"""

from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SEED_PATH = os.path.join(HERE, "portmap-seed.json")
RACK_USER = os.path.expanduser("~/Library/Application Support/Rack2")
LOCAL_PATH = os.path.join(RACK_USER, "forge-portmap.json")


def _read(path: str) -> list:
    """Module entries from a port map, or none if it cannot be read.

    A missing or unreadable map is not an error: a fresh machine has no local
    scan, and a checkout has no shipped seed until one is generated.
    """
    if not os.path.exists(path):
        return []
    try:
        with open(path, encoding="utf-8") as source:
            doc = json.load(source)
    except Exception:
        return []
    entries = doc.get("modules")
    return entries if isinstance(entries, list) else []


def _key(entry: dict) -> tuple:
    return (entry.get("plugin"), entry.get("model"))


def _scan(entry: dict) -> int:
    v = entry.get("scan")
    return v if isinstance(v, int) else 0


def has_ranges(entry: dict) -> bool:
    return any("minValue" in p for p in (entry.get("params") or [])
               if isinstance(p, dict))


def entries(inv: dict | None = None,
            seed_path: str | None = None,
            local_path: str | None = None) -> list:
    """Port-map entries with shipped measurements folded under local ones.

    This machine's own scan wins, because it was read from the modules that
    are actually installed here. The one exception is a local entry recorded
    by an older scanner than the shipped one: those predate ranges being
    measured at all, so keeping them would discard real bounds in favour of a
    block that never had any.
    """
    merged: dict[tuple, dict] = {}

    for entry in _read(seed_path or SEED_PATH):
        if inv is not None:
            plug = inv.get(entry.get("plugin"))
            if not plug or entry.get("pluginVersion") != plug.get("version"):
                continue
        merged[_key(entry)] = entry

    for entry in _read(local_path or LOCAL_PATH):
        if inv is not None:
            plug = inv.get(entry.get("plugin"))
            if not plug or entry.get("pluginVersion") != plug.get("version"):
                continue
        shipped = merged.get(_key(entry))
        if shipped is not None and _scan(entry) < _scan(shipped):
            continue
        merged[_key(entry)] = entry

    return list(merged.values())


def export(dest: str, local_path: str | None = None) -> dict:
    """Write a seed holding every locally measured entry that carries ranges.

    Entries without ranges are dropped rather than shipped empty: they would
    occupy space, match on version, and deliver nothing, while shadowing
    nothing they could usefully replace.
    """
    kept = [e for e in _read(local_path or LOCAL_PATH) if has_ranges(e)]
    kept.sort(key=lambda e: (e.get("plugin") or "", e.get("model") or ""))
    params = sum(len([p for p in e.get("params") or [] if "minValue" in p])
                 for e in kept)
    with open(dest, "w") as f:
        json.dump({"modules": kept}, f, indent=1, sort_keys=True)
        f.write("\n")
    return {
        "modules": len(kept),
        "params": params,
        "vendors": len({e.get("plugin") for e in kept}),
        "path": dest,
    }


def _stats(path: str) -> dict:
    es = _read(path)
    with_r = [e for e in es if has_ranges(e)]
    return {
        "modules": len(es),
        "with_ranges": len(with_r),
        "params": sum(len([p for p in e.get("params") or [] if "minValue" in p])
                      for e in with_r),
        "vendors": len({e.get("plugin") for e in with_r}),
    }


def main(argv: list[str]) -> int:
    cmd = argv[1] if len(argv) > 1 else "stats"
    if cmd == "export":
        dest = argv[2] if len(argv) > 2 else SEED_PATH
        got = export(dest)
        print(f"wrote {got['path']}")
        print(f"  {got['modules']} modules, {got['params']} params, "
              f"{got['vendors']} vendors")
        return 0
    if cmd == "stats":
        for name, path in (("local ", LOCAL_PATH), ("seed  ", SEED_PATH)):
            s = _stats(path)
            if not s["modules"]:
                print(f"{name}: none at {path}")
                continue
            print(f"{name}: {s['modules']} modules, {s['with_ranges']} with "
                  f"ranges, {s['params']} params, {s['vendors']} vendors")
        return 0
    print(f"usage: {os.path.basename(argv[0])} [export [PATH] | stats]",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
