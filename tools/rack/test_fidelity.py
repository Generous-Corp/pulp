#!/usr/bin/env python3
"""Can the fidelity harness tell a patch that plays from one that does not?

    python3 tools/rack/test_fidelity.py            # shape checks, no Rack
    python3 tools/rack/test_fidelity.py --with-rack  # and three real launches

The shape checks are the ones that run everywhere. They cover the failures
that would make the harness agree with any patch put in front of it -- a
comparator that overlooks a clamped value, a pitch reader that finds notes in
silence, an instrumented patch that still reaches a sound card.

`--with-rack` adds the part that cannot be faked: three launches of real Rack,
one on a patch known to play four pitches and two on the same patch broken on
purpose. A check that has never been seen to fail is not evidence, and the two
sabotages are the only place in this file where the whole chain -- Rack, the
probe, the engine, the analyser -- is asked to disagree with something.

Those three runs make no sound. The harness deletes every audio interface
before it launches, so nothing in the patch can reach an output device.
"""

import copy
import json
import math
import os
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import fidelity as F                                        # noqa: E402
import measure_ranges as mr                                 # noqa: E402

FIXTURE = os.path.join(HERE, "patch_idioms", "regressions",
                       "four-pitch-sequence.vcv")

# What the fixture writes: four steps a fifth of an octave apart, on
# Fundamental's SEQ3, into Fundamental's VCO.
FIXTURE_STEPS = [0.0, 0.25, 0.5, 0.75]
SEQ, VCO = 0, 1


def check(ok: bool, label: str, detail: str = "") -> int:
    print(f"  {'ok    ' if ok else 'WRONG '} {label}"
          + (f" — {detail}" if detail and not ok else ""))
    return 0 if ok else 1


def fixture() -> dict:
    with open(FIXTURE) as f:
        return json.load(f)


# ── The comparator ───────────────────────────────────────────────────────────

def _engine_from(patch: dict) -> dict:
    """The readback a perfectly faithful Rack would have written."""
    return {
        "modules": [{"id": m["id"], "plugin": m["plugin"], "model": m["model"],
                     "params": [{"id": p["id"], "value": p["value"]}
                                for p in m.get("params", [])]}
                    for m in patch["modules"]],
        "cables": [dict(c) for c in patch["cables"]],
    }


def test_a_faithful_load_reads_as_faithful() -> int:
    """The control. A comparator that never passes cannot fail informatively."""
    p, _ = F.instrument(fixture(), taps=[(VCO, 2, "audio")])
    return check(F.structural_diff(p, _engine_from(p)) == [],
                 "an engine holding exactly what was written reads clean")


def test_a_clamped_value_is_caught() -> int:
    p, _ = F.instrument(fixture(), taps=[(VCO, 2, "audio")])
    e = _engine_from(p)
    e["modules"][SEQ]["params"][3]["value"] = 1.0        # was 0.5
    out = F.structural_diff(p, e)
    return check(len(out) == 1 and "param" in out[0],
                 "a parameter the engine holds differently is named", str(out))


def test_a_default_that_overwrote_a_written_value_is_caught() -> int:
    p, _ = F.instrument(fixture(), taps=[(VCO, 2, "audio")])
    e = _engine_from(p)
    for prm in e["modules"][SEQ]["params"]:
        prm["value"] = 0.0                               # every knob at default
    out = F.structural_diff(p, e)
    return check(len(out) == 4,
                 "every written value the engine reset to a default is named",
                 str(out))


def test_a_cable_on_the_wrong_port_is_caught() -> int:
    p, _ = F.instrument(fixture(), taps=[(VCO, 2, "audio")])
    e = _engine_from(p)
    e["cables"][0]["inputId"] = 1                        # V/OCT -> FM
    out = F.structural_diff(p, e)
    return check(len(out) == 2 and any("not connected" in d for d in out),
                 "a cable that landed on a different jack is named", str(out))


def test_no_readback_is_not_a_pass() -> int:
    """The one that matters most. Nothing read back is not nothing wrong."""
    p, _ = F.instrument(fixture(), taps=[(VCO, 2, "audio")])
    out = F.structural_diff(p, None)
    return check(len(out) == 1 and "broken measurement" in out[0],
                 "a run that read nothing back says so instead of passing",
                 str(out))


