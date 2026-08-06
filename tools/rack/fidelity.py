#!/usr/bin/env python3
"""Does a generated patch play what was written into it?

    fidelity.py <patch.vcv>              # run both checks against one patch
    fidelity.py <patch.vcv> --seconds 6  # a longer listening window

Everything downstream of writing a patch file has been taken on trust: that
Rack restores the parameter values the generator chose, connects the cables it
drew, and that the result makes the notes it describes. This measures the
chain in three links, kept apart because a reader needs to know which one gave
way -- and because any one of them alone is a partial result:

  STRUCTURAL -- the engine, once loaded, holds what the file said. Rejects a
  value the loader clamped, an index that addressed the wrong knob, a cable on
  the wrong port, a default that overwrote what was written. Cheap, and it can
  only ever REJECT: a patch whose numbers all survived can still be wired into
  silence.

  PLAYED -- the step pitches the file wrote are the voltages the sequencer
  emits. This is where a knob addressed by the wrong index shows up, and it
  shows up with the file, the engine and the audio all looking correct.

  AUDIBLE -- the pitches a listener would hear, read off the signal the patch
  actually produced. The claim that matters, and the only one that can accept.

An exit code of 1 means a claim failed; 2 means the run could not measure at
all. They are different, and a measurement that did not happen is never
reported as a patch that passed.

All three are measured from inside a running Rack, by a probe module the
harness builds and drops into a scratch install (see `fidelity_probe.cpp`).
The probe writes down the engine's own view of the patch, and records whatever
is patched into its jacks.

AUDIO: the harness DELETES every audio-interface module before it runs, and
replaces it with the probe. Nothing in the patch can then reach an output
device, so a run makes no sound -- which matters, because these patches are
oscillators and the point of the exercise is to hear them. Rack still opens
CoreAudio at startup to enumerate devices; that is driver init, not a stream.

Every launch gets a THROWAWAY Rack user directory, for the reason
`measure_ranges.py` sets out at length: a killed session leaves a crash flag
that makes the next launch block on an alert nothing headless can answer. The
user's own autosave, log, settings and installed plugins are never written to.
"""

from __future__ import annotations

import array
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))

import crash_watch                                              # noqa: E402
import fetch_sdk                                                # noqa: E402
import measure_ranges as mr                                     # noqa: E402

PROBE_PLUGIN = "ForgeProbe"
PROBE_MODEL = "PROBE"
PROBE_INPUTS = 4

# Re-exported so a caller has one name to import: the split between driving
# Rack and reading a recording is an authoring boundary, not an API the tests
# and the CLI should have to know about.
from fidelity_signal import (                                   # noqa: E402,F401
    C4_HZ, CONFIDENCE, MIN_NOTE_WINDOWS, SILENCE, STEP_TOLERANCE, WINDOW,
    distinct, estimate_f0, heard_notes, held_voltages, hz_to_volts, rms,
    rms_label, runs, segment_pitches, spread, tracking, volts_to_semitones)


# ── Building the probe ───────────────────────────────────────────────────────

