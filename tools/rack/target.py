#!/usr/bin/env python3
"""Work out where a generated module or patch should be sent.

Forge Modular exists to put things into VCV Rack, so the question it has to
answer constantly is *which* Rack. There may be a standalone Rack installed,
running or not; there may be a Rack Pro plugin installed in one or more
formats; there may be a DAW open with Rack Pro loaded, or open without it.

The rule is that **context decides**. Running as a plugin inside a DAW, the
user is in that DAW and the answer is the Rack Pro instance beside us.
Running standalone, the answer is standalone Rack. Everything else is
fallback, and where nothing can be found we say so rather than guessing.

One asymmetry is not going away and shapes the whole design: **standalone
Rack can be launched and handed a patch file; a Rack Pro plugin instance
cannot.** No plugin can instantiate another plugin in its host, and no plugin
can tell its host to open a file. So from inside a DAW we can prepare and
report, but the last step is the user's.

    target.py            # what this machine can reach
    target.py --json
"""
from __future__ import annotations

import json
import os
import subprocess
import sys

RACK_USER_DIR = os.path.expanduser("~/Library/Application Support/Rack2")
PLUGIN_DIRS_RACK = [os.path.join(RACK_USER_DIR, d)
                    for d in (os.listdir(RACK_USER_DIR)
                              if os.path.isdir(RACK_USER_DIR) else [])
                    if d.startswith("plugins-")]

APPS = ["/Applications/VCV Rack 2 Free.app",
        "/Applications/VCV Rack 2 Pro.app"]

# Where each format's plugins live on macOS. Rack Pro installs as all four.
PLUGIN_PATHS = {
    "au": os.path.expanduser("~/Library/Audio/Plug-Ins/Components"),
    "vst3": os.path.expanduser("~/Library/Audio/Plug-Ins/VST3"),
    "vst2": os.path.expanduser("~/Library/Audio/Plug-Ins/VST"),
    "clap": os.path.expanduser("~/Library/Audio/Plug-Ins/CLAP"),
}
SYSTEM_PLUGIN_PATHS = {
    "au": "/Library/Audio/Plug-Ins/Components",
    "vst3": "/Library/Audio/Plug-Ins/VST3",
    "vst2": "/Library/Audio/Plug-Ins/VST",
    "clap": "/Library/Audio/Plug-Ins/CLAP",
}

# Process names of the DAWs Pulp already carries host quirks for. Used only to
# notice that one is open; the plugin knows its own host far more reliably.
DAWS = {
    "Logic Pro": "Logic Pro", "Ableton Live": "Live", "REAPER": "REAPER",
    "Bitwig Studio": "Bitwig", "Cubase": "Cubase", "Studio One": "Studio One",
    "FL Studio": "FL Studio", "Pro Tools": "Pro Tools",
    "Digital Performer": "Digital Performer", "Ardour": "Ardour",
}


def _running(name: str) -> bool:
    try:
        return subprocess.run(["pgrep", "-x", name], capture_output=True,
                              timeout=10).returncode == 0
    except Exception:
        return False


def detect(in_plugin: bool = False, host: str | None = None) -> dict:
    """Everything we can learn about where Rack is on this machine."""
    apps = [p for p in APPS if os.path.isdir(p)]
    standalone_running = any(
        _running(os.path.basename(p)[:-4]) or _running("Rack") for p in apps)

    formats = {}
    for fmt, d in PLUGIN_PATHS.items():
        found = []
        for base in (d, SYSTEM_PLUGIN_PATHS[fmt]):
            if not os.path.isdir(base):
                continue
            for entry in os.listdir(base):
                if "vcv" in entry.lower() or "rack" in entry.lower():
                    found.append(os.path.join(base, entry))
        if found:
            formats[fmt] = found

    daws_open = [label for proc, label in DAWS.items() if _running(proc)]

    return {
        "standalone_installed": apps,
        "standalone_running": standalone_running,
        "plugin_formats": formats,
        "daws_open": daws_open,
        "in_plugin": in_plugin,
        "host": host,
    }


