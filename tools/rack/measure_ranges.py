#!/usr/bin/env python3
"""Measure a vendor's parameter ranges without anybody opening Rack by hand.

CARTOG reads `ParamQuantity::minValue/maxValue/defaultValue` off the widgets
Rack has instantiated, so a module's ranges exist only once that module has been
PLACED on the canvas. That has meant the ranges were, in practice, never
measured: they needed a person to open Rack, drag in the modules they cared
about, and know that doing so was the thing that unblocked the model.

Nothing about that needs a person. CARTOG rescans whenever the rack's module
count changes -- the button is only a forced refresh -- and Rack opens a patch
headlessly. So: write a patch containing CARTOG and the modules to measure,
open it headless, let CARTOG scan, and read the map back.

    measure_ranges.py CVfunk                  # every module of one plugin
    measure_ranges.py CVfunk Ouros Node       # only these
    measure_ranges.py --all                   # every installed plugin

AUDIO: headless Rack still opens an audio device for about a second. This
places no oscillator and connects no cables, so it is silence -- but it is not
nothing, and on a shared or occupied machine it should be announced rather than
sprung.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# Pro before Free, because a machine with both is a machine where Pro is the
# one being used. Overridable, because neither is guaranteed to be the build
# somebody wants measured.
RACK_CANDIDATES = (
    "/Applications/VCV Rack 2 Pro.app/Contents/MacOS/Rack",
    "/Applications/VCV Rack 2 Free.app/Contents/MacOS/Rack",
)

RACK_USER_DIR = os.path.expanduser("~/Library/Application Support/Rack2")
PORTMAP = os.path.join(RACK_USER_DIR, "forge-portmap.json")

# CARTOG has to be IN the patch: it measures the rack it is part of.
SCANNER = {"plugin": "ForgeModular", "model": "CARTOG"}


def rack_binary() -> str | None:
    override = os.environ.get("FORGE_RACK_BIN")
    if override:
        return override if os.path.exists(override) else None
    for p in RACK_CANDIDATES:
        if os.path.exists(p):
            return p
    return None


def installed_modules(plugin: str) -> list[str]:
    """Model slugs a plugin actually ships, read from its own manifest.

    From the plugin, not from the library index: the index describes what the
    library OFFERS, and a patch naming a model this install does not have is a
    patch Rack refuses to open -- which would read as a scan that found nothing.
    """
    for root in (os.path.join(RACK_USER_DIR, "plugins-mac-arm64"),
                 os.path.join(RACK_USER_DIR, "plugins-mac-x64")):
        man = os.path.join(root, plugin, "plugin.json")
        if os.path.exists(man):
            with open(man) as f:
                doc = json.load(f)
            return [m["slug"] for m in doc.get("modules", []) if m.get("slug")]
    return []


def write_patch(path: str, plugin: str, models: list[str]) -> int:
    """A patch that is only a scanner and its subjects. No cables, no sound."""
    mods = [dict(SCANNER, id=0, pos=[0, 0])]
    for i, model in enumerate(models, start=1):
        # Row-major, because Rack refuses to place two modules in one slot and
        # a refused module is a module that never gets measured.
        mods.append({"id": i, "plugin": plugin, "model": model,
                     "pos": [(i * 8) % 128, i // 16]})
    with open(path, "w") as f:
        json.dump({"version": "2.6.6", "modules": mods, "cables": []}, f)
    return len(mods)


def measured(portmap: dict, plugin: str) -> tuple[int, int]:
    """(modules with params, modules whose params carry ranges)."""
    # `modules` is a LIST of entries carrying their own plugin/model, not a
    # dict keyed by slug. Reading it as a dict returns nothing for every
    # plugin, which looks exactly like "the scan measured nothing".
    with_params = with_ranges = 0
    for entry in (portmap.get("modules") or []):
        if entry.get("plugin") != plugin:
            continue
        params = entry.get("params") or []
        if not params:
            continue
        with_params += 1
        if any("minValue" in p for p in params):
            with_ranges += 1
    return with_params, with_ranges


def read_portmap() -> dict:
    if not os.path.exists(PORTMAP):
        return {}
    try:
        with open(PORTMAP) as f:
            return json.load(f)
    except Exception:                                       # noqa: BLE001
        return {}


def main(argv: list[str]) -> int:
    args = argv[1:]
    if not args:
        print(__doc__.strip().split("\n\n")[-2])
        return 2

    rack = rack_binary()
    if not rack:
        print("no Rack found; set FORGE_RACK_BIN", file=sys.stderr)
        return 2

    if args[0] == "--all":
        root = os.path.join(RACK_USER_DIR, "plugins-mac-arm64")
        plugins = [d for d in sorted(os.listdir(root))
                   if os.path.isdir(os.path.join(root, d))]
    else:
        plugins = [args[0]]
    only = args[1:] if len(args) > 1 and args[0] != "--all" else []

    settle = float(os.environ.get("FORGE_RACK_SETTLE", "8"))
    total_before = total_after = 0

    for plugin in plugins:
        models = only or installed_modules(plugin)
        if not models:
            print(f"{plugin}: no installed modules found — skipped")
            continue

        before = measured(read_portmap(), plugin)
        tmp = tempfile.mkdtemp()
        patch = os.path.join(tmp, "measure.vcv")
        n = write_patch(patch, plugin, models)

        print(f"{plugin}: placing {n} modules (scanner + {len(models)}) "
              f"— headless Rack opens an audio device briefly", flush=True)
        try:
            subprocess.run([rack, "-h", "-u", RACK_USER_DIR, patch],
                           capture_output=True, timeout=settle + 45)
        except subprocess.TimeoutExpired:
            # Headless Rack does not always exit on its own. The scan happens
            # on load, long before any timeout, so a timeout is not a failure
            # -- what matters is whether the map changed.
            pass
        time.sleep(1.0)

        after = measured(read_portmap(), plugin)
        print(f"  params: {before[0]} -> {after[0]} modules   "
              f"ranges: {before[1]} -> {after[1]} modules")
        total_before += before[1]
        total_after += after[1]

    print(f"\nmodules carrying measured ranges: {total_before} -> {total_after}")
    # Nothing gained is not an error -- it may all have been measured already --
    # but it must not read as success either.
    if total_after == total_before:
        print("nothing new was measured; check that the plugin is installed "
              "and that Rack opened the patch")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