def build_probe(dest: str, sdk: str | None = None) -> str:
    """Compile the probe into `dest/ForgeProbe`, and return that directory.

    Built rather than installed. The probe exists to answer one question about
    one run, and putting a diagnostic module into the user's Rack -- where it
    would show up in their browser and outlive the question -- would be a
    strange price for a test to charge. A scratch install costs a two-second
    compile instead.
    """
    sdk = sdk or fetch_sdk.installed_at() or fetch_sdk.DEST
    if not os.path.isdir(sdk):
        raise SystemExit(
            f"the Rack SDK is not installed at {sdk}, so the probe cannot be "
            f"built. Run tools/rack/fetch_sdk.py.")
    out = os.path.join(dest, PROBE_PLUGIN)
    os.makedirs(out, exist_ok=True)
    lib = os.path.join(out, "plugin.dylib")
    r = subprocess.run(
        ["clang++", "-std=c++20", "-O2", "-fPIC", "-shared", "-o", lib,
         os.path.join(HERE, "fidelity_probe.cpp"),
         f"-I{sdk}/include", f"-I{sdk}/dep/include",
         "-DARCH_MAC", "-undefined", "dynamic_lookup"],
        capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("the probe did not compile:\n" + r.stderr)
    # Rack resolves the entry point with dlsym and reports nothing at all when
    # it comes back null, so the plugin would simply be absent -- and a run
    # with no probe in it looks exactly like a run that measured nothing.
    nm = subprocess.run(["nm", "-gU", lib], capture_output=True, text=True).stdout
    if " T _init" not in nm:
        raise SystemExit("the probe does not export _init, so Rack cannot load it")
    shutil.copy(os.path.join(HERE, "fidelity_probe.json"),
                os.path.join(out, "plugin.json"))
    return out


# ── A Rack that has never seen this machine's user directory ─────────────────

def make_scratch(probe_dir: str) -> str:
    """A throwaway Rack user directory with the probe added to the library.

    The plugins are symlinked ONE BY ONE rather than as a whole directory, so
    the probe can be added to the library without the user's install being
    touched: linking the arch directory wholesale would leave nowhere to put a
    plugin they do not have. Nothing here is ever written back.
    """
    scratch = tempfile.mkdtemp(prefix="rack-fidelity-")
    for arch in ("plugins-mac-arm64", "plugins-mac-x64"):
        real = os.path.join(mr.RACK_USER_DIR, arch)
        if not os.path.isdir(real):
            continue
        mirror = os.path.join(scratch, arch)
        os.makedirs(mirror)
        for name in os.listdir(real):
            src = os.path.join(real, name)
            if os.path.isdir(src):
                os.symlink(src, os.path.join(mirror, name))
    # The probe goes into whichever arch directory this Rack will read.
    arch = ("plugins-mac-arm64" if os.path.isdir(
        os.path.join(scratch, "plugins-mac-arm64")) else "plugins-mac-x64")
    os.makedirs(os.path.join(scratch, arch), exist_ok=True)
    os.symlink(probe_dir, os.path.join(scratch, arch, PROBE_PLUGIN))
    # Settings carry the Pro token and the licences save a round trip, so a
    # scratch launch is licensed exactly as a real one is.
    for name in ("settings.json", "licenses"):
        src = os.path.join(mr.RACK_USER_DIR, name)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(scratch, name))
        elif os.path.exists(src):
            shutil.copy2(src, os.path.join(scratch, name))
    return scratch


def drop_scratch(scratch: str) -> None:
    """Remove the scratch directory, and never anything it points at."""
    for arch in ("plugins-mac-arm64", "plugins-mac-x64"):
        d = os.path.join(scratch, arch)
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            p = os.path.join(d, name)
            if os.path.islink(p):
                os.unlink(p)
    shutil.rmtree(scratch, ignore_errors=True)


# ── Instrumenting a patch ────────────────────────────────────────────────────

# Modules that reach a sound card. Removed before a run: a patch under test is
# an oscillator, the harness is unattended, and the machine is somebody's desk.
AUDIO_MODELS = {("Core", "AudioInterface"), ("Core", "AudioInterface2"),
                ("Core", "AudioInterface16"), ("Core", "Audio"),
                ("Core", "Audio2"), ("Core", "Audio8"), ("Core", "Audio16")}


def is_audio_interface(mod: dict) -> bool:
    if (mod.get("plugin"), mod.get("model")) in AUDIO_MODELS:
        return True
    # By name as well as by slug: Rack has renamed these across versions and a
    # module that slips through reaches a speaker, which is the one failure
    # here that is not merely a wrong answer.
    return (mod.get("plugin") == "Core"
            and str(mod.get("model", "")).startswith("Audio"))


def sources_feeding(patch: dict, module_ids: set) -> list[tuple[int, int]]:
    """(moduleId, outputId) for every cable that ends in one of `module_ids`.

    Ordered and de-duplicated, so two cables from one output tap one jack.
    """
    out: list[tuple[int, int]] = []
    for c in patch.get("cables") or []:
        if c.get("inputModuleId") in module_ids:
            key = (c.get("outputModuleId"), c.get("outputId"))
            if key not in out:
                out.append(key)
    return out


