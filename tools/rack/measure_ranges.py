#!/usr/bin/env python3
"""Measure a vendor's parameter ranges without anybody opening Rack by hand.

CARTOG reads `ParamQuantity::minValue/maxValue/defaultValue` off the widgets
Rack has instantiated, so a module's ranges exist only once that module has been
PLACED on the canvas. That has meant the ranges were, in practice, never
measured: they needed a person to open Rack, drag in the modules they cared
about, and know that doing so was the thing that unblocked the model.

Nothing about that needs a person. Rack opens a patch headlessly, and CARTOG
scans as soon as it joins the rack. So: write a patch containing the modules to
measure and CARTOG, open it headless, and read the map back.

    measure_ranges.py CVfunk                  # every module of one plugin
    measure_ranges.py CVfunk Ouros Node       # only these
    measure_ranges.py --all                   # every installed plugin

CARTOG GOES LAST. `onAdd` fires per module as RackWidget takes it, in the order
the patch lists them, so a scanner named first measures only itself. That is not
a guess -- placed first it logs "CARTOG placed alongside 1 modules" and writes a
map with none of its subjects in it; placed last, alongside all of them.

Each launch gets a THROWAWAY Rack user directory, with the module library
symlinked in. Rack asks "did I crash last time?" from state in that directory,
and every automated run has to be killed -- so against the real one the answer
becomes permanently yes and the launch blocks on an alert nobody can answer.
See `make_scratch`. The user's autosave, log and open rack are left alone.

AUDIO: headless Rack still opens an audio device for about a second. This
places no oscillator and connects no cables, so it is silence -- but it is not
nothing, and on a shared or occupied machine it should be announced rather than
sprung. Rack is stopped as soon as the scan lands rather than left to idle.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from typing import NamedTuple

import crash_watch
import param_units

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

# Written by CARTOG's scan, and the only proof from inside Rack that the scan
# ran at all. A run that measured nothing AND never logged these did not find
# an empty library -- it failed to scan, which looks identical from the map.
SCANNED = "forge: CARTOG placed alongside"
WROTE = "forge: wrote port map to"


def rack_binary() -> str | None:
    override = os.environ.get("FORGE_RACK_BIN")
    if override:
        return override if os.path.exists(override) else None
    for p in RACK_CANDIDATES:
        if os.path.exists(p):
            return p
    return None


def plugin_roots() -> list[str]:
    """Where Rack keeps installed plugins, whichever arch this Mac is."""
    return [p for p in (os.path.join(RACK_USER_DIR, "plugins-mac-arm64"),
                        os.path.join(RACK_USER_DIR, "plugins-mac-x64"))
            if os.path.isdir(p)]


def installed_plugins() -> list[str]:
    """Every plugin installed, across both architectures' directories."""
    out: set[str] = set()
    for root in plugin_roots():
        out.update(d for d in os.listdir(root)
                   if os.path.isdir(os.path.join(root, d)))
    return sorted(out)


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
    """A patch that is only the subjects and their scanner. No cables, no sound.

    The subjects come FIRST and CARTOG last, because CARTOG scans the rack it
    finds when it is added and Rack adds modules in the order the patch lists
    them. Named first, it measures an empty rack.
    """
    mods = []
    for i, model in enumerate(models):
        # A generous stride, because Rack shuffles modules that would overlap
        # and the widest panels in a library run past 30HP. Nothing depends on
        # where a module lands -- a panel's controls are measured in its own
        # coordinates -- so the layout only has to stay out of its own way.
        mods.append({"id": i, "plugin": plugin, "model": model,
                     "pos": [(i % 8) * 40, i // 8]})
    mods.append(dict(SCANNER, id=len(models), pos=[(len(models) % 8) * 40,
                                                   len(models) // 8]))
    with open(path, "w") as f:
        json.dump({"version": "2.6.6", "modules": mods, "cables": []}, f)
    return len(mods)


def ranges_by_model(portmap: dict, plugin: str) -> dict[str, tuple[int, int]]:
    """Per model of `plugin`: (params recorded, params carrying ranges).

    `modules` is a LIST of entries carrying their own plugin/model, not a dict
    keyed by slug. Reading it as a dict returns nothing for every plugin, which
    looks exactly like "the scan measured nothing".
    """
    out: dict[str, tuple[int, int]] = {}
    for entry in (portmap.get("modules") or []):
        if entry.get("plugin") != plugin:
            continue
        model = entry.get("model")
        if not model:
            continue
        params = entry.get("params") or []
        out[model] = (len(params),
                      sum(1 for p in params if "minValue" in p))
    return out


def shortfall(portmap: dict, plugin: str,
              models: list[str]) -> tuple[list[str], list[str]]:
    """(models with no entry at all, models whose params carry no ranges).

    A module with no params is neither: a blank panel has nothing to measure
    and holding it against the run would make a correct scan look broken.
    """
    seen = ranges_by_model(portmap, plugin)
    missing = [m for m in models if m not in seen]
    unranged = [m for m in models
                if m in seen and seen[m][0] > 0 and seen[m][1] == 0]
    return missing, unranged


def stale_scans(portmap: dict, plugin: str, models: list[str]) -> list[str]:
    """Target models whose entry was written by an older scanner than the map's.

    A launch that aborts leaves its subjects exactly as they were, so a sweep
    can finish looking complete while some modules still carry an entry from a
    scanner that did not measure what this one does. Nothing else notices: the
    entry is present, it parses, and it is simply less than it should be.

    The bar is the newest scan version anywhere in the map rather than a
    constant, so this cannot drift out of step with CARTOG's `kScanVersion`
    the next time that moves -- a duplicated constant would go stale in the
    one file whose job is noticing staleness.
    """
    entries = {e.get("model"): e for e in (portmap.get("modules") or [])
               if e.get("plugin") == plugin}
    versions = [e.get("scan") for e in (portmap.get("modules") or [])
                if isinstance(e.get("scan"), int)]
    if not versions:
        return []
    newest = max(versions)
    return [m for m in models
            if m in entries and isinstance(entries[m].get("scan"), int)
            and entries[m]["scan"] < newest]


def measured(portmap: dict, plugin: str) -> tuple[int, int]:
    """(modules with params, modules whose params carry ranges)."""
    seen = ranges_by_model(portmap, plugin)
    with_params = sum(1 for n, _ in seen.values() if n)
    with_ranges = sum(1 for n, r in seen.values() if n and r)
    return with_params, with_ranges


def with_units(portmap: dict) -> tuple[int, int]:
    """(modules a units-aware scanner measured, modules that carry a unit).

    The two are different and the gap is not a fault. A vendor is free to leave
    every `configParam` unitless, and a scan that says so is a measurement --
    so the sweep reports both numbers rather than the second alone, which on
    its own reads as a rescan that half worked.
    """
    known = carrying = 0
    for entry in (portmap.get("modules") or []):
        if not (entry.get("params") or []):
            continue
        if not param_units.knows_units(entry):
            continue
        known += 1
        if any("unit" in p for p in entry["params"]):
            carrying += 1
    return known, carrying


def read_portmap() -> dict:
    if not os.path.exists(PORTMAP):
        return {}
    try:
        with open(PORTMAP) as f:
            return json.load(f)
    except Exception:                                       # noqa: BLE001
        return {}


def read_log(user_dir: str) -> str:
    path = os.path.join(user_dir, "log.txt")
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def make_scratch() -> str:
    """A throwaway Rack user directory, fresh for every launch.

    Rack remembers, somewhere in its user directory, that the last session
    ended badly, and the next launch then blocks on "VCV Rack crashed during
    the last session ... Clear your patch and start over?" -- an alert nothing
    headless can answer. Every automated run ends badly by construction,
    because headless Rack has no way to exit on its own and must be killed, so
    against the real user directory the failures accumulate until every launch
    wedges: measured on a working machine, 2 of 5 launches loaded the patch,
    then 0 of 8.

    A directory Rack has never seen has no last session to have crashed in.
    Same machine, same patch, same binary: 5 of 5.

    It is also the better citizen. The scan no longer overwrites the user's
    autosave, their `log.txt`, or the rack they had open.
    """
    scratch = tempfile.mkdtemp(prefix="rack-measure-")
    # The library is symlinked rather than copied: it is the whole point of
    # the run and it is gigabytes.
    for arch in ("plugins-mac-arm64", "plugins-mac-x64"):
        real = os.path.join(RACK_USER_DIR, arch)
        if os.path.isdir(real):
            os.symlink(real, os.path.join(scratch, arch))
    # Settings carry the Pro token and the licenses save a round trip, so a
    # scratch launch is licensed exactly as the real one is.
    for name in ("settings.json", "licenses"):
        src = os.path.join(RACK_USER_DIR, name)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(scratch, name))
        elif os.path.exists(src):
            shutil.copy2(src, os.path.join(scratch, name))
    # The map goes in so CARTOG merges into the library's history rather than
    # writing a map holding only this run.
    if os.path.exists(PORTMAP):
        shutil.copy2(PORTMAP, os.path.join(scratch, "forge-portmap.json"))
    return scratch


def drop_scratch(scratch: str) -> None:
    """Remove the scratch directory, and never the library it points at."""
    for arch in ("plugins-mac-arm64", "plugins-mac-x64"):
        link = os.path.join(scratch, arch)
        if os.path.islink(link):
            os.unlink(link)              # unlink the link, not what it names
    shutil.rmtree(scratch, ignore_errors=True)


LOADING = "Loading patch"

# How long a launch gets to reach "Loading patch" before it is written off as
# wedged, and how many launches a measurement gets in total. Measured on a
# working machine a load starts inside a second; 20s is slack, not a wait.
LOAD_WINDOW = 20.0
ATTEMPTS = 8

# Rack died inside its MIDI init, before any patch was parsed. Matched on the
# frames Rack prints for it rather than on "the launch failed", so this cannot
# claim the mechanism for an unrelated death.
ABORT_MARKS = ("rtmidiInit", "RtMidiDriver", "MidiInCore")

# A sporadic abort is worth another launch; a machine where CoreMIDI is
# genuinely unavailable is not worth eight, because each one is a crash report
# on somebody's desk. Measured rate is well under one percent, so three
# attempts leaves the odds of a real failure being called sporadic negligible.
ABORT_ATTEMPTS = 3


def rack_argv(rack: str, user_dir: str, patch: str) -> list[str]:
    """Rack's command line: a plain exec, deliberately.

    The obvious-looking fix for the CoreMIDI aborts below is to launch through
    `launchctl asuser <uid>`, so the process joins the user's GUI login session
    and CoreMIDI has a bootstrap namespace to find `MIDIServer` in. Measured,
    it is wrong twice:

      * `launchctl asuser` needs root. Without it the command is never executed
        at all -- it fails with "Could not switch to audit session … Operation
        not permitted", and `launchctl asuser $(id -u) /bin/echo HELLO` prints
        nothing. Wrapping every SSH launch in it would break the one case it
        was added for, and 50 of 50 wrapped probes failed where 0 of 50
        unwrapped ones did.
      * The session is not the variable anyway. A bare `MIDIClientCreate`
        probe -- the same call Rack aborts inside -- fails at well under one
        percent from a GUI session (1 of 400) and not at all over SSH with no
        `SECURITYSESSIONID` (0 of 50). The aborts are sporadic, not structural.

    So: a plain exec, which is also a line a person can copy out of a log and
    run by hand. The retry is the whole remedy.
    """
    return [rack, "-h", "-u", user_dir, patch]


SKIP_FILE = os.path.expanduser(
    "~/Library/Application Support/Forge Modular/rack-scan-skip.json")


def load_skips() -> dict:
    try:
        with open(SKIP_FILE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def is_skipped(skips: dict, plugin: str, model: str) -> bool:
    return f"{plugin}/{model}" in skips


def record_skip(plugin: str, model: str, widget: str, why: str) -> None:
    """Write down a module that takes Rack with it, so nobody meets it twice.

    The list builds ITSELF. `crash_watch` already reads the faulting stack, so
    the widget that did it is on hand at the moment we learn -- and a skip list
    maintained by hand is one that goes stale the first time somebody is in a
    hurry.

    Written ONLY where a crash report names the death (see `measure_batch`). A
    skip list that also collected launches which merely failed would fill up
    with the machine's bad afternoons and quietly shrink the library forever
    after, which is worse than no list: a module that crashes is a fact about
    the module, a launch that wedged is a fact about the moment.
    """
    skips = load_skips()
    skips[f"{plugin}/{model}"] = {
        "widget": widget, "why": why,
        "recorded": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    os.makedirs(os.path.dirname(SKIP_FILE), exist_ok=True)
    tmp = SKIP_FILE + ".part"
    with open(tmp, "w") as f:
        json.dump(skips, f, indent=1, sort_keys=True)
    os.replace(tmp, SKIP_FILE)


def crashing_widget(crashes: list) -> str:
    """The vendor widget named in a crash stack, if one is.

    A constructor that loads a font is the shape this keeps taking -- headless
    Rack has no font context -- so the frame that names a `*Widget` is the
    useful half of the report and the rest is Rack's own unwinding.
    """
    for c in crashes:
        for frame in c.frames:
            if "Widget" in frame and "rack::" not in frame:
                return frame.split("::")[-1][:60]
    return ""


class Batch(NamedTuple):
    """What a batch of models came to, once every launch it needed was spent."""
    #: CARTOG saw and measured these
    measured: list[str]
    #: a crash report names these; written to the skip list, never met again
    skipped: list[str]
    #: these failed with no crash report to blame them; reported, not persisted
    failed: list[str]

    def __add__(self, other: "Batch") -> "Batch":
        return Batch(self.measured + other.measured,
                     self.skipped + other.skipped,
                     self.failed + other.failed)


# The biggest patch a single launch is asked to carry.
#
# Bisection already turns one bad widget into one lost module rather than a
# lost vendor, so this is not what makes the sweep safe -- it is what makes it
# cheap. A vendor entered whole costs log2(N) doubled launches to isolate a
# crasher, and this library's worst plugin ships 148 modules; entering in
# chunks bounds that, and bounds how much a launch that wedges for reasons of
# its own has to be redone. Measured launch cost is a few seconds either way,
# so the chunking is close to free on a clean vendor.
MAX_BATCH = int(os.environ.get("FORGE_RACK_MAX_BATCH", "32"))


def measure_batch(rack: str, plugin: str, models: list[str],
                  scan_window: float) -> Batch:
    """Measure these models, halving the batch when one of them kills Rack.

    A whole vendor in one patch means one bad widget loses the vendor: a
    hundred-module plugin scans nothing because its third module's constructor
    called `Window::loadFont` with no font context and took the process down.
    That is not hypothetical -- it happened sixteen times in one sweep, three
    widgets between them. Bisecting costs a few extra launches and turns each
    of those into one lost module.

    Success is CARTOG reporting it saw ALL of them, not merely that it ran. A
    launch where Rack silently declined one module scans the rest and looks
    clean, and the module it dropped would otherwise be recorded as measured
    while carrying whatever an older scanner left.

    A crash we can prove predates the patch -- the CoreMIDI abort -- is already
    retried inside `run_rack`, so a crash report reaching here at batch size
    one names this module. A failure with NO crash report does not: it is the
    machine's, and it is reported rather than written down.
    """
    if not models:
        return Batch([], [], [])
    if len(models) > MAX_BATCH:
        mid = len(models) // 2
        return (measure_batch(rack, plugin, models[:mid], scan_window)
                + measure_batch(rack, plugin, models[mid:], scan_window))

    mark = crash_watch.now()
    tmp = tempfile.mkdtemp(prefix="rack-patch-")
    try:
        patch = os.path.join(tmp, "measure.vcv")
        want = write_patch(patch, plugin, models)
        log, gave_up = run_rack(rack, patch, scan_window)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    saw = scanned_alongside(log)
    if not gave_up and saw is not None and saw >= want:
        return Batch(list(models), [], [])

    if len(models) == 1:
        why = gave_up or (f"CARTOG saw {saw} of {want} modules"
                          if saw is not None else "CARTOG never scanned")
        crashes = [c for c in crash_watch.since(mark) if not c.retryable]
        if not crashes:
            # Nothing crashed, so nothing here is evidence about this module.
            print(f"    {plugin}/{models[0]}: not measured — {why}", flush=True)
            return Batch([], [], [models[0]])
        widget = crashing_widget(crashes)
        record_skip(plugin, models[0], widget, why)
        print(f"    {plugin}/{models[0]}: skipped"
              + (f" — {widget} took Rack down" if widget else f" — {why}"),
              flush=True)
        return Batch([], [models[0]], [])

    mid = len(models) // 2
    print(f"    {plugin}: batch of {len(models)} did not measure "
          f"({gave_up or f'CARTOG saw {saw} of {want}'}) — halving", flush=True)
    return (measure_batch(rack, plugin, models[:mid], scan_window)
            + measure_batch(rack, plugin, models[mid:], scan_window))


def exit_verdict(log: str, crashes: list | None = None) -> str:
    """Why a launch that ended on its own did not measure anything.

    Reports what was seen and does not name a cause it cannot check. The
    CoreMIDI abort in particular has invited two confident wrong diagnoses --
    a missing GUI session, and client exhaustion from relaunching in a loop --
    and neither survived measurement, so this says the observable thing and
    leaves the reader with the skill's account rather than a false lead.

    A macOS crash report is the better witness where one exists: Rack's own
    log carries the stack only when its signal handler got to run, and the
    report is written either way. The log stays as the fallback so this keeps
    working on a machine with reporting turned off.
    """
    if crashes:
        if all(c.retryable for c in crashes):
            return "Rack aborted in CoreMIDI before it reached the patch"
        worst = next(c for c in crashes if not c.retryable)
        return f"Rack died before it scanned: {worst.summary}"
    if aborted_in_coremidi(log):
        return "Rack aborted in CoreMIDI before it reached the patch"
    return "Rack exited before it scanned"


def aborted_in_coremidi(log: str) -> bool:
    """Whether this launch died in Rack's MIDI init, on the evidence.

    Gated on the stack Rack actually prints for it -- `rtmidiInit`,
    `RtMidiDriver`, `MidiInCore` -- rather than on anything about the shell.
    A diagnosis that fires whenever a launch merely FAILED would claim this
    mechanism for every unrelated death, which is how a right diagnosis
    becomes a wrong one.
    """
    return any(mark in log for mark in ABORT_MARKS)


class Launch(NamedTuple):
    log: str
    why: str                 #: empty when the scan landed
    crashes: list
    died: bool               #: the process ended on its own, rather than wedging


# How long to wait for macOS to finish writing a crash report after the process
# it describes has gone.
#
# It is not instant, and a single short sleep gets the answer wrong in the
# expensive direction: with 0.6s, a module whose widget reliably killed Rack
# was classified as "exited before it scanned" three launches running before
# the report finally appeared, so every crasher cost four launches per bisect
# level instead of one -- and one vendor ran into the sweep's own timeout that
# way. Polling costs nothing on a launch that did not crash, because a report
# either exists shortly or never does.
CRASH_REPORT_WINDOW = float(os.environ.get("FORGE_CRASH_REPORT_WINDOW", "6"))


def await_crash_reports(mark: float,
                        window: float = CRASH_REPORT_WINDOW) -> list:
    """Crash reports written since `mark`, giving macOS time to write them."""
    deadline = time.time() + window
    while True:
        found = crash_watch.since(mark)
        if found or time.time() >= deadline:
            return found
        time.sleep(0.25)


def launch_once(rack: str, patch: str, scan_window: float) -> Launch:
    """One headless launch. Returns (log, "" | why it failed, crash reports).

    Waits on the scan rather than on a clock. Rack truncates its log at
    startup and it names the patch it was given, so a log mentioning THIS
    patch is this run's log and nobody's leftovers.

    Headless Rack does not reliably exit on its own, and a run left to hit a
    timeout is a Rack holding an audio device for the length of the timeout.
    Stopping it the moment the map is written keeps the device open for about
    a second, which is the whole of the audio this makes.

    On success the map the scratch Rack wrote replaces the real one. CARTOG
    merges rather than rewrites and the real map was seeded in, so this adds
    to the library's history and never truncates it.
    """
    scratch = make_scratch()
    # Marked before the launch so only THIS launch's crash reports are read.
    mark = crash_watch.now()
    proc = subprocess.Popen(rack_argv(rack, scratch, patch),
                            # stdin from /dev/null, or Rack's "Press enter to
                            # exit." waits on OUR terminal forever and leaves a
                            # live Rack behind every single run. Closed rather
                            # than inherited: with no stdin the read returns at
                            # once instead of blocking on a key nobody will
                            # press. The kill below is the belt to this brace.
                            stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    log = ""
    why = ""
    crashes: list = []
    died = False
    try:
        started = time.time()
        loading = False
        while True:
            log = read_log(scratch)
            mine = patch in log
            if mine and not loading and LOADING in log:
                loading = True
            if mine and WROTE in log:
                break
            if proc.poll() is not None:
                # Say what was seen, and only name the cause we can actually
                # check. A CoreMIDI abort OUTSIDE a GUI session is that
                # session's absence and no number of retries will fix it;
                # inside one it is something transient, and asserting the
                # session was missing when it demonstrably was not would be
                # the tool inventing a diagnosis -- which is how a reader ends
                # up debugging the wrong machine.
                # macOS writes the report a moment after the process dies, and
                # how long a moment is decides whether this launch is charged
                # to the module or to the machine.
                died = True
                crashes = await_crash_reports(mark)
                why = exit_verdict(log, crashes)
                break
            waited = time.time() - started
            if not loading and waited > LOAD_WINDOW:
                why = "Rack never got as far as loading the patch"
                break
            if waited > scan_window:
                why = "Rack loaded the patch but never finished scanning"
                break
            time.sleep(0.25)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
        measured_map = os.path.join(scratch, "forge-portmap.json")
        if not why and os.path.exists(measured_map):
            shutil.copy2(measured_map, PORTMAP)
        drop_scratch(scratch)
    return Launch(log, why, crashes, died)


def run_rack(rack: str, patch: str, scan_window: float,
             attempts: int = ATTEMPTS) -> tuple[str, str]:
    """Launch until one launch measures, or the attempts run out.

    The scratch user directory is what makes a launch reliable (see
    `make_scratch`); this is the backstop under it. A launch that wedges does
    so silently -- Rack blocks in an alert and its log simply stops -- so a
    launch is treated as cheap and disposable: if the log has not reached
    "Loading patch" in twenty seconds, kill it and launch again.

    A CoreMIDI abort is retried too -- it happens before any of our code is
    reachable, so it can carry no information about what we came to measure --
    but on a shorter leash of its own. Each one is a crash report on somebody's
    desk, so a machine where CoreMIDI is genuinely unavailable should cost
    three of them and not eight.

    What must NOT happen is a wedged launch reported as a library with no
    ranges in it, which is why every giving-up path names its reason and none
    of them returns quietly.
    """
    why = "no attempt was made"
    aborts = 0
    for attempt in range(1, attempts + 1):
        launch = launch_once(rack, patch, scan_window)
        log, why, crashes = launch.log, launch.why, launch.crashes
        if not why:
            return log, ""
        # An unrecognised crash is ours until proven otherwise, so it is fatal
        # on the first one. Only a crash we can PROVE predates patch loading
        # may be retried, and retrying that cannot hide a defect of ours
        # because none of our code has run yet.
        if launch.died and any(not c.retryable for c in crashes):
            return log, why
        if launch.died and (crashes or aborted_in_coremidi(log)):
            aborts += 1
            if aborts >= ABORT_ATTEMPTS:
                return log, (f"{aborts} launches died in CoreMIDI before "
                             f"reaching the patch")
        if attempt < attempts:
            print(f"    launch {attempt}: {why} — retrying", flush=True)
            time.sleep(2.0)
    return log, f"{attempts} launches, last: {why}"


def round_trip_faults(portmap: dict, tolerance: float = 1e-4) -> list[str]:
    """Params whose recorded conversion does not survive a round trip.

    A unit and three conversion numbers are only worth recording if a physical
    value placed through them comes back as itself. Nothing else in the sweep
    would notice if they did not: a scanner emitting a sign error, or the
    identity where a curve belonged, writes a map that parses, reports a module
    as measured, and lands "cutoff 40 Hz" somewhere else entirely.

    Checked at the control's own default, which is the one position every
    param is guaranteed to have and to be able to express.
    """
    faults = []
    for entry in (portmap.get("modules") or []):
        if not param_units.knows_units(entry):
            continue
        for p in (entry.get("params") or []):
            if "displayBase" not in p:
                continue                # identity; nothing was recorded to check
            raw = p.get("defaultValue")
            if not isinstance(raw, (int, float)):
                continue
            display = param_units.to_display(float(raw), p)
            if display is None:
                continue                # not expressible there, and says so
            back = param_units.from_display(display, p)
            if back is None or abs(back - float(raw)) > tolerance * max(
                    1.0, abs(float(raw))):
                faults.append(
                    f"{entry.get('plugin')}/{entry.get('model')} "
                    f"{p.get('name')!r}: {raw} -> {display:g}"
                    f"{p.get('unit','')} -> {back}")
    return faults


def scanned_alongside(log: str) -> int | None:
    """How many modules CARTOG saw, from Rack's own log. None if it never ran."""
    for line in reversed(log.splitlines()):
        if SCANNED in line:
            tail = line.split(SCANNED, 1)[1].strip().split()
            if tail and tail[0].isdigit():
                return int(tail[0])
    return None


def main(argv: list[str]) -> int:
    args = argv[1:]
    if not args:
        # By content, not by position: the docstring gets edited and a
        # usage message that silently becomes a paragraph of prose is a
        # small lie the next person has to notice.
        for para in (__doc__ or "").split("\n\n"):
            if "measure_ranges.py " in para:
                print(para.strip("\n"))
                break
        return 2

    if args[0] == "--verify":
        # Reads the map and launches nothing, so it is the one check that can
        # be run on a machine with no Rack and while a sweep is in flight.
        book = read_portmap()
        known, carrying = with_units(book)
        faults = round_trip_faults(book)
        print(f"modules measured by a units-aware scan: {known}   "
              f"carrying a unit: {carrying}")
        for f in faults[:20]:
            print(f"  does not round-trip: {f}")
        if faults:
            print(f"{len(faults)} params do not round-trip")
            return 1
        print("every recorded conversion round-trips")
        return 0

    rack = rack_binary()
    if not rack:
        print("no Rack found; set FORGE_RACK_BIN", file=sys.stderr)
        return 2

    if args[0] == "--all":
        plugins = installed_plugins()
    else:
        plugins = [args[0]]
    only = args[1:] if len(args) > 1 and args[0] != "--all" else []

    # How long a launch that HAS loaded the patch gets to finish scanning. A
    # big library is a lot of widgets to build; a wedged launch is caught by
    # the much shorter load window, not by this.
    scan_window = float(os.environ.get("FORGE_RACK_SCAN_WINDOW", "180"))
    total_before = total_after = 0
    failures: list[str] = []
    units_before = with_units(read_portmap())

    skips = load_skips()
    for plugin in plugins:
        models = only or installed_modules(plugin)
        if not models:
            print(f"{plugin}: no installed modules found — skipped")
            continue

        # A module already known to take Rack down is not met again. It is
        # named rather than silently dropped, or the library quietly shrinks
        # by whatever was crashing the day the list was written.
        known = [m for m in models if is_skipped(skips, plugin, m)]
        models = [m for m in models if m not in known]
        if known:
            print(f"{plugin}: skipping {len(known)} known to crash Rack: "
                  f"{', '.join(known)}")
        if not models:
            continue

        before = measured(read_portmap(), plugin)
        print(f"{plugin}: placing {len(models)} modules and the scanner "
              f"— headless Rack opens an audio device briefly", flush=True)
        batch = measure_batch(rack, plugin, models, scan_window)

        if batch.skipped:
            failures.append(f"{plugin}: crashed Rack and were written to the "
                            f"skip list: {', '.join(batch.skipped)}")
        if batch.failed:
            failures.append(f"{plugin}: did not measure: "
                            f"{', '.join(batch.failed)}")

        after = measured(read_portmap(), plugin)
        print(f"  params: {before[0]} -> {after[0]} modules   "
              f"ranges: {before[1]} -> {after[1]} modules   "
              f"(measured {len(batch.measured)} of {len(models)})")

        after_map = read_portmap()
        # Asked only of the ones a launch claimed to measure. A module that
        # crashed Rack has already been reported once, and reporting it again
        # as unmapped buries the reason it is unmapped under a second line
        # that does not say.
        missing, unranged = shortfall(after_map, plugin, batch.measured)
        stale = stale_scans(after_map, plugin, batch.measured)
        if stale:
            failures.append(f"{plugin}: left behind by an older scanner: "
                            f"{', '.join(stale)}")
        if missing:
            failures.append(f"{plugin}: never mapped: {', '.join(missing)}")
        if unranged:
            failures.append(f"{plugin}: mapped with no ranges: "
                            f"{', '.join(unranged)}")

        total_before += before[1]
        total_after += after[1]

    print(f"\nmodules carrying measured ranges: {total_before} -> {total_after}")
    units_after = with_units(read_portmap())
    print(f"modules measured by a units-aware scan: "
          f"{units_before[0]} -> {units_after[0]}   "
          f"carrying a unit: {units_before[1]} -> {units_after[1]}")

    # A conversion that does not invert is worse than one never recorded: it
    # places a physical value confidently and wrongly, and every later reader
    # trusts it. Checked over the whole map, because a merge carries entries
    # forward from scanners this run never touched.
    faults = round_trip_faults(read_portmap())
    if faults:
        failures.append(f"{len(faults)} recorded conversions do not "
                        f"round-trip, e.g. {faults[0]}")

    if failures:
        # The failure mode this exists to catch is a run that reports success
        # over a map it did not change, so a shortfall is an exit code and not
        # a remark somebody has to notice.
        print("\nnot measured:")
        for f in failures:
            print(f"  {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