# ── Instrumenting ────────────────────────────────────────────────────────────

def test_the_audio_interface_is_removed() -> int:
    p, _ = F.instrument(fixture())
    bad = 0
    bad += check(not any(F.is_audio_interface(m) for m in p["modules"]),
                 "no audio interface survives instrumentation")
    ids = {m["id"] for m in p["modules"]}
    bad += check(all(c["inputModuleId"] in ids and c["outputModuleId"] in ids
                     for c in p["cables"]),
                 "no cable is left dangling where one used to be")
    return bad


def test_the_probe_takes_over_from_the_speakers() -> int:
    """What was being sent to the sound card is what gets recorded."""
    p, taps = F.instrument(fixture())
    probe = [m for m in p["modules"] if m["model"] == F.PROBE_MODEL]
    bad = check(len(probe) == 1, "exactly one probe is added")
    bad += check(taps and taps[0] == (VCO, 2, "audio"),
                 "the first tap is the signal the audio interface was fed",
                 str(taps))
    bad += check(any(t[2] == "pitch" and t[0] == SEQ for t in taps),
                 "the pitch CV is tapped as well", str(taps))
    bad += check(p["modules"][-1]["model"] == F.PROBE_MODEL,
                 "the probe is listed last, so every module is in the engine "
                 "before its widget is added")
    return bad


def test_the_caller_patch_is_not_edited() -> int:
    before = fixture()
    F.instrument(before)
    return check(before == fixture(), "instrumenting leaves the input alone")


# ── Reading names ────────────────────────────────────────────────────────────

def test_step_knobs_are_told_from_their_trigger_buttons() -> int:
    bad = check(F.is_step_param("CV 1 step 3"), "a step's pitch knob matches")
    # Our own sequencer names its knobs this way, and requiring "CV" too made
    # the check unprovable on the module it matters most for.
    bad += check(F.is_step_param("Step 3"), "so does a bare numbered step")
    bad += check(not F.is_step_param("Step 3 trigger"),
                 "the trigger button beside it does not")
    bad += check(not F.is_step_param("Steps"),
                 "the knob for how many steps there are does not")
    bad += check(not F.is_step_param("Step gate"),
                 "nor an unnumbered gate knob")
    return bad


def test_pitch_inputs_are_recognised_by_name() -> int:
    bad = check(F.is_pitch_port("1V/octave pitch"), "Fundamental's V/OCT jack")
    bad += check(F.is_pitch_port("V/OCT"), "the bare label")
    bad += check(not F.is_pitch_port("Frequency modulation"),
                 "an FM input is not a pitch input")
    bad += check(not F.is_pitch_port("Steps"), "a step-count input is not one")
    return bad


def test_unmapped_modules_say_so_rather_than_report_no_steps() -> int:
    """A module nobody cartographed has unknown steps, not zero steps."""
    p = fixture()
    vals, why = F.step_values(p, SEQ, {})
    return check(vals == [] and "no mapped parameter names" in why,
                 "an unmapped module is reported as unknown", why)


def test_written_steps_are_read_off_the_patch() -> int:
    names = {("Fundamental", "SEQ3", i): f"CV 1 step {i - 3}"
             for i in range(4, 12)}
    names[("Fundamental", "SEQ3", 3)] = "Steps"
    vals, why = F.step_values(fixture(), SEQ, names)
    return check(sorted(vals) == FIXTURE_STEPS and not why,
                 "the four written step pitches are found", f"{vals} {why}")


def test_a_value_outside_a_knobs_range_is_named_before_launching() -> int:
    """The one structural break found on a real generated patch, made cheap.

    A knob with a floor of a thousandth, written zero, loads as a thousandth.
    Nothing says so: the file keeps the zero and the module holds something
    else. Predicting it from the declared range costs no Rack at all.
    """
    ranges = {("Fundamental", "SEQ3", 4): (-10.0, 10.0, "CV 1 step 1"),
              ("Fundamental", "VCO", 2): (-54.0, 54.0, "Frequency")}
    bad = check(F.will_be_clamped(fixture(), ranges) == [],
                "a patch inside every range predicts no clamp")
    over = fixture()
    over["modules"][SEQ]["params"][1]["value"] = 42.0
    out = F.will_be_clamped(over, ranges)
    bad += check(len(out) == 1 and "42" in out[0] and "clamp" in out[0],
                 "a value past a knob's ceiling is named", str(out))
    bad += check(F.will_be_clamped(over, {}) == [],
                 "a knob with no mapped range is not guessed at")
    return bad