def _words(name: str) -> set:
    out, word = set(), []
    for ch in (name or "").upper():
        if ch.isalnum() or ch == "/":
            word.append(ch)
        elif word:
            out.add("".join(word))
            word = []
    if word:
        out.add("".join(word))
    return out


def is_pitch_port(name: str) -> bool:
    """Whether a jack called `name` is where a module is told which note.

    Matched on whole words, so "STEPS" (how many steps a sequence has) cannot
    be read as a pitch input by containing "STEP", and "PITCH" is required to
    be the label rather than a fragment of one.
    """
    w = _words(name)
    return bool(w & {"PITCH", "VOCT", "V/OCT"}) or any(
        "V/OCT" in x for x in w)


def port_names(portmap: dict) -> dict:
    """(plugin, model, "in"|"out", index) -> the name its author gave the jack."""
    out = {}
    for e in (portmap.get("modules") or []):
        key = (e.get("plugin"), e.get("model"))
        for kind, field in (("in", "inputs"), ("out", "outputs")):
            for p in (e.get(field) or []):
                out[(key[0], key[1], kind, p.get("index"))] = p.get("name") or ""
    return out


def pitch_sources(patch: dict, names: dict) -> list[tuple[int, int]]:
    """(moduleId, outputId) for every cable that ends in a pitch input.

    Read from the port map, because a jack's NAME exists only inside Rack --
    a patch file records indices and nothing else. Without the map this finds
    nothing, which is correct: guessing that input 0 is pitch would tap the
    wrong wire and then compare the wrong two things.
    """
    by_id = {m.get("id"): m for m in (patch.get("modules") or [])}
    out: list[tuple[int, int]] = []
    for c in patch.get("cables") or []:
        dest = by_id.get(c.get("inputModuleId"))
        if not dest:
            continue
        name = names.get((dest.get("plugin"), dest.get("model"), "in",
                          c.get("inputId")), "")
        if is_pitch_port(name):
            key = (c.get("outputModuleId"), c.get("outputId"))
            if key not in out:
                out.append(key)
    return out


def is_step_param(name: str) -> bool:
    """Whether a knob called `name` holds one step's pitch.

    A step's pitch knob is "STEP" and WHICH step -- "Step 3", "CV 1 step 3".
    The number is what separates it from "Steps", the knob for how many steps
    a sequence has, which is a different word and a different thing.

    Trigger and gate words disqualify, because "Step 3 trigger" names the
    button beside the pitch knob on the same panel and counting those would
    expect twice as many notes as any patch plays.

    Requiring the word "CV" as well was too strict and made the check silently
    unprovable on the sequencer it matters most for: ForgeModular's own SEQ
    names its knobs "Step 1" through "Step 8", so a patch that demonstrably
    played four pitches reported having written no steps at all.
    """
    w = _words(name)
    if w & {"TRIGGER", "TRIG", "GATE"}:
        return False
    return "STEP" in w and any(x.isdigit() for x in w)


def param_names(portmap: dict) -> dict:
    """(plugin, model, index) -> the name its author gave the knob."""
    out = {}
    for e in (portmap.get("modules") or []):
        for p in (e.get("params") or []):
            out[(e.get("plugin"), e.get("model"), p.get("index"))] = \
                p.get("name") or ""
    return out


def step_values(patch: dict, source_id: int, names: dict
                ) -> tuple[list[float], str]:
    """(the pitches the patch wrote into `source_id`'s steps, why it could not).

    Only parameters the patch WROTE are counted. A step the file left alone is
    a step the generator did not choose, and holding the module's defaults
    against what plays would report a sequence nobody asked for as missing.

    The reason is returned rather than swallowed. A module with no mapped
    parameter names yields no steps, which is indistinguishable from a module
    whose steps are all zero -- and a check that cannot tell those apart would
    pass the silent patch it exists to catch.
    """
    mod = next((m for m in (patch.get("modules") or [])
                if m.get("id") == source_id), None)
    if not mod:
        return [], f"module {source_id} is not in the patch"
    key = (mod.get("plugin"), mod.get("model"))
    known = [i for (p, m, i) in names if (p, m) == key]
    if not known:
        return [], (f"{key[0]}/{key[1]} has no mapped parameter names, so "
                    f"which knobs hold the steps is not known here — run "
                    f"measure_ranges.py for that plugin")
    out = []
    for p in (mod.get("params") or []):
        if is_step_param(names.get((key[0], key[1], p.get("id")), "")):
            out.append(float(p.get("value", 0.0)))
    if not out:
        return [], (f"the patch wrote no step values on {key[0]}/{key[1]}, so "
                    f"there is no written sequence to hold the run against")
    return out, ""


