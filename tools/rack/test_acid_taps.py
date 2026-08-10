#!/usr/bin/env python3
"""Shape tests for the strict, synchronized acid tap contract."""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import acid_taps as A                                      # noqa: E402


def check(ok: bool, label: str, detail="") -> int:
    print(f"  {'ok    ' if ok else 'WRONG '} {label}"
          + (f" — {detail}" if detail and not ok else ""))
    return 0 if ok else 1


def inventory() -> dict:
    def module(tags, inputs=(), outputs=(), roles_in=(), roles_out=()):
        return {"tags": list(tags), "inputs": list(inputs),
                "outputs": list(outputs), "roles_in": list(roles_in),
                "roles_out": list(roles_out)}

    modules = {
        "Clock": module(["LFO"], outputs=["Square"], roles_out=[["Clock"]]),
        "Seq": module(["Sequencer"], inputs=["Clock"],
                      outputs=["Trigger", "Pitch", "Accent", "Slide"],
                      roles_in=["Clock"],
                      roles_out=["Trigger", "Cv", "Cv", "Cv"]),
        "Slew": module(["Slew limiter"], inputs=["Voltage", "Gate"],
                       outputs=["Glide"], roles_in=["Cv", "Gate"],
                       roles_out=["Cv"]),
        "VCO": module(["VCO"], inputs=["V/OCT"], outputs=["Saw"],
                      roles_in=["Pitch"], roles_out=["Audio"]),
        "Env": module(["Envelope generator"], inputs=["Gate"],
                      outputs=["Envelope"], roles_in=["Gate"],
                      roles_out=["Cv"]),
        "Mix": module(["Mixer"], inputs=["A", "B"], outputs=["Mix"],
                      roles_in=["Cv", "Cv"], roles_out=["Cv"]),
        "Filter": module(["VCF"], inputs=["Cutoff", "Audio"],
                         outputs=["Lowpass"], roles_in=["Cv", "Audio"],
                         roles_out=["Audio"]),
        "VCA": module(["VCA"], inputs=["CV", "Audio"], outputs=["Audio"],
                      roles_in=["Cv", "Audio"], roles_out=["Audio"]),
    }
    return {"Fixture": {"modules": modules}}


def patch() -> dict:
    names = ("Clock", "Seq", "Slew", "VCO", "Env", "Mix", "Filter", "VCA")
    modules = [{"id": i + 1, "plugin": "Fixture", "model": name}
               for i, name in enumerate(names)]
    modules.append({"id": 9, "plugin": "Core", "model": "AudioInterface2"})

    def cable(source, out, destination, inp):
        return {"outputModuleId": source, "outputId": out,
                "inputModuleId": destination, "inputId": inp}

    return {"modules": modules, "cables": [
        cable(1, 0, 2, 0),       # clock -> sequencer
        cable(2, 1, 3, 0),       # raw pitch -> slew
        cable(2, 3, 3, 1),       # slide lane -> slew gate
        cable(3, 0, 4, 0),       # post-slew pitch -> oscillator
        cable(4, 0, 7, 1),       # oscillator -> filter
        cable(2, 0, 5, 0),       # sequencer trigger -> envelope
        cable(2, 2, 6, 0),       # accent's physical origin
        cable(5, 0, 6, 1),       # envelope also shapes cutoff
        cable(6, 0, 7, 0),       # effective combined cutoff
        cable(7, 0, 8, 1),       # filter -> amplifier
        cable(5, 0, 8, 0),       # envelope -> amplifier CV
        cable(8, 0, 9, 0),       # final audio
    ]}


def test_ordered_plan_preserves_origins_across_transparent_modules() -> int:
    result = A.plan(patch(), inventory())
    expected = [(1, 0, "clock"), (2, 1, "raw_pitch"),
                (3, 0, "post_slew_pitch"), (2, 2, "accent"),
                (2, 3, "slide"), (6, 0, "effective_cutoff"),
                (7, 0, "filter_audio"), (8, 0, "final_audio")]
    bad = check(result.status == A.READY and result.taps == expected,
                "all eight witnesses have deterministic semantic order",
                result.as_dict())
    accent = result.witnesses.get("accent", {})
    cutoff = result.witnesses.get("effective_cutoff", {})
    bad += check(accent.get("physical_origin") == [2, 2]
                 and cutoff.get("module") == 6
                 and cutoff.get("physical_origin") == [2, 2],
                 "the accent lane survives the mixer while cutoff taps its output",
                 result.witnesses)
    return bad