# ── Hearing ──────────────────────────────────────────────────────────────────

def _tone(hz: float, seconds: float, rate: float) -> list[float]:
    n = int(seconds * rate)
    return [5.0 * math.sin(2 * math.pi * hz * i / rate) for i in range(n)]


def test_a_known_tone_reads_as_its_own_pitch() -> int:
    """The detector's control. It has to be right before it may say 'wrong'."""
    rate = 44100.0
    notes, _ = F.heard_notes(_tone(F.C4_HZ, 0.5, rate), rate)
    bad = check(len(notes) == 1 and abs(notes[0]) < 0.1,
                "middle C reads as middle C", str(notes))
    notes, _ = F.heard_notes(_tone(F.C4_HZ * 2, 0.5, rate), rate)
    bad += check(len(notes) == 1 and abs(notes[0] - 12) < 0.1,
                 "an octave up reads as twelve semitones", str(notes))
    return bad


def _band_limited_saw(hz: float, seconds: float, rate: float) -> list[float]:
    """A sawtooth with no energy above Nyquist, as an oscillator makes it."""
    n = int(seconds * rate)
    harmonics = max(1, int(rate / 2 / hz))
    out = []
    for i in range(n):
        t = i / rate
        v = sum(math.sin(2 * math.pi * h * hz * t) / h
                for h in range(1, harmonics + 1))
        out.append(5 * 2 / math.pi * v)
    return out


def test_the_reader_is_accurate_across_the_range_it_claims() -> int:
    """Where the pitch reader can be trusted, pinned rather than assumed.

    Every note below is checked against the frequency it was built from, so
    this fails if the reader drifts -- including the way it used to fail,
    which was to report a confident number from the end of its own search
    rather than from the signal.
    """
    rate = 44100.0
    bad = 0
    for hz in (55.0, 110.0, 220.0, F.C4_HZ, 440.0, 880.0, 1046.5):
        for name, wave in (("sine", _tone),
                           ("saw", _band_limited_saw)):
            got = F.estimate_f0(wave(hz, F.WINDOW, rate), rate)
            err = abs(12 * math.log2(got / hz)) if got else None
            bad += check(err is not None and err < 0.2,
                         f"a {name} at {hz:g} Hz reads within a fifth of a "
                         f"semitone", f"read {got}")
    return bad


def test_nothing_periodic_is_not_a_pitch() -> int:
    """Noise and DC are where a detector invents notes. It must not."""
    import random
    rate = 44100.0
    n = int(F.WINDOW * rate)
    random.seed(1)
    bad = check(F.estimate_f0([random.uniform(-5, 5) for _ in range(n)],
                              rate) is None,
                "noise is not given a pitch")
    bad += check(F.estimate_f0([3.0] * n, rate) is None,
                 "a steady voltage is not given a pitch")
    return bad


def test_silence_is_not_a_note() -> int:
    rate = 44100.0
    notes, dropped = F.heard_notes([0.0] * int(rate), rate)
    bad = check(notes == [] and dropped > 0,
                "a silent recording yields no notes and says how much it "
                "could not resolve", str(notes))
    bad += check(F.rms_label(F.rms([0.0] * 100)) == "silent",
                 "a silent recording is called silent")
    bad += check(F.rms_label(F.rms(_tone(F.C4_HZ, 0.1, rate))) != "silent",
                 "a recording with signal in it is not")
    return bad