def param_ranges(portmap: dict) -> dict:
    """(plugin, model, index) -> (min, max, name) for every mapped knob."""
    out = {}
    for e in (portmap.get("modules") or []):
        for p in (e.get("params") or []):
            if "minValue" in p and "maxValue" in p:
                out[(e.get("plugin"), e.get("model"), p.get("index"))] = (
                    p["minValue"], p["maxValue"], p.get("name") or "")
    return out


def will_be_clamped(patch: dict, ranges: dict) -> list[str]:
    """Values the patch writes that the module's own bounds will not accept.

    Reading only, and it needs no Rack: a knob declares its range, and a value
    outside it is one Rack replaces with the nearest bound as it loads. Nothing
    reports that -- the patch keeps the number it was written with, the module
    quietly holds a different one, and the difference only shows up as the
    patch not doing what it says.

    Cheap enough to run before a launch, so a defect that a run would find in
    ten seconds is named in none.
    """
    out = []
    for m in (patch.get("modules") or []):
        for prm in (m.get("params") or []):
            key = (m.get("plugin"), m.get("model"), prm.get("id"))
            if key not in ranges:
                continue
            lo, hi, name = ranges[key]
            v = float(prm.get("value", 0.0))
            if v < lo or v > hi:
                out.append(f"module {m.get('id')} ({m.get('model')}) "
                           f"param {prm.get('id')} {name!r}: the patch writes "
                           f"{v:g}, which the knob's range [{lo:g}, {hi:g}] "
                           f"will clamp on load")
    return out


def choose_taps(patch: dict, names: dict) -> list[tuple[int, int, str]]:
    """What to listen to, in probe-input order: the sound first, then the CV.

    The sound is whatever the audio interfaces were being fed -- the signal a
    listener would have heard. The CV is whatever is telling an oscillator
    which note to play. Together they are the two ends of the claim: the
    second says what was asked for, the first says what came out.
    """
    sinks = {m.get("id") for m in (patch.get("modules") or [])
             if is_audio_interface(m)}
    out = [(m, o, "audio") for m, o in sources_feeding(patch, sinks)]
    for m, o in pitch_sources(patch, names):
        if not any(m == am and o == ao for am, ao, _ in out):
            out.append((m, o, "pitch"))
    return out[:PROBE_INPUTS]


def instrument(patch: dict, taps: list[tuple] | None = None
               ) -> tuple[dict, list[tuple]]:
    """A copy of `patch` with the audio interfaces gone and the probe wired in.

    Returns the new patch and the taps it recorded, in probe-input order, so a
    reader of the capture knows which channel is which.

    When no taps are named, the probe takes over from the audio interfaces: it
    is patched with exactly what they were being fed. That is the signal a
    listener would have heard, which is the thing under test.

    The probe goes LAST in the module list, which is what puts every other
    module and cable in the engine before its widget is added -- the same
    ordering constraint CARTOG has, for the same reason.
    """
    patch = json.loads(json.dumps(patch))          # never edit the caller's
    mods = patch.get("modules") or []
    sinks = {m.get("id") for m in mods if is_audio_interface(m)}
    if taps is None:
        taps = choose_taps(patch, port_names(mr.read_portmap()))

    patch["modules"] = [m for m in mods if m.get("id") not in sinks]
    patch["cables"] = [c for c in (patch.get("cables") or [])
                       if c.get("inputModuleId") not in sinks
                       and c.get("outputModuleId") not in sinks]

    next_id = max([m.get("id", -1) for m in patch["modules"]] + [-1]) + 1
    xs = [m.get("pos", [0, 0])[0] for m in patch["modules"]] or [0]
    patch["modules"].append({"id": next_id, "plugin": PROBE_PLUGIN,
                             "model": PROBE_MODEL,
                             "pos": [max(xs) + 12, 0]})

    next_cable = max([c.get("id", -1) for c in patch["cables"]] + [-1]) + 1
    for i, (src_mod, src_out, _kind) in enumerate(taps[:PROBE_INPUTS]):
        patch["cables"].append({
            "id": next_cable + i,
            "outputModuleId": src_mod, "outputId": src_out,
            "inputModuleId": next_id, "inputId": i,
            "color": "#0c8e15"})
    return patch, list(taps[:PROBE_INPUTS])