def holders(plugin_dirs=None) -> list[dict]:
    """Which processes currently have our installed plugin open.

    Rack maps a plugin's library when it loads it and keeps that mapping.
    Replacing the file underneath a running Rack is safe -- the old inode
    survives -- but the running Rack keeps showing the OLD module, so a
    regeneration appears to do nothing at all. That is the actual hazard, and
    it is invisible without asking.

    Answers for a Rack Pro instance inside a DAW as readily as a standalone
    one, because both are processes holding the file.
    """
    import glob
    if plugin_dirs is None:
        plugin_dirs = PLUGIN_DIRS_RACK
    files = []
    for d in plugin_dirs:
        files += glob.glob(os.path.join(d, "*.vcvplugin"))
        files += glob.glob(os.path.join(d, "*", "plugin.dylib"))
    if not files:
        return []
    try:
        out = subprocess.run(["lsof", "-F", "cn", *files], capture_output=True,
                             text=True, timeout=30).stdout
    except Exception:
        return []
    found, cmd = [], None
    for line in out.splitlines():
        if line.startswith("c"):
            cmd = line[1:]
        elif line.startswith("n") and cmd:
            found.append({"process": cmd, "file": line[1:]})
    # One entry per process; a Rack maps several of our files at once.
    seen, uniq = set(), []
    for f in found:
        if f["process"] in seen:
            continue
        seen.add(f["process"])
        uniq.append(f)
    return uniq


def choose(env: dict) -> dict:
    """Pick a target, and say plainly what can and cannot be done with it.

    `can_launch` is the honest part. Standalone Rack can be started and given
    a patch path. A Rack Pro plugin instance can be neither started nor handed
    a file by us -- so for that target we prepare the artifact, report where it
    is, and the user takes it from there.
    """
    # Context first. Inside a DAW the user is in that DAW, and sending them to
    # a separate standalone Rack would be answering a question they did not ask.
    if env["in_plugin"]:
        if env["plugin_formats"]:
            return {
                "target": "rack-plugin",
                "where": env.get("host") or "this DAW",
                "can_launch": False,
                "why": "a plugin cannot instantiate another plugin in its host, "
                       "so the Rack Pro instance has to be added by the user",
                "formats": sorted(env["plugin_formats"]),
            }
        if env["standalone_installed"]:
            return {
                "target": "rack-standalone",
                "where": env["standalone_installed"][0],
                "can_launch": True,
                "why": "Rack Pro is not installed as a plugin, so the standalone "
                       "is the only Rack available",
            }
        return {"target": None, "why": "VCV Rack is not installed"}

    # Standalone Forge Modular: standalone Rack is the natural pair.
    if env["standalone_installed"]:
        return {
            "target": "rack-standalone",
            "where": env["standalone_installed"][0],
            "can_launch": True,
            "already_running": env["standalone_running"],
            "why": "standalone Forge Modular pairs with standalone Rack",
        }
    if env["plugin_formats"]:
        return {
            "target": "rack-plugin",
            "where": env["daws_open"][0] if env["daws_open"] else "a DAW",
            "can_launch": False,
            "why": "only the Rack Pro plugin is installed; open it in a DAW",
            "formats": sorted(env["plugin_formats"]),
        }
    return {"target": None, "why": "VCV Rack is not installed"}


def main(argv):
    in_plugin = "--in-plugin" in argv
    host = None
    if "--host" in argv:
        host = argv[argv.index("--host") + 1]
    env = detect(in_plugin, host)
    env["holders"] = holders()
    pick = choose(env)
    if "--json" in argv:
        print(json.dumps({"environment": env, "target": pick}, indent=2))
        return 0

    print("environment")
    print(f"  standalone installed : {env['standalone_installed'] or 'no'}")
    print(f"  standalone running   : {env['standalone_running']}")
    print(f"  plugin formats       : {sorted(env['plugin_formats']) or 'none'}")
    print(f"  DAWs open            : {env['daws_open'] or 'none'}")
    h = env.get("holders") or []
    print(f"  has our plugin open  : "
          f"{', '.join(x['process'] for x in h) if h else 'nothing'}")
    print(f"  we are               : {'a plugin in ' + (host or 'a DAW') if in_plugin else 'the standalone app'}")
    print()
    print("target")
    if not pick.get("target"):
        print(f"  none — {pick['why']}")
        return 1
    print(f"  {pick['target']} ({pick['where']})")
    print(f"  can we launch it: {'yes' if pick.get('can_launch') else 'no'}")
    print(f"  {pick['why']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
