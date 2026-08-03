#!/usr/bin/env python3
"""Every installed module must be in the port map.

The port map is the only place a module's knobs, jacks, lights and displays
are written down -- a plugin.json names a model and stops, and the panel
artwork does not draw its controls. So a module missing from the map is a
module the preview draws as a bare faceplate: labels floating over nothing,
no knobs, no jacks. That is a picture that looks like a rendering bug and is
actually a data gap.

This check needs no Rack. A plugin's manifest already lists every model it
ships, so coverage is a set difference between what is installed and what was
measured -- which is why it can run in CI on a machine with no GUI.

It exists because CARTOG only ever recorded modules PLACED in a rack, and the
map is merged and never rewritten. Fundamental reached 39/39 because somebody
once opened a patch holding all of them; ForgeModular sat at 1/30 -- CARTOG
itself -- for as long as the map has existed, and every Forge module has been
previewed from fallback geometry the whole time. Nothing noticed, because a
missing module produces a panel rather than an error.

    test_portmap_coverage.py                  # this machine
    PORTMAP=x.json PLUGINS=dir/ ... .py       # a map and plugin dir to check
"""

from __future__ import annotations

import glob
import json
import os
import sys

PORTMAP = os.environ.get("PORTMAP") or os.path.expanduser(
    "~/Library/Application Support/Rack2/forge-portmap.json")
PLUGINS = os.environ.get("PLUGINS") or os.path.expanduser(
    "~/Library/Application Support/Rack2/plugins-mac-arm64")

# Modules that ship with Rack itself rather than a plugin directory. They have
# no manifest on disk to enumerate, so they cannot be required here.
BUILTIN = {"Core"}

bad = 0


def ok(msg):
    print(f"  ok     {msg}")


def wrong(msg):
    global bad
    print(f"  WRONG  {msg}")
    bad += 1


def declared(plugins_dir):
    """{plugin_slug: {model_slug, ...}} from the installed manifests."""
    out = {}
    for d in sorted(glob.glob(os.path.join(plugins_dir, "*"))):
        j = os.path.join(d, "plugin.json")
        if not os.path.exists(j):
            continue
        try:
            man = json.load(open(j))
        except Exception as e:                              # noqa: BLE001
            wrong(f"unreadable manifest {j}: {e}")
            continue
        slug = man.get("slug")
        if not slug:
            continue
        out[slug] = {m["slug"] for m in man.get("modules", []) if m.get("slug")}
    return out


def measured(portmap_path):
    """{plugin_slug: {model_slug, ...}} from the port map."""
    out = {}
    if not os.path.exists(portmap_path):
        return out
    doc = json.load(open(portmap_path))
    mods = doc.get("modules")
    mods = list(mods.values()) if isinstance(mods, dict) else (mods or [])
    for m in mods:
        p, s = m.get("plugin"), m.get("model")
        if p and s:
            out.setdefault(p, set()).add(s)
    return out


def check(portmap_path, plugins_dir, label):
    """Report coverage per plugin. Returns True when everything is covered."""
    want, got = declared(plugins_dir), measured(portmap_path)
    if not want:
        print(f"  SKIP   {label}: no plugin manifests under {plugins_dir} "
              f"(this is a skip, not a pass)")
        return None
    clean = True
    for plugin in sorted(want):
        if plugin in BUILTIN:
            continue
        missing = want[plugin] - got.get(plugin, set())
        have = len(want[plugin]) - len(missing)
        if missing:
            clean = False
            shown = ", ".join(sorted(missing)[:8])
            more = f" (+{len(missing) - 8} more)" if len(missing) > 8 else ""
            wrong(f"{label}: {plugin} {have}/{len(want[plugin])} measured — "
                  f"missing {shown}{more}")
        else:
            ok(f"{label}: {plugin} {have}/{len(want[plugin])} measured")
    return clean


# ---------------------------------------------------------- can it even fail?
#
# A coverage check that cannot report a gap is a green light wired to nothing,
# and this one is a set difference -- exactly the shape that reads as passing
# when one of the two sets is empty. So both verdicts are exercised against
# built fixtures before the real map is judged by it.

import tempfile                                            # noqa: E402

with tempfile.TemporaryDirectory() as tmp:
    pdir = os.path.join(tmp, "plugins")
    os.makedirs(os.path.join(pdir, "Demo"))
    json.dump({"slug": "Demo", "version": "1.0",
               "modules": [{"slug": "AAA"}, {"slug": "BBB"}]},
              open(os.path.join(pdir, "Demo", "plugin.json"), "w"))

    full = os.path.join(tmp, "full.json")
    json.dump({"modules": [{"plugin": "Demo", "model": "AAA"},
                           {"plugin": "Demo", "model": "BBB"}]},
              open(full, "w"))
    partial = os.path.join(tmp, "partial.json")
    json.dump({"modules": [{"plugin": "Demo", "model": "AAA"}]},
              open(partial, "w"))

    before = bad
    if check(full, pdir, "fixture:complete") is True and bad == before:
        ok("a complete map passes")
    else:
        wrong("a complete map did not pass — the check cannot say yes")

    before = bad
    verdict = check(partial, pdir, "fixture:incomplete")
    if verdict is False and bad == before + 1:
        bad = before                     # that failure was the point
        ok("a map missing one module fails, and names it")
    else:
        wrong("a map missing a module was accepted — the check cannot say no")

# ------------------------------------------------------------- this machine
print()
result = check(PORTMAP, PLUGINS, "installed")
if result is None:
    pass
elif result:
    ok("every installed module has been measured")

print("\nall good" if bad == 0 else "\nFAILED")
sys.exit(1 if bad else 0)