# ── Running it ───────────────────────────────────────────────────────────────

ENGINE_FILE = "forge-probe-engine.json"
SIGNAL_JSON = "forge-probe-signal.json"
SIGNAL_RAW = "forge-probe-signal.f32"

LOAD_WINDOW = 25.0        # to reach "Loading patch" before the launch is written off
ABORT_ATTEMPTS = 3        # a CoreMIDI abort is sporadic; four of them is a machine


class Run:
    """What one launch produced. `why` is empty only when everything landed."""

    def __init__(self, engine: dict | None, signal: dict | None,
                 frames: list | None, log: str, why: str):
        self.engine = engine
        self.signal = signal
        self.frames = frames or []
        self.log = log
        self.why = why


def launch_once(rack: str, patch_path: str, probe_dir: str,
                seconds: float) -> Run:
    """One headless Rack, run until the probe has written both its files.

    Rack is asked to leave rather than killed: the probe's capture is flushed
    from the audio callback the moment the window fills, but the fallback --
    the module destructor -- only runs on a clean shutdown. Holding stdin open
    is what keeps Rack alive at all; it exits as soon as a line arrives.
    """
    scratch = make_scratch(probe_dir)
    env = dict(os.environ, FORGE_PROBE_OUT=scratch,
               FORGE_PROBE_SECONDS=str(seconds))
    proc = subprocess.Popen(
        [rack, "-h", "-u", scratch, patch_path],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, env=env)
    engine = signal = None
    frames: list | None = None
    log = ""
    why = ""
    # The listening window plus room for a big library to load and tear down.
    deadline = time.time() + seconds + 60.0
    try:
        loading = False
        started = time.time()
        while True:
            log = mr.read_log(scratch)
            mine = patch_path in log
            if mine and mr.LOADING in log:
                loading = True
            if os.path.exists(os.path.join(scratch, SIGNAL_JSON)):
                break
            if proc.poll() is not None:
                why = ("Rack aborted in CoreMIDI before it reached the patch"
                       if mr.aborted_in_coremidi(log)
                       else "Rack exited before the probe wrote anything")
                break
            now = time.time()
            if not loading and now - started > LOAD_WINDOW:
                why = "Rack never got as far as loading the patch"
                break
            if now > deadline:
                why = ("Rack loaded the patch but the probe never finished "
                       "recording")
                break
            time.sleep(0.2)
        engine = _read_json(os.path.join(scratch, ENGINE_FILE))
        signal = _read_json(os.path.join(scratch, SIGNAL_JSON))
        if signal:
            frames = read_capture(os.path.join(scratch, SIGNAL_RAW),
                                  int(signal.get("channels") or PROBE_INPUTS))
    finally:
        if proc.poll() is None:
            try:
                proc.communicate(b"\n", timeout=25)
            except Exception:                                   # noqa: BLE001
                proc.kill()
                proc.wait(timeout=10)
        drop_scratch(scratch)
    return Run(engine, signal, frames, log, why)