def test_a_plucked_note_is_not_thrown_away() -> int:
    """A note short enough to read once, with silence either side, is a note.

    Requiring every note to hold two windows made a real envelope-gated patch
    -- one whose recording plainly carries signal, and whose middle C the
    reader finds -- report as playing nothing at all. Silence around a reading
    is what tells a brief note from the boundary between two long ones.
    """
    plucked = [None, None, 0.0, None, None, 12.0, None]
    kept = [v for v, n in F.runs(plucked)]
    bad = check(kept.count(0.0) == 1, "the grouping keeps the reading")
    rate = 44100.0
    sig = ([0.0] * int(0.12 * rate) + _tone(F.C4_HZ, 0.07, rate)
           + [0.0] * int(0.12 * rate) + _tone(F.C4_HZ * 2, 0.07, rate)
           + [0.0] * int(0.12 * rate))
    notes, _ = F.heard_notes(sig, rate)
    bad += check(len(notes) == 2 and abs(notes[0]) < 0.3
                 and abs(notes[1] - 12) < 0.3,
                 "two plucks separated by silence read as two notes",
                 str(notes))
    return bad


def test_a_sequence_of_tones_reads_as_that_sequence() -> int:
    rate = 44100.0
    want = [0.0, 3.0, 6.0, 9.0]
    sig: list[float] = []
    for semis in want:
        sig += _tone(F.C4_HZ * 2 ** (semis / 12), 0.4, rate)
    notes, _ = F.heard_notes(sig, rate)
    return check(len(notes) == 4
                 and all(abs(a - b) < 0.15 for a, b in zip(notes, want)),
                 "four tones read as four notes at the right pitches",
                 str(notes))


def test_one_stray_window_is_not_a_note() -> int:
    """A boundary between two notes is not a third note."""
    values = [0.0] * 8 + [1.6] + [3.0] * 8
    kept = [v for v, n in F.runs(values) if n >= F.MIN_NOTE_WINDOWS]
    return check(kept == [0.0, 3.0],
                 "a single-window reading between two notes is dropped",
                 str(kept))


def test_a_run_is_reported_by_its_middle_not_its_edge() -> int:
    values = [2.7] + [3.05] * 6
    got = F.runs(values)
    return check(len(got) == 1 and abs(got[0][0] - 3.05) < 0.01,
                 "the window that contains the change does not set the pitch",
                 str(got))


def test_tracking_needs_both_taps() -> int:
    err, why = F.tracking([0.0, 3.0], [])
    bad = check(err is None and "cannot be compared" in why,
                "one tap alone is not a comparison", why)
    err, why = F.tracking([0.0, 3.0, 6.0], [0.0, 0.25])
    bad += check(err is None and "do not line up" in why,
                 "counts that disagree are not silently zipped", why)
    return bad


def test_tracking_is_blind_to_tuning_but_not_to_intervals() -> int:
    # The same intervals an octave up: a coarse-tune knob is not a wrong note.
    err, why = F.tracking([12.0, 15.0, 18.0], [0.0, 0.25, 0.5])
    bad = check(err is not None and err < 1e-9 and not why,
                "a transposed reading of the right intervals is not an error",
                f"{err} {why}")
    err, why = F.tracking([0.0, 1.0, 2.0], [0.0, 0.25, 0.5])
    bad += check(err is not None and err > 3.0,
                 "an oscillator not tracking a volt per octave is an error",
                 str(err))
    return bad


def test_scatter_is_not_reported_as_mistuning() -> int:
    """The same step read three times, differently, is the reader — not the patch.

    A note an envelope plucks is loud enough to read for a window or two, so
    one step reads 5.06, 5.33 and 5.39 across three passes. Taking the worst
    of those called a correct generated patch a third of a semitone out of
    tune. An oscillator that is actually mistuned is wrong the SAME way every
    time, so the average over repeats tells them apart.
    """
    notes = [3.09, 6.91, 5.33, -0.25, 3.11, 7.01, 5.39, 0.14, 3.10, 7.05, 5.06]
    volts = [0.25, 0.583, 0.417, 0.0, 0.25, 0.583, 0.417, 0.0,
             0.25, 0.583, 0.417]
    err, why = F.tracking(notes, volts)
    bad = check(err is not None and err < F.TRACKING_TOLERANCE and not why,
                "scatter around the right pitch is not called mistuning",
                f"{err} {why}")
    bad += check(F.spread(notes, volts) > err,
                 "and the reading says how much it varied", str(F.spread(notes, volts)))
    # An oscillator half a semitone sharp on every step, consistently.
    off = [0.0, 3.5, 7.5, 0.0, 3.5, 7.5]
    same = [0.0, 0.25, 0.583, 0.0, 0.25, 0.583]
    err, _ = F.tracking(off, same)
    bad += check(err is not None and err > F.TRACKING_TOLERANCE,
                 "a consistent offset is still caught", str(err))
    return bad


