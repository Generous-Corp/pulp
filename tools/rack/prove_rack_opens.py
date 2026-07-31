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


class RackDidNotStart(RuntimeError):
    """Rack aborted before it got as far as the patch.

    Kept apart from every other failure because it is not evidence about any
    patch: it is Rack failing to start. Blaming the patch for it is exactly
    the mistake this checker exists to prevent, in the other direction.
    """


def console_owner():
    """Who owns the window session, or "root" when nobody is logged in.

    Overridable so the refusal below can be tested without logging anyone out.
    """
    override = os.environ.get("PROVE_RACK_CONSOLE_USER")
    if override is not None:
        return override
    r = subprocess.run(["stat", "-f", "%Su", "/dev/console"],
                       capture_output=True, text=True, timeout=20)
    return r.stdout.strip()


def can_start_rack():
    """Can Rack even start here?

    Rack initialises MIDI before it loads any plugin, and RtMidi's CoreMIDI
    backend THROWS when MIDIClientCreate fails -- which is what happens with no
    window session. Rack does not catch it, so the process calls terminate and
    aborts in rtmidiInit, having loaded nothing.

    Over SSH to a machine sitting at the login window that is every launch. The
    checker was handing Rack patch after patch and collecting a crash report
    for each one, while the answer -- "Rack cannot run here" -- had nothing to
    do with any patch. Ask first; do not find out by crashing it.

    A LOCKED screen is fine: someone is logged in, the session exists, and
    Rack starts normally. The fatal case is nobody logged in at all.
    """
    return console_owner() not in ("", "root")


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
        r = subprocess.run([str(RACK), "-h", "-u", tmp, str(Path(patch).resolve())],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=180)
        log = (Path(tmp) / "log.txt").read_text(errors="replace")
        if os.environ.get("PROVE_RACK_KEEP_LOG"):
            Path(os.environ["PROVE_RACK_KEEP_LOG"],
                 Path(patch).stem + ".log").write_text(log)
    if "Loaded plugin ForgeModular" not in log:
        # Rack initialises MIDI BEFORE plugins, and RtMidi's CoreMIDI backend
        # throws when MIDIClientCreate is declined -- which happens sporadically
        # when many Racks are started back to back. Rack does not catch it, so
        # the process aborts having loaded nothing. Distinguished from every
        # other failure because it says nothing whatever about the patch, and
        # calling it a patch fault is how a sweep blamed sixmix for a crash.
        if r.returncode != 0 and "Initializing plugins" not in log:
            raise RackDidNotStart(
                "Rack aborted during startup (CoreMIDI client creation is the "
                f"usual cause; exit {r.returncode}) — it never reached the patch")
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
    if not can_start_rack():
        print("no window session on this machine (console owner: "
              f"{console_owner() or 'nobody'}) — Rack aborts in MIDI init "
              "before loading anything, so nothing here can be checked.\n"
              "This is a skip, not a pass, and not a fault of the patches.")
        return 3
    plugin_dir = installed_plugin_dir()
    if plugin_dir is None:
        print("no ForgeModular plugin is installed in Rack — nothing to compare")
        return 3
    names = module_names(plugin_dir)

    patches = [Path(a) for a in argv] or sorted(PATCHES.glob("*.vcv"))
    if not patches:
        print("no patches to check")
        return 3

    bad, flaky = [], []
    for patch in patches:
        doc = json.load(open(patch))
        want = Counter(names[m["model"]] for m in doc["modules"]
                       if m["plugin"] == "ForgeModular" and m["model"] in names)
        unknown = [m["model"] for m in doc["modules"]
                   if m["plugin"] == "ForgeModular" and m["model"] not in names]
        ids = [m.get("id") for m in doc["modules"]]
        duplicate_ids = len(set(ids)) != len(ids)
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

        # Also answered without starting Rack. Two entries sharing an id give
        # both widgets the one module, and the second ModuleWidget destructor
        # calls Engine::removeModule for a module already removed -- the
        # assertion in removeModule_NoLock aborts Rack on teardown. Opening
        # such a patch to find out crashes the program we are asking.
        if duplicate_ids:
            print(f"{patch.name:24} {sum(want.values()):2} modules  MISMATCH")
            print("    duplicate module ids — Rack would abort on teardown")
            print("    not opened")
            bad.append(patch.name)
            continue

        # Retried ONCE, loudly, on a mismatch.
        #
        # Launching eighteen Racks back to back is not the same as launching
        # one: a sweep called sixmix a mismatch that passes three times out of
        # three on its own, and two later sweeps passed all eighteen. The cause
        # is not understood, so this does not paper over it -- a patch that
        # only passes on the retry is REPORTED as having flaked, and one that
        # fails twice is a failure. A silent retry would turn an intermittent
        # checker into a checker that looks reliable.
        def ask_rack():
            got, _ = rack_creates(patch, plugin_dir)
            return got

        try:
            try:
                got = ask_rack()
            except RackDidNotStart as exc:
                print(f"{patch.name:24} Rack did not start — retrying ({exc})")
                flaky.append(patch.name)
                got = ask_rack()
            if got != want:
                second = ask_rack()
                if second == want:
                    print(f"{patch.name:24} {sum(want.values()):2} modules  "
                          "ok (FLAKED once — first launch disagreed)")
                    flaky.append(patch.name)
                    continue
                got = second
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
    if flaky:
        print(f"NOTE — {len(flaky)} passed only on a second launch: "
              f"{', '.join(flaky)}")
        print("      Rack disagreed with itself; the cause is not understood.")
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