def run(rack: str, patch_path: str, probe_dir: str, seconds: float,
        attempts: int = 4) -> Run:
    """Launch until one launch produces a capture, or the attempts run out.

    Only a crash that provably predates patch loading is retried, and the
    classification comes from `crash_watch` rather than from guessing at a
    stack: a retry loop that swallows real crashes would turn a patch that
    kills Rack into a patch that merely takes a while to fail.
    """
    aborts = 0
    last = Run(None, None, None, "", "no attempt was made")
    for attempt in range(1, attempts + 1):
        mark = crash_watch.now()
        last = launch_once(rack, patch_path, probe_dir, seconds)
        if not last.why:
            return last
        crashes = crash_watch.since(mark)
        fatal = [c for c in crashes if not c.retryable]
        if fatal:
            last.why = f"{last.why}; {fatal[0].summary}"
            return last
        if crashes:
            aborts += 1
            if aborts >= ABORT_ATTEMPTS:
                last.why = (f"{aborts} launches died in CoreMIDI before "
                            f"reaching the patch")
                return last
        if attempt < attempts:
            print(f"    launch {attempt}: {last.why} — retrying", flush=True)
            time.sleep(2.0)
    return last


def _read_json(path: str) -> dict | None:
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def read_capture(path: str, channels: int) -> list[list[float]]:
    """The raw capture as one list of samples per channel."""
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError:
        return []
    a = array.array("f")
    a.frombytes(raw[:len(raw) - len(raw) % 4])
    if sys.byteorder != "little":
        a.byteswap()
    return [list(a[c::channels]) for c in range(channels)]


# ── Structural fidelity ──────────────────────────────────────────────────────

# Parameter values round-trip through JSON as decimal text, so an exact
# comparison is not the question -- a knob that came back a millionth away is
# the same knob. A knob that came back at its default, or clamped to a bound,
# is not, and both of those move it much further than this.
PARAM_TOLERANCE = 1e-6


def structural_diff(written: dict, engine: dict | None) -> list[str]:
    """Everything the engine holds that the patch file did not ask for.

    Compared against the file that was actually handed to Rack, so the probe
    and its own cables are part of the comparison rather than an exception --
    an instrument that exempted itself could not notice that it had failed to
    arrive.
    """
    if not engine:
        return ["the probe never wrote the engine state, so nothing was read "
                "back — this is a broken measurement, not a failed patch"]
    out: list[str] = []
    live = {m.get("id"): m for m in (engine.get("modules") or [])}
    for m in (written.get("modules") or []):
        mid = m.get("id")
        got = live.get(mid)
        if got is None:
            out.append(f"module {mid} ({m.get('plugin')}/{m.get('model')}) "
                       f"is not in the loaded engine")
            continue
        if (got.get("plugin"), got.get("model")) != (m.get("plugin"),
                                                     m.get("model")):
            out.append(f"module {mid} loaded as {got.get('plugin')}/"
                       f"{got.get('model')}, not {m.get('plugin')}/"
                       f"{m.get('model')}")
            continue
        have = {p.get("id"): p.get("value") for p in (got.get("params") or [])}
        for p in (m.get("params") or []):
            pid, want = p.get("id"), p.get("value")
            if pid not in have:
                out.append(f"module {mid} ({m.get('model')}) has no param "
                           f"{pid}, but the patch set it to {want}")
            elif abs(float(have[pid]) - float(want)) > PARAM_TOLERANCE:
                out.append(f"module {mid} ({m.get('model')}) param {pid}: "
                           f"wrote {want}, engine holds {have[pid]}")

    def ends(c):
        return (c.get("outputModuleId"), c.get("outputId"),
                c.get("inputModuleId"), c.get("inputId"))

    want_cables = [ends(c) for c in (written.get("cables") or [])]
    got_cables = [ends(c) for c in (engine.get("cables") or [])]
    for c in want_cables:
        if c not in got_cables:
            out.append(f"cable {c[0]}:{c[1]} -> {c[2]}:{c[3]} is not connected "
                       f"in the loaded engine")
    for c in got_cables:
        if c not in want_cables:
            out.append(f"the engine has a cable {c[0]}:{c[1]} -> {c[2]}:{c[3]} "
                       f"that the patch never asked for")
    return out


# ── The check ────────────────────────────────────────────────────────────────