def test_a_capture_is_read_back_channel_by_channel() -> int:
    import struct
    import tempfile
    path = os.path.join(tempfile.mkdtemp(), "cap.f32")
    with open(path, "wb") as f:
        f.write(struct.pack("<8f", 1, 2, 3, 4, 5, 6, 7, 8))
    got = F.read_capture(path, 4)
    return check(got == [[1.0, 5.0], [2.0, 6.0], [3.0, 7.0], [4.0, 8.0]],
                 "interleaved frames split back into their channels", str(got))


# ── With Rack ────────────────────────────────────────────────────────────────

def _run_patch(patch: dict, label: str, probe: str, seconds: float = 4.0):
    """One real launch. Returns the Verdict, or None when Rack never measured."""
    import shutil
    import tempfile
    stage = tempfile.mkdtemp(prefix="fid-test-")
    try:
        given, taps = F.instrument(patch)
        path = os.path.join(stage, "under-test.vcv")
        with open(path, "w") as f:
            json.dump(given, f)
        got = F.run(mr.rack_binary(), path, probe, seconds)
        if got.why:
            print(f"    {label}: the launch did not measure — {got.why}")
            return None
        return F.judge(given, got, taps, F.param_names(mr.read_portmap()))
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def test_with_rack(probe: str) -> int:
    """The fixture plays, and two deliberate breaks stop it playing.

    The sabotages are chosen to break DIFFERENT links, because a harness that
    only ever fails one way is only testing one thing:

      * the four step pitches written to the wrong row of knobs. The file
        still says four distinct pitches and the engine still holds all four,
        so the structural check is CLEAN -- and the sequencer plays the row
        that is patched, which is empty. Only the written-steps check can
        catch this, and it is the exact failure the whole exercise was called
        to look for.

      * the cable from the sequencer to the oscillator cut -- the sequencer
        still emits its four voltages and the oscillator still makes a tone,
        so both taps record something. What breaks is the correspondence
        between them, which is the audible claim.

    Flattening the written steps instead is NOT a sabotage, and reading like
    one cost a cycle here: a patch edited to ask for one pitch and then heard
    playing one pitch is faithful. A sabotage has to make what plays differ
    from what was written, not change both together.
    """
    bad = 0

    v = _run_patch(fixture(), "control", probe)
    if v is None:
        return 1 + check(False, "the control patch ran at all")
    for line in v.lines:
        print("      " + line)
    bad += check(v.ok, "the fixture passes every claim")
    bad += check(len(F.distinct(v.notes)) == 4,
                 "four distinct pitches are heard", str(v.distinct))

    misaddressed = fixture()
    for p in misaddressed["modules"][SEQ]["params"]:
        if p["id"] in (4, 5, 6, 7):
            p["id"] += 8                # the same pitches, on CV row 2
    v = _run_patch(misaddressed, "wrong-row sabotage", probe)
    if v is None:
        bad += check(False, "the wrong-row sabotage ran at all")
    else:
        bad += check(not v.ok and bool(v.played_why),
                     "steps written to a row nothing is patched to are "
                     "rejected", f"ok={v.ok} played={v.played_why!r}")
        bad += check(v.structural == [],
                     "and the structural check stays clean, so only listening "
                     "could have caught it", str(v.structural))
        bad += check(len(F.distinct(v.notes)) == 1,
                     "and only one pitch is actually heard", str(v.distinct))

    cut = fixture()
    cut["cables"] = [c for c in cut["cables"]
                     if not (c["outputModuleId"] == SEQ
                             and c["inputModuleId"] == VCO)]
    v = _run_patch(cut, "cut-cable sabotage", probe)
    if v is None:
        bad += check(False, "the cut-cable sabotage ran at all")
    else:
        bad += check(not v.ok and bool(v.audible_why),
                     "a patch whose pitch never reaches the oscillator is "
                     "rejected", f"ok={v.ok} audible={v.audible_why!r}")
        bad += check(len(F.distinct(v.notes)) == 1,
                     "and the oscillator is heard sitting on one pitch",
                     str(v.distinct))
    return bad