def test_shared_sequencer_lane_is_a_measured_failure() -> int:
    broken = patch()
    accent = next(c for c in broken["cables"]
                  if c["outputModuleId"] == 2 and c["inputModuleId"] == 6)
    accent["outputId"] = 1
    result = A.plan(broken, inventory())
    capture = {name: [0.0, 2.0, 0.0] for name in A.ORDER}
    verdict = A.evaluate_capture(result, capture)
    return check(result.status == A.READY and verdict["verdict"] == A.FAIL
                 and "share a physical" in verdict["reasons"][0],
                 "one lane wearing pitch and accent labels fails", verdict)


def test_missing_clock_is_unmeasured_not_guessed() -> int:
    broken = patch()
    broken["cables"] = [c for c in broken["cables"]
                        if not (c["outputModuleId"] == 1
                                and c["inputModuleId"] == 2)]
    result = A.plan(broken, inventory())
    return check(result.status == A.UNMEASURED and not result.taps
                 and any(reason.startswith("clock:") for reason in result.reasons),
                 "a missing sequencer clock is explicitly UNMEASURED",
                 result.as_dict())


def test_missing_cutoff_witness_is_unmeasured_not_a_raw_accent_guess() -> int:
    broken = patch()
    broken["cables"] = [c for c in broken["cables"]
                        if not (c["outputModuleId"] == 6
                                and c["inputModuleId"] == 7)]
    result = A.plan(broken, inventory())
    return check(result.status == A.UNMEASURED and not result.taps
                 and any(reason.startswith("accent:") for reason in result.reasons),
                 "no filter-CV path means no effective-cutoff witness",
                 result.as_dict())


def test_an_uncartographed_cutoff_port_is_unmeasured() -> int:
    inv = inventory()
    inv["Fixture"]["modules"]["Filter"]["inputs"] = []
    inv["Fixture"]["modules"]["Filter"]["roles_in"] = []
    result = A.plan(patch(), inv)
    return check(result.status == A.UNMEASURED
                 and any(reason.startswith("accent:") for reason in result.reasons),
                 "a cable index cannot substitute for Cartog evidence",
                 result.as_dict())


def test_capture_requires_every_synchronized_series() -> int:
    result = A.plan(patch(), inventory())
    series = {name: [0.0, 2.0, 0.0] for name in A.ORDER if name != "slide"}
    verdict = A.evaluate_capture(result, series)
    bad = check(verdict["verdict"] == A.UNMEASURED
                and verdict["reasons"] == ["slide: synchronized series is missing"],
                "a missing recorded channel cannot pass", verdict)
    series["slide"] = [0.0, 2.0]
    verdict = A.evaluate_capture(result, series)
    bad += check(verdict["verdict"] == A.UNMEASURED
                 and any("lengths differ" in r for r in verdict["reasons"]),
                 "series recorded on different time bases cannot pass", verdict)
    series["slide"] = [0.0, 2.0, 0.0]
    verdict = A.evaluate_capture(result, series)
    bad += check(verdict["verdict"] == A.PASS
                 and "not 303 quality" in verdict["note"],
                 "complete capture passes only the stated measurement contract",
                 verdict)
    return bad


def main() -> int:
    bad = 0
    for function in (
            test_ordered_plan_preserves_origins_across_transparent_modules,
            test_shared_sequencer_lane_is_a_measured_failure,
            test_missing_clock_is_unmeasured_not_guessed,
            test_missing_cutoff_witness_is_unmeasured_not_a_raw_accent_guess,
            test_an_uncartographed_cutoff_port_is_unmeasured,
            test_capture_requires_every_synchronized_series):
        print(f"{function.__name__}:")
        bad += function()
    print("\n" + ("all good" if not bad else f"FAILED ({bad})"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