class Verdict:
    """What a run proved, kept as separate claims rather than one pass/fail.

    Rounding a partial result up to a pass is the failure this whole exercise
    exists to avoid, so structural and audible each carry their own state and
    their own reason, and `ok` is true only when nothing is outstanding.
    """

    def __init__(self):
        self.structural: list[str] = []       # what disagreed; empty is a pass
        self.structural_ran = False
        self.audible_ran = False
        self.notes: list[float] = []
        self.distinct: list[float] = []
        self.volts: list[float] = []
        self.written: list[float] = []        # the steps the file asked for
        self.error: float | None = None       # worst tracking error, semitones
        self.audible_why = ""                 # why the audible claim is unproven
        self.played_why = ""                  # why the written steps are unproven
        self.dropped = 0
        self.level = 0.0                      # RMS of the signal that was heard
        self.lines: list[str] = []

    @property
    def ok(self) -> bool:
        return (self.structural_ran and not self.structural
                and self.audible_ran and not self.audible_why
                and not self.played_why)


# How far a heard pitch may sit from the pitch its CV asked for. A semitone is
# the smallest interval anybody writes, so a quarter of one is a wide margin
# for the reader and still nowhere near letting a wrong note pass.
TRACKING_TOLERANCE = 0.25


def judge(given: dict, got: Run, taps: list[tuple],
          names: dict | None = None) -> Verdict:
    """Turn one run into the claims it supports, with the evidence for each.

    Three links, kept apart because they fail in different places and a reader
    needs to know which one gave way:

      the file's numbers  ->  the loaded engine        (structural)
      the written steps   ->  the voltages emitted     (played)
      the voltages        ->  the pitches heard        (audible)

    Only all three together mean the patch plays what was written. Any one of
    them alone is a partial result, and is reported as one.
    """
    v = Verdict()
    v.structural = structural_diff(given, got.engine)
    v.structural_ran = got.engine is not None
    v.lines.append("structural:")
    v.lines += ([f"  {d}" for d in v.structural]
                or ["  every value and cable survived the load"])

    v.lines.append("audible:")
    rate = float((got.signal or {}).get("sampleRate") or 0)
    frames = int((got.signal or {}).get("frames") or 0)
    if not frames or not rate:
        v.audible_why = ("the engine produced no samples at all, so nothing "
                         "was heard and nothing can be concluded")
        v.lines.append("  " + v.audible_why)
        return v
    v.audible_ran = True
    v.lines.append(f"  captured {frames} frames at {rate:g} Hz "
                   f"({frames / rate:.2f}s)")

    # Every tap is reported; the FIRST of each kind is the one judged. A
    # stereo pair is two taps carrying one melody, and reading the second over
    # the first would make the verdict depend on which channel was listed last.
    for ch, (src_mod, src_out, kind) in enumerate(taps):
        where = f"  tap {ch} ({kind}, module {src_mod} output {src_out}):"
        if kind == "audio":
            notes, dropped = heard_notes(got.frames[ch], rate)
            level = rms(got.frames[ch])
            if not v.notes:
                v.notes, v.dropped, v.distinct = notes, dropped, distinct(notes)
                v.level = level
            v.distinct = v.distinct or distinct(notes)
            v.lines.append(f"{where} {rms_label(level)}, {len(notes)} notes, "
                           f"{len(distinct(notes))} distinct — "
                           f"{distinct(notes)} semitones from middle C "
                           f"({dropped} windows unresolved)")
        else:
            volts = held_voltages(got.frames[ch], rate)
            v.lines.append(f"{where} {len(volts)} held values — "
                           f"{distinct(volts, STEP_TOLERANCE)} V")
            if not v.volts:
                v.volts = volts
                v.written, v.played_why = step_values(given, src_mod,
                                                      names or {})

    heard = distinct(v.volts, STEP_TOLERANCE)
    want = distinct(v.written, STEP_TOLERANCE)
    if not v.played_why and v.volts:
        missing = [w for w in want
                   if all(abs(w - h) > STEP_TOLERANCE for h in heard)]
        extra = [h for h in heard
                 if all(abs(h - w) > STEP_TOLERANCE for w in want)]
        v.lines.append(f"  the patch wrote {len(want)} distinct step pitches; "
                       f"{len(heard)} came out")
        if missing:
            v.played_why = (f"step pitches the patch wrote never played: "
                            f"{missing} V")
        elif extra:
            v.played_why = (f"voltages came out that the patch never wrote: "
                            f"{extra} V")
    elif not v.played_why:
        v.played_why = "no pitch CV was tapped, so no written step was proven"
    if v.played_why:
        v.lines.append("  " + v.played_why)

    if not v.notes:
        # SILENT and UNREADABLE are different findings, and the level tells
        # them apart. Reported as one message they are indistinguishable, and
        # a patch that plainly sounds reads as one that makes nothing.
        if v.level < SILENCE:
            v.audible_why = ("the audio tap is silent — nothing at all came "
                             "out of the patch")
        else:
            v.audible_why = (
                f"the audio tap carries signal at {v.level:.3f} V RMS but no "
                f"window settled on a pitch, so what it makes is not a note "
                f"this can read — noise, a click, or something faster than "
                f"the {WINDOW * 1000:.0f} ms it reads at")
    elif not v.volts:
        v.audible_why = ("no pitch CV was tapped, so the notes heard cannot "
                         "be held against what the patch asked for")
    else:
        v.error, why = tracking(v.notes, v.volts)
        if why:
            v.audible_why = why
        elif v.error > TRACKING_TOLERANCE:
            v.audible_why = (f"each step is consistently {v.error:.2f} "
                             f"semitones from the pitch its CV asked for, "
                             f"which is more than the {spread(v.notes, v.volts):.2f} "
                             f"this reading varies by")
        else:
            # The reader's own precision beside the verdict, so a pass on a
            # plucked patch is not read as a tighter result than it is.
            v.lines.append(f"  every step is within {v.error:.2f} semitones "
                           f"of the pitch its CV asked for "
                           f"(the same step varied {spread(v.notes, v.volts):.2f} "
                           f"between passes, which is what this can resolve)")
    if v.audible_why:
        v.lines.append("  " + v.audible_why)
    return v