def test_audio_artifact_contains_only_audio_taps() -> int:
    """The Quality Lab artifact must not accidentally include pitch CV."""
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "render.wav")
        meta = F.write_audio_artifact(
            path,
            [[0.0, 2.5, -5.0], [0.0, 1.0, 2.0], [0.0, -1.0, 1.0]],
            [(1, 0, "audio"), (2, 0, "pitch"), (1, 1, "audio")],
            48000.0,
        )
        with wave.open(path, "rb") as wav:
            bad = check(wav.getnchannels() == 2,
                        "only the two listener-audio taps become WAV channels")
            bad += check(wav.getnframes() == 3 and wav.getframerate() == 48000,
                         "the artifact keeps the capture length and sample rate")
        bad += check(meta["source_peak_volts"] == 5.0,
                     "artifact metadata preserves the original voltage peak")
        bad += check(meta["source_rms_volts"] > F.SILENCE,
                     "artifact metadata preserves the original audibility")
        bad += check(len(meta["source_rms_volts_by_channel"]) == 2
                     and meta["source_rms_volts_by_channel"][0]
                     != meta["source_rms_volts_by_channel"][1],
                     "artifact metadata preserves audibility per channel")
        with open(meta["fidelity_metadata"]) as source:
            sidecar = json.load(source)
        bad += check(sidecar["source_rms_volts"] == meta["source_rms_volts"]
                     and sidecar["source_rms_volts_by_channel"]
                     == meta["source_rms_volts_by_channel"]
                     and sidecar["minimum_source_rms_volts"] == F.SILENCE,
                     "the WAV is content-bound to its pre-normalized level")
        try:
            F.write_audio_artifact(
                os.path.join(d, "silent.wav"), [[1e-6] * 100],
                [(1, 0, "audio")], 48000.0)
            bad += check(False, "peak normalization cannot amplify silence")
        except ValueError as exc:
            bad += check("audibility floor" in str(exc),
                         "peak normalization cannot amplify silence", str(exc))
        return bad


def main(argv: list[str]) -> int:
    bad = 0
    for fn in (test_a_faithful_load_reads_as_faithful,
               test_a_clamped_value_is_caught,
               test_a_default_that_overwrote_a_written_value_is_caught,
               test_a_cable_on_the_wrong_port_is_caught,
               test_no_readback_is_not_a_pass,
               test_the_audio_interface_is_removed,
               test_the_probe_takes_over_from_the_speakers,
               test_the_caller_patch_is_not_edited,
               test_step_knobs_are_told_from_their_trigger_buttons,
               test_pitch_inputs_are_recognised_by_name,
               test_unmapped_modules_say_so_rather_than_report_no_steps,
               test_written_steps_are_read_off_the_patch,
               test_a_value_outside_a_knobs_range_is_named_before_launching,
               test_a_known_tone_reads_as_its_own_pitch,
               test_the_reader_is_accurate_across_the_range_it_claims,
               test_nothing_periodic_is_not_a_pitch,
               test_silence_is_not_a_note,
               test_a_plucked_note_is_not_thrown_away,
               test_a_sequence_of_tones_reads_as_that_sequence,
               test_one_stray_window_is_not_a_note,
               test_a_run_is_reported_by_its_middle_not_its_edge,
               test_tracking_needs_both_taps,
               test_tracking_is_blind_to_tuning_but_not_to_intervals,
               test_scatter_is_not_reported_as_mistuning,
               test_a_capture_is_read_back_channel_by_channel,
               test_audio_artifact_contains_only_audio_taps):
        print(f"{fn.__name__}:")
        bad += fn()

    if "--with-rack" in argv:
        import shutil
        import tempfile
        if not mr.rack_binary():
            print("test_with_rack:\n  SKIP  no Rack on this machine")
        else:
            print("test_with_rack:")
            stage = tempfile.mkdtemp(prefix="fid-probe-")
            try:
                bad += test_with_rack(F.build_probe(stage))
            finally:
                shutil.rmtree(stage, ignore_errors=True)

    print("\n" + ("all good" if bad == 0 else f"FAILED ({bad})"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
