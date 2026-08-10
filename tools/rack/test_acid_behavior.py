#!/usr/bin/env python3
"""Deterministic mutation tests for the measured acid behavior contract."""

import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import acid_behavior as B                                  # noqa: E402
import acid_taps as A                                      # noqa: E402


PERIOD = 64
LEAD = 9
PITCHES = (0.0, 0.5, 0.0, 0.5, 0.0, -0.25, 0.0, 0.5)
ACCENTED = {0, 4}
SLID = {1, 6}


def check(ok: bool, label: str, detail="") -> int:
    print(f"  {'ok    ' if ok else 'WRONG '} {label}"
          + (f" — {detail}" if detail and not ok else ""))
    return 0 if ok else 1


def ready_plan(failures=None, witnesses=None) -> A.Plan:
    taps = [(index, 0, name) for index, name in enumerate(A.ORDER)]
    return A.Plan(A.READY, taps, witnesses or {}, [], list(failures or []))


def fixture() -> dict[str, list[float]]:
    edges = [LEAD + step * PERIOD for step in range(17)]
    count = edges[-1] + 8
    series = {name: [0.0] * count for name in A.ORDER}
    for edge in edges:
        for sample in range(edge, min(edge + 5, count)):
            series["clock"][sample] = 5.0

    for absolute_step, (left, right) in enumerate(zip(edges, edges[1:])):
        step = absolute_step % 8
        pitch = PITCHES[step]
        previous = PITCHES[(step - 1) % 8]
        for offset, sample in enumerate(range(left, right)):
            position = offset / PERIOD
            series["raw_pitch"][sample] = pitch
            series["accent"][sample] = 5.0 if step in ACCENTED else 0.0
            series["slide"][sample] = 5.0 if step in SLID else 0.0
            if step in SLID and pitch != previous and offset < PERIOD // 4:
                progress = offset / (PERIOD // 4)
                series["post_slew_pitch"][sample] = (
                    previous + (pitch - previous) * progress)
            else:
                series["post_slew_pitch"][sample] = pitch

            envelope = math.exp(-5.0 * position)
            accent = 1.1 * envelope if step in ACCENTED else 0.0
            series["effective_cutoff"][sample] = 0.7 + 0.18 * envelope + accent

            phase = 2.0 * math.pi * (3.0 + pitch * 2.0) * position
            base = math.sin(phase)
            bright = math.sin(phase * 4.0)
            color = 0.42 if step in ACCENTED else 0.035
            transient = 1.0 + (0.9 * envelope if step in ACCENTED else 0.0)
            filtered = (base + color * bright) * 1.8
            series["filter_audio"][sample] = filtered
            series["final_audio"][sample] = filtered * transient * 0.7
    return series


def has_reason(result: dict, text: str) -> bool:
    return any(text in reason for reason in result["reasons"])


def erase_accent_response(series: dict[str, list[float]], name: str) -> None:
    plain_for_accented = {0: 2, 4: 2}
    values = series[name]
    for absolute_step in range(16):
        step = absolute_step % 8
        if step not in plain_for_accented:
            continue
        loop_start = absolute_step - step
        source = LEAD + (loop_start + plain_for_accented[step]) * PERIOD
        target = LEAD + absolute_step * PERIOD
        values[target:target + PERIOD] = values[source:source + PERIOD]


def set_step(series: dict[str, list[float]], name: str, step: int,
             value) -> None:
    for loop in range(2):
        start = LEAD + (loop * 8 + step) * PERIOD
        series[name][start:start + PERIOD] = [value] * PERIOD


def glide_step(series: dict[str, list[float]], step: int,
               loops=range(2)) -> None:
    previous = PITCHES[step - 1]
    target = PITCHES[step]
    for loop in loops:
        start = LEAD + (loop * 8 + step) * PERIOD
        for offset in range(PERIOD // 4):
            progress = offset / (PERIOD // 4)
            series["post_slew_pitch"][start + offset] = (
                previous + (target - previous) * progress)


def plateau_step(series: dict[str, list[float]], step: int) -> None:
    previous = PITCHES[step - 1]
    target = PITCHES[step]
    midpoint = previous + (target - previous) * 0.5
    for loop in range(2):
        start = LEAD + (loop * 8 + step) * PERIOD
        series["post_slew_pitch"][start:start + PERIOD // 4] = (
            [midpoint] * (PERIOD // 4))


def test_strong_fixture_passes() -> int:
    result = B.evaluate(ready_plan(), fixture())
    observation = result["observations"]
    return check(result["verdict"] == B.PASS
                 and observation["clock"]["complete_loops"] == 2
                 and len(observation["raw_pitch"]["distinct_pitches"]) >= 3
                 and observation["accent"]["matched_pairs"][0]["passes"],
                 "two loops prove pitches, selective slew, cutoff, and audio",
                 result)


def test_free_running_phase_does_not_make_articulation_stale() -> int:
    series = fixture()
    for loop, scale in enumerate((0.65, 1.35)):
        start = LEAD + loop * 8 * PERIOD
        end = start + 8 * PERIOD
        series["final_audio"][start:end] = [
            value * scale for value in series["final_audio"][start:end]]
    result = B.evaluate(ready_plan(), series)
    return check(result["verdict"] == B.PASS,
                 "free-running oscillator phase may vary articulated level",
                 result)


def test_process_slew_active_low_selection_passes() -> int:
    series = fixture()
    series["slide"] = [0.0 if value > 1.0 else 10.0
                       for value in series["slide"]]
    plan = ready_plan(witnesses={"slide": {"active_polarity": "low"}})
    result = B.evaluate(plan, series)
    return check(result["verdict"] == B.PASS
                 and result["observations"]["slide"]["active_polarity"] == "low",
                 "Process SLEW realizes same-edge selection with active-low gate",
                 result)


def test_cut_and_bypass_fail() -> int:
    cut = fixture()
    cut["final_audio"] = [0.0] * len(cut["final_audio"])
    cut["final_audio"][:LEAD] = [1.0] * LEAD
    result = B.evaluate(ready_plan(), cut)
    bad = check(result["verdict"] == B.FAIL
                and has_reason(result, "final_audio: the captured signal is silent"),
                "a cut listener path fails", result)

    for name in ("filter_audio", "final_audio"):
        bypass = fixture()
        erase_accent_response(bypass, name)
        result = B.evaluate(ready_plan(), bypass)
        bad += check(result["verdict"] == B.FAIL
                     and has_reason(result, "filter/final-audio"),
                     f"a bypassed {name} accent response fails", result)

    amplifier_bypass = fixture()
    amplifier_bypass["final_audio"] = list(amplifier_bypass["filter_audio"])
    result = B.evaluate(ready_plan(), amplifier_bypass)
    bad += check(result["verdict"] == B.FAIL
                 and has_reason(result, "bypasses the amplifier"),
                 "an exact filter-to-final amplifier bypass fails", result)
    return bad


def test_unsafe_final_output_fails_without_weakening_behavior_checks() -> int:
    unsafe = fixture()
    unsafe["final_audio"] = [value * 12.0 for value in unsafe["final_audio"]]
    result = B.evaluate(ready_plan(), unsafe)
    return check(result["verdict"] == B.FAIL
                 and has_reason(result, "output safety limit")
                 and result["observations"]["final_audio"]["peak_volts"] > 20.0,
                 "an acid behavior pass cannot hide unsafe final voltage", result)


def test_rewired_slew_and_zero_cutoff_fail() -> int:
    rewired = fixture()
    rewired["post_slew_pitch"] = list(rewired["raw_pitch"])
    result = B.evaluate(ready_plan(), rewired)
    bad = check(result["verdict"] == B.FAIL
                and has_reason(result, "does not show a repeatable"),
                "a post-slew tap rewired before the slew fails", result)

    stepped = fixture()
    plateau_step(stepped, 1)
    result = B.evaluate(ready_plan(), stepped)
    bad += check(result["verdict"] == B.FAIL
                 and has_reason(result, "does not show a repeatable"),
                 "a held midpoint followed by a jump is not a glide", result)

    zero = fixture()
    zero["effective_cutoff"] = [0.0] * len(zero["effective_cutoff"])
    result = B.evaluate(ready_plan(), zero)
    bad += check(result["verdict"] == B.FAIL
                 and has_reason(result, "captured cutoff is zero"),
                 "zero cutoff evidence fails", result)

    flat = fixture()
    flat["effective_cutoff"] = [0.7] * len(flat["effective_cutoff"])
    flat["effective_cutoff"][:LEAD] = [3.0] * LEAD
    result = B.evaluate(ready_plan(), flat)
    bad += check(result["verdict"] == B.FAIL
                 and has_reason(result, "captured cutoff is flat"),
                 "nonzero but flat cutoff evidence fails", result)
    return bad


def test_all_on_selectors_fail() -> int:
    bad = 0
    for name in ("accent", "slide"):
        mutated = fixture()
        mutated[name] = [5.0] * len(mutated[name])
        result = B.evaluate(ready_plan(), mutated)
        bad += check(result["verdict"] == B.FAIL
                     and has_reason(result, "every step is selected"),
                     f"all-on {name} is not selective behavior", result)
    return bad


def test_pitch_and_selector_coverage_fail() -> int:
    two_pitch = fixture()
    for name in ("raw_pitch", "post_slew_pitch"):
        two_pitch[name] = [0.0 if value < 0.0 else value
                           for value in two_pitch[name]]
    result = B.evaluate(ready_plan(), two_pitch)
    bad = check(result["verdict"] == B.FAIL
                and has_reason(result, "need at least 3"),
                "a two-pitch loop is inadequate acid behavior", result)

    for name in ("accent", "slide"):
        empty = fixture()
        empty[name] = [0.0] * len(empty[name])
        result = B.evaluate(ready_plan(), empty)
        bad += check(result["verdict"] == B.FAIL
                     and has_reason(result, f"{name}: no steps are selected"),
                     f"empty {name} selection fails", result)
    return bad


def test_shared_lane_is_a_separate_structural_failure() -> int:
    failure = "pitch, accent, and slide share a physical sequencer lane"
    result = B.evaluate(ready_plan([failure]), fixture())
    return check(result["verdict"] == B.FAIL
                 and result["structural_failures"] == [failure]
                 and not result["behavior_failures"]
                 and not result["measurement_failures"],
                 "shared physical lanes remain structural hard failures", result)


def test_wrong_timebase_and_stale_loop_are_unmeasured() -> int:
    wrong = fixture()
    wrong["slide"] = wrong["slide"][::2]
    result = B.evaluate(ready_plan(), wrong)
    bad = check(result["verdict"] == B.UNMEASURED
                and has_reason(result, "not on one timebase"),
                "a channel recorded on a different timebase is unmeasured", result)

    stale = fixture()
    second_loop = LEAD + 8 * PERIOD
    stale["raw_pitch"][second_loop:second_loop + PERIOD] = [0.25] * PERIOD
    result = B.evaluate(ready_plan(), stale)
    bad += check(result["verdict"] == B.UNMEASURED
                 and has_reason(result, "stale or mismatched series"),
                 "two loop signatures must agree", result)

    return bad


def test_wraparound_slide_transition_is_measured() -> int:
    selected = fixture()
    set_step(selected, "slide", 0, 5.0)
    result = B.evaluate(ready_plan(), selected)
    bad = check(result["verdict"] == B.FAIL
                and has_reason(result, "selected transition into step 0"),
                "selected step 0 must glide from the preceding loop", result)

    unselected = fixture()
    set_step(unselected, "slide", 4, 5.0)
    glide_step(unselected, 4)
    glide_step(unselected, 0)
    result = B.evaluate(ready_plan(), unselected)
    bad += check(result["verdict"] == B.FAIL
                 and has_reason(result, "unselected transition into step 0"),
                 "unselected step 0 must not glide across the loop boundary",
                 result)
    return bad


def test_inadequate_coverage_is_unmeasured() -> int:
    short = fixture()
    end = LEAD + 12 * PERIOD
    short = {name: values[:end] for name, values in short.items()}
    result = B.evaluate(ready_plan(), short)
    return check(result["verdict"] == B.UNMEASURED
                 and has_reason(result, "need at least 16"),
                 "less than two complete loops cannot prove behavior", result)


def main() -> int:
    bad = 0
    for function in (
            test_strong_fixture_passes,
            test_free_running_phase_does_not_make_articulation_stale,
            test_process_slew_active_low_selection_passes,
            test_cut_and_bypass_fail,
            test_unsafe_final_output_fails_without_weakening_behavior_checks,
            test_rewired_slew_and_zero_cutoff_fail,
            test_all_on_selectors_fail,
            test_pitch_and_selector_coverage_fail,
            test_shared_lane_is_a_separate_structural_failure,
            test_wrong_timebase_and_stale_loop_are_unmeasured,
            test_wraparound_slide_transition_is_measured,
            test_inadequate_coverage_is_unmeasured):
        print(f"{function.__name__}:")
        bad += function()
    print("\n" + ("all good" if not bad else f"FAILED ({bad})"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