def check(patch_path: str, seconds: float = 4.0,
          taps: list[tuple] | None = None,
          probe_dir: str | None = None) -> tuple[int, list[str]]:
    """Run both checks against one patch. Returns (exit code, report lines)."""
    rack = mr.rack_binary()
    if not rack:
        return 2, ["no Rack found; set FORGE_RACK_BIN"]
    with open(patch_path) as f:
        original = json.load(f)

    stage = tempfile.mkdtemp(prefix="fidelity-")
    portmap = mr.read_portmap()
    try:
        probe = probe_dir or build_probe(stage)
        given, used = instrument(original, taps)
        if not used:
            return 2, ["nothing to listen to: the patch feeds no audio "
                       "interface and no oscillator is told a pitch, so there "
                       "is no signal for the probe to record"]
        run_patch = os.path.join(stage, "under-test.vcv")
        with open(run_patch, "w") as f:
            json.dump(given, f)

        for line in will_be_clamped(original, param_ranges(portmap)):
            print(f"  {line}", flush=True)
        print(f"  running {os.path.basename(patch_path)} headless for "
              f"{seconds:g}s — every audio interface was removed first, so "
              f"this makes no sound", flush=True)
        got = run(rack, run_patch, probe, seconds)
        if got.why:
            return 2, [f"the run did not measure anything: {got.why}"]
        v = judge(given, got, used, param_names(portmap))
        return (0 if v.ok else 1), v.lines
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def main(argv: list[str]) -> int:
    args = [a for a in argv[1:] if not a.startswith("--")]
    seconds = 4.0
    if "--seconds" in argv:
        at = argv.index("--seconds") + 1
        seconds = float(argv[at])
        args = [a for a in args if a != argv[at]]
    if not args:
        print(__doc__.split("\n\n")[1].strip("\n"))
        return 2
    code, lines = check(args[0], seconds)
    for line in lines:
        print(line)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
