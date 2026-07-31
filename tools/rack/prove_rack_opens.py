#!/usr/bin/env python3
"""Does Rack open our patches, with exactly the modules they name?

    prove_rack_opens.py [patch.vcv ...]      (default: every example patch)

Everything else in this repo checks a patch against our own manifests, which
is a check of one thing we wrote against another thing we wrote. This asks
RACK. It opens each patch in the real, installed Rack and reads back the
"Creating module <name>" lines from Rack's own log -- so the answer to "are
these the same modules with the same names as the ones Forge Modular showed
you" comes from the program that has to create them, not from us.

That distinction is not academic. The preview draws from the patch and from
our manifests; Rack can only create what its installed plugin BINARY contains.
On a machine running an older build those differ, and a patch renders
perfectly here and opens over there as a rack with modules silently missing.

Headless (`-h`), against a throwaway user directory whose plugins are a
symlink to the real ones, so a run cannot touch anyone's autosave, settings or
window. Rack exits on its own at stdin EOF, about a second in.

NOTE: headless Rack still opens a CoreAudio device for ~1s. It is brief and
nothing is patched to an output by default, but it is not silent-by
construction, so this is not something to run beside someone who is listening.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
PATCHES = REPO / "examples" / "forge-modular" / "patches"
# Overridable so the checker's own comparison can be tested against a stub
# that reports a known set of modules, on machines with no Rack at all.
RACK = Path(os.environ.get(
    "PROVE_RACK_BIN", "/Applications/VCV Rack 2 Free.app/Contents/MacOS/Rack"))
USER_DIR = Path.home() / "Library" / "Application Support" / "Rack2"
BRAND = "Forge"


def installed_plugin_dir():
    """Where Rack keeps the plugin it would actually load."""
    for name in ("plugins-mac-arm64", "plugins-mac-x64", "plugins"):
        d = USER_DIR / name
        if (d / "ForgeModular" / "plugin.json").exists():
            return d
    return None


def module_names(plugin_dir):
    """slug -> the name Rack logs, which is brand-prefixed."""
    manifest = json.load(open(plugin_dir / "ForgeModular" / "plugin.json"))
    return {m["slug"]: f"{BRAND} {m.get('name', m['slug'])}" for m in manifest["modules"]}


def rack_creates(patch, plugin_dir):
    """Open `patch` in a real Rack and return what Rack says it created.

    Rack is left to exit by itself. An earlier version of this sent it a
    newline and polled its log, and Rack sometimes exited MID-LOAD -- a patch
    whose modules had not been created yet read as a patch missing its
    modules, which is the exact failure this script exists to detect. A probe
    that manufactures the fault it is looking for is worse than no probe.
    """
    with tempfile.TemporaryDirectory(prefix="rack-open-") as tmp:
        os.symlink(plugin_dir, Path(tmp) / plugin_dir.name)
        subprocess.run([str(RACK), "-h", "-u", tmp, str(Path(patch).resolve())],
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=180)
        log = (Path(tmp) / "log.txt").read_text(errors="replace")
    if "Loaded plugin ForgeModular" not in log:
        raise RuntimeError("Rack never loaded the ForgeModular plugin — this run "
                           "proves nothing about the patch")
    created = Counter(
        line.split("Creating module ", 1)[1].strip()
        for line in log.splitlines()
        if "Creating module " in line and "Creating module widget" not in line)
    return Counter({k: v for k, v in created.items() if k.startswith(BRAND + " ")}), log


def main(argv):
    if not RACK.exists():
        print(f"Rack is not installed at {RACK} — cannot ask it anything.")
        return 3            # not a pass, and not a failure of the patches
    plugin_dir = installed_plugin_dir()
    if plugin_dir is None:
        print("no ForgeModular plugin is installed in Rack — nothing to compare")
        return 3
    names = module_names(plugin_dir)

    patches = [Path(a) for a in argv] or sorted(PATCHES.glob("*.vcv"))
    if not patches:
        print("no patches to check")
        return 3

    bad = []
    for patch in patches:
        doc = json.load(open(patch))
        want = Counter(names[m["model"]] for m in doc["modules"]
                       if m["plugin"] == "ForgeModular" and m["model"] in names)
        unknown = [m["model"] for m in doc["modules"]
                   if m["plugin"] == "ForgeModular" and m["model"] not in names]
        # Answered WITHOUT starting Rack, deliberately. Rack blocks on a modal
        # error dialog -- even headless -- when a patch names a module its
        # installed plugin does not have, so asking it would hang this checker
        # on the exact fault the checker exists to report. Confirmed against
        # Rack 2.6.6: the stack sits in osdialog_message under
        # patch::Manager::load and never returns.
        if unknown:
            print(f"{patch.name:24} {sum(want.values()):2} modules  MISMATCH")
            print(f"    the installed plugin has no such model: {unknown}")
            print("    not opened: Rack would block on an error dialog")
            bad.append(patch.name)
            continue

        try:
            got, _ = rack_creates(patch, plugin_dir)
        except subprocess.TimeoutExpired:
            print(f"{patch.name:24} RACK NEVER FINISHED — it is most likely "
                  "waiting on a dialog nobody can click")
            bad.append(patch.name)
            continue
        except Exception as exc:                       # noqa: BLE001 — reported, not hidden
            print(f"{patch.name:24} COULD NOT ASK RACK: {exc}")
            bad.append(patch.name)
            continue

        ok = want == got
        print(f"{patch.name:24} {sum(want.values()):2} modules  "
              f"{'ok' if ok else 'MISMATCH'}")
        if want - got:
            print(f"    the patch asks for these and Rack did not create them: "
                  f"{dict(want - got)}")
        if got - want:
            print(f"    Rack created these and the patch never asked: "
                  f"{dict(got - want)}")
        if not ok:
            bad.append(patch.name)

    print()
    if bad:
        print(f"FAILED — {len(bad)} of {len(patches)}: {', '.join(bad)}")
        return 1
    print(f"all {len(patches)} patches open in Rack {rack_version()} with exactly "
          f"the modules they name")
    return 0


def rack_version():
    try:
        info = REPO and subprocess.run(
            ["plutil", "-extract", "CFBundleVersion", "raw",
             str(RACK.parent.parent / "Info.plist")],
            capture_output=True, text=True, timeout=20)
        return info.stdout.strip() or "?"
    except Exception:                                  # noqa: BLE001
        return "?"


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
