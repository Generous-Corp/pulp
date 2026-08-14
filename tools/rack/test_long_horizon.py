#!/usr/bin/env python3
"""Long-duration prompt contracts must survive real-DSP acceptance."""

from __future__ import annotations

import json
import math
import os
import pathlib
import sys
import tempfile
from collections import namedtuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch as P  # noqa: E402


def cable(level: float, centroid: float, *, centroid_range: float = 0.3,
          onsets: float = 0.0, peak: float | None = None,
          duty: float = 1.0, periodicity: float = 0.0,
          interval_cv: float = 1.0, pitch_changes: int = 0,
          pitch_hz: float = 0.0, period_ms: float = 0.0) -> dict:
    return {
        "finite": True,
        "mean_abs_v": level,
        "peak_abs_v": peak if peak is not None else level * 2.0,
        "dynamics": {"mean_rms": level, "duty_cycle": duty},
        "pitch": {"distinct_pitches": 1, "pitch_changes": pitch_changes,
                  "median_hz": pitch_hz},
        "onsets": {"per_second": onsets, "onsets": onsets * 6.0,
                   "periodicity": periodicity, "interval_cv": interval_cv,
                   "period_ms": period_ms},
        "spectrum": {"centroid_mean_hz": centroid,
                     "centroid_range_octaves": centroid_range},
    }


def report(rows: list[tuple[float, list[dict]]]) -> str:
    payload = {
        "window_seconds": 6.0,
        "checkpoints": [
            {"start_seconds": start,
             "report": {"schema": 1, "cables": cables,
                        "pairwise": ([{"left": 0, "right": 1,
                                       "correlation": 0.2}]
                                     if len(cables) >= 2 else [])}}
            for start, cables in rows
        ],
    }
    return P.LONG_HORIZON_MARKER + json.dumps(payload, separators=(",", ":"))


def evolving_drone_contract() -> P.RuntimeQualityContract:
    return P.compile_runtime_quality_contract(
        "Several complementary layers form a sustained drone for 10 minutes. "
        "Avoid obvious sequencing, make the timbre slowly evolve, and keep "
        "independent stereo movement.")


def layer_inventory() -> dict:
    return {
        "Test": {"modules": {
            "Tone": {"tags": ["Oscillator"], "roles_out": ["Audio"]},
            "Mix": {"tags": ["Mixer"],
                    "roles_in": ["Audio", "Audio", "Audio"],
                    "roles_out": ["Audio"]}}},
        "Core": {"modules": {
            "AudioInterface2": {"roles_in": ["Audio", "Audio"]}}},
    }


def layer_patch(two_sources: bool) -> dict:
    modules = [
        {"id": 1, "plugin": "Test", "model": "Tone"},
        {"id": 4, "plugin": "Core", "model": "AudioInterface2"},
    ]
    cables = [
        {"outputModuleId": 1, "outputId": 0,
         "inputModuleId": 4, "inputId": 0},
    ]
    if two_sources:
        modules.append({"id": 2, "plugin": "Test", "model": "Tone"})
        right_source = 2
    else:
        right_source = 1
    cables.append({"outputModuleId": right_source, "outputId": 0,
                   "inputModuleId": 4, "inputId": 1})
    return {"modules": modules, "cables": cables}


def check_long_horizon_contract() -> tuple[int, int]:
    bad, ran = 0, 35
    contract = evolving_drone_contract()
    if contract != P.RuntimeQualityContract(
            sustained=True, no_obvious_sequence=True,
            multiple_audible_layers=True, minimum_audible_layers=2,
            spectral_evolution=True, evolving=True, stereo=True,
            decorrelated_stereo=True, duration_seconds=600.0):
        bad += 1
        print(f"  WRONG  compositional drone constraints compiled as {contract}")
    else:
        print("  ok     sustained/layers/no-sequence/spectral/stereo/duration compile")

    constant = P.compile_runtime_quality_contract(
        "Hold a constant calibration tone for 10 minutes")
    if constant.duration_seconds != 600.0 or constant.evolving:
        bad += 1
        print(f"  WRONG  duration alone invented an evolution request: {constant}")
    else:
        print("  ok     a long constant-tone request does not invent evolution")

    direct = P.compile_runtime_quality_contract(
        "An evolving drone with a stereo field")
    if not direct.evolving or P.runtime_quality_seconds(direct) != 60.0:
        bad += 1
        print(f"  WRONG  direct/no-duration evolution escaped runtime proof: {direct}")
    else:
        print("  ok     direct evolving language gets a bounded 60-second proof")

    never_repeat = P.compile_runtime_quality_contract(
        "An ambient generative drone that never repeats")
    if not never_repeat.sustained or not never_repeat.nonrepeating or \
            P.runtime_quality_seconds(never_repeat) != 60.0:
        bad += 1
        print(f"  WRONG  never-repeat promise escaped a measured contract: {never_repeat}")
    else:
        print("  ok     never-repeat language requires entropy and real-DSP qualification")

    if P.generation_attempt_count(
            saved_response=False, claimed_gating=True,
            module_contract=False, retries=1,
            quality_contract=direct) != 2 or \
            P.generation_attempt_count(
                saved_response=True, claimed_gating=True,
                module_contract=False, retries=1,
                quality_contract=direct) != 1:
        bad += 1
        print("  WRONG  no-duration measured intent lost its bounded redesign retry")
    else:
        print("  ok     no-duration named quality retains one redesign retry")

    negated = [P.compile_runtime_quality_contract(text) for text in (
        "Not a drone; make a stereo melody",
        "Not a drone but a stereo melody",
        "Choose a stereo melody rather than a drone")]
    if any(item.sustained or item.no_obvious_sequence or not item.stereo
           for item in negated):
        bad += 1
        print(f"  WRONG  negated/cross-clause properties inverted intent: {negated}")
    else:
        print("  ok     negation and semicolons cannot leak into another property")

    preserved = [P.compile_runtime_quality_contract(text) for text in (
        "Evolve without losing stereo",
        "Avoid collapsing the stereo field",
        "Do not remove the sustained layer")]
    if not preserved[0].stereo or not preserved[1].stereo or \
            not preserved[2].sustained:
        bad += 1
        print(f"  WRONG  preservation verbs erased affirmative constraints: {preserved}")
    else:
        print("  ok     negated failure verbs preserve affirmative properties")

    wide = P.compile_runtime_quality_contract("Create a wide field")
    if not wide.stereo or not wide.decorrelated_stereo:
        bad += 1
        print(f"  WRONG  ordinary wide stereo omitted decorrelation: {wide}")
    else:
        print("  ok     ordinary wide stereo requires measured decorrelation")

    counted = P.compile_runtime_quality_contract(
        "Build four independent audible layers")
    if counted.minimum_audible_layers != 4:
        bad += 1
        print(f"  WRONG  explicit layer count collapsed: {counted}")
    else:
        print("  ok     explicit layer counts survive compilation")

    if P.long_horizon_seconds("make a slowly evolving drone") is not None:
        bad += 1
        print("  WRONG  vague slowness silently opted into an expensive proof")
    else:
        print("  ok     only an explicit numeric duration enables the long proof")

    checkpoints = P.long_horizon_checkpoints(600.0)
    if checkpoints != [0.0, 60.0, 300.0, 594.0]:
        bad += 1
        print(f"  WRONG  ten-minute checkpoints are not well-spaced: {checkpoints}")
    else:
        print("  ok     four windows span the beginning, arc, and tail")

    flat = report([(0, [cable(1.0, 500, centroid_range=.04)]),
                   (60, [cable(1.02, 505, centroid_range=.04)]),
                   (300, [cable(1.0, 498, centroid_range=.05)]),
                   (594, [cable(1.01, 502, centroid_range=.04)])])
    errors = P.long_horizon_evolution_errors(flat)
    if len(errors) < 2:
        bad += 1
        print(f"  WRONG  invariant output escaped corroborated/timbral gates: {errors}")
    else:
        print("  ok     evolution needs two dimensions and material timbral motion")

    if P.long_horizon_evolution_errors(flat, constant):
        bad += 1
        print("  WRONG  the constant-tone contract was judged as evolving")
    else:
        print("  ok     non-evolving long-form control remains eligible")

    pan_only = P.compile_runtime_quality_contract(
        "Make the stereo balance change over time")
    pan_report = report([
        (0, [cable(.2, 500), cable(.2, 500)]),
        (15, [cable(1.0, 500), cable(.2, 500)]),
        (30, [cable(.2, 500), cable(1.0, 500)]),
        (54, [cable(1.0, 500), cable(.2, 500)]),
    ])
    if pan_only.spectral_evolution or \
            P.long_horizon_evolution_errors(pan_report, pan_only):
        bad += 1
        print("  WRONG  non-spectral evolution was forced to move centroid")
    else:
        print("  ok     generic evolution need not invent spectral motion")

    pitch_motion = report([
        (0, [cable(.2, 500, pitch_hz=220)]),
        (15, [cable(1.0, 500, pitch_hz=330)]),
        (30, [cable(.3, 500, pitch_hz=440)]),
        (54, [cable(1.0, 500, pitch_hz=330)]),
    ])
    generic = P.compile_runtime_quality_contract("Let the sound evolve over time")
    if P.long_horizon_evolution_errors(pitch_motion, generic):
        bad += 1
        print("  WRONG  material slow pitch motion could not prove evolution")
    else:
        print("  ok     slow pitch motion is a general evolution dimension")

    rate_motion = report([
        (0, [cable(.2, 500, onsets=1, periodicity=.7, period_ms=1000)]),
        (15, [cable(1.0, 500, onsets=2, periodicity=.7, period_ms=500)]),
        (30, [cable(.3, 500, onsets=4, periodicity=.7, period_ms=250)]),
        (54, [cable(1.0, 500, onsets=2, periodicity=.7, period_ms=500)]),
    ])
    if P.long_horizon_evolution_errors(rate_motion, generic):
        bad += 1
        print("  WRONG  reliable event-rate motion could not prove evolution")
    else:
        print("  ok     reliable event-period motion is a general evolution dimension")

    # Stable tonal anchor is allowed: a second path remains within 20 dB and
    # supplies real spectral motion while combined level and centroid both move.
    evolving = report([
        (0, [cable(1.0, 500, centroid_range=.05), cable(.2, 300, centroid_range=.25)]),
        (60, [cable(1.0, 500, centroid_range=.05), cable(1.0, 950, centroid_range=.35)]),
        (300, [cable(1.0, 500, centroid_range=.05), cable(.3, 420, centroid_range=.30)]),
        (594, [cable(1.0, 500, centroid_range=.05), cable(1.2, 1100, centroid_range=.40)]),
    ])
    if P.long_horizon_evolution_errors(evolving):
        bad += 1
        print("  WRONG  stable anchor plus material evolving layer was rejected")
    else:
        print("  ok     stable anchor may coexist with a material evolving layer")

    stationary_broadband = report([
        (0, [cable(.2, 500, centroid_range=.8), cable(.2, 500, centroid_range=.8)]),
        (60, [cable(1.0, 500, centroid_range=.8), cable(.3, 500, centroid_range=.8)]),
        (300, [cable(.3, 500, centroid_range=.8), cable(1.0, 500, centroid_range=.8)]),
        (594, [cable(1.0, 500, centroid_range=.8), cable(.2, 500, centroid_range=.8)]),
    ])
    if not any("timbrally invariant" in error for error in
               P.long_horizon_evolution_errors(stationary_broadband, contract)):
        bad += 1
        print("  WRONG  stationary broadband variance impersonated evolution")
    else:
        print("  ok     within-window broadband variance is not long-term evolution")

    fixture = pathlib.Path(__file__).with_name("test_fixtures") / \
        "long-horizon" / "dsp-600-v3-4b9f0aa3.json"
    m5 = P.LONG_HORIZON_MARKER + json.dumps(
        json.loads(fixture.read_text()), separators=(",", ":"))
    m5_payload = json.loads(fixture.read_text())
    old_balances = []
    for checkpoint in m5_payload["checkpoints"]:
        left, right = checkpoint["report"]["cables"][:2]
        old_balances.append(20.0 * math.log10(
            left["mean_abs_v"] / right["mean_abs_v"]))
    if max(old_balances) - min(old_balances) < 3.0:
        bad += 1
        print("  WRONG  fixture does not reproduce the old balance-only pass")
    else:
        print("  ok     fixture reproduces the old near-silent balance-only pass")
    layer_paths = [{101}, {202}]
    m5_errors = P.long_horizon_evolution_errors(m5, contract, layer_paths)
    joined = "\n".join(m5_errors)
    if not all(word in joined for word in
               ("two independent dimensions", "timbrally invariant", "stereo",
                "fast event stream")):
        bad += 1
        print(f"  WRONG  exact M5 negative graduated or feedback is incomplete: {m5_errors}")
    else:
        print("  ok     exact M5 4b9f0aa3 artifact fails for all measured reasons")

    # Confirm the instrument, not merely the red result: keep the exact M5
    # report shape but make its second path material from the first window,
    # evolving in level and spectrum, and remove the fast event stream. The
    # same evaluator must now pass or its rejection is not trustworthy.
    corrected = json.loads(fixture.read_text())
    levels = (.2, 1.0, .3, 1.2)
    centroids = (300.0, 950.0, 420.0, 1100.0)
    for checkpoint, level, centroid in zip(
            corrected["checkpoints"], levels, centroids):
        lane = checkpoint["report"]["cables"][1]
        lane["mean_abs_v"] = level
        lane["dynamics"]["mean_rms"] = level
        lane["dynamics"]["duty_cycle"] = 1.0
        lane["spectrum"]["centroid_mean_hz"] = centroid
        lane["spectrum"]["centroid_range_octaves"] = .3
        lane["onsets"]["per_second"] = 0.0
        checkpoint["report"]["cables"][0]["dynamics"]["duty_cycle"] = 1.0
        checkpoint["report"]["pairwise"] = [
            {"left": 0, "right": 1, "correlation": 0.2}]
    corrected_report = P.LONG_HORIZON_MARKER + json.dumps(
        corrected, separators=(",", ":"))
    if P.long_horizon_evolution_errors(
            corrected_report, contract, layer_paths):
        bad += 1
        print("  WRONG  the M5 instrument rejects its corrected positive control")
    else:
        print("  ok     the same M5-shaped instrument accepts corrected evidence")

    # Left/right copies of one source are not multiple layers.
    one = P.runtime_quality_static_errors(
        layer_patch(False), layer_inventory(), contract)
    two = P.runtime_quality_static_errors(
        layer_patch(True), layer_inventory(), contract)
    if not one or two:
        bad += 1
        print(f"  WRONG  independent source-path proof confused stereo with layers: {one}, {two}")
    else:
        print("  ok     layers require independent source-to-output paths")

    periodic_errors = P.runtime_quality_static_errors(
        layer_patch(False), layer_inventory(), never_repeat)
    entropy_patch = layer_patch(False)
    entropy_patch["modules"].insert(
        1, {"id": 9, "plugin": "Test", "model": "Random"})
    entropy_patch["cables"].insert(0, {
        "outputModuleId": 9, "outputId": 0,
        "inputModuleId": 1, "inputId": 0})
    entropy_inventory = layer_inventory()
    entropy_inventory["Test"]["modules"]["Random"] = {
        "name": "Random voltage", "tags": ["Random"]}
    entropy_errors = P.runtime_quality_static_errors(
        entropy_patch, entropy_inventory, never_repeat)
    if not any("never repeats" in error for error in periodic_errors) or entropy_errors:
        bad += 1
        print(f"  WRONG  entropy topology proof misclassified periodic/connected patches: "
              f"{periodic_errors}, {entropy_errors}")
    else:
        print("  ok     never-repeat rejects periodic-only routes and accepts connected entropy")

    mixed = layer_patch(True)
    mixed["modules"].insert(2, {"id": 3, "plugin": "Test", "model": "Mix"})
    mixed["cables"] = [
        {"outputModuleId": 1, "outputId": 0,
         "inputModuleId": 3, "inputId": 0},
        {"outputModuleId": 2, "outputId": 0,
         "inputModuleId": 3, "inputId": 1},
        {"outputModuleId": 3, "outputId": 0,
         "inputModuleId": 4, "inputId": 0},
        {"outputModuleId": 3, "outputId": 0,
         "inputModuleId": 4, "inputId": 1},
    ]
    mixed_paths = P.runtime_quality_layer_paths(mixed, layer_inventory())
    mixed_errors = P.long_horizon_evolution_errors(
        corrected_report, contract, mixed_paths)
    if mixed_errors:
        bad += 1
        print(f"  WRONG  a normal two-source stereo premix was impossible: {mixed_errors}")
    else:
        print("  ok     independent paths may become one materially active premix")

    three_contract = P.compile_runtime_quality_contract(
        "Use three independent layers")
    three_mixed = json.loads(json.dumps(mixed))
    three_mixed["modules"].append(
        {"id": 5, "plugin": "Test", "model": "Tone"})
    three_mixed["cables"].insert(2, {
        "outputModuleId": 5, "outputId": 0,
        "inputModuleId": 3, "inputId": 2})
    three_paths = P.runtime_quality_layer_paths(
        three_mixed, layer_inventory())
    if not P.runtime_quality_static_errors(
            layer_patch(True), layer_inventory(), three_contract) or \
            P.runtime_quality_static_errors(
            three_mixed, layer_inventory(), three_contract) or \
            P.long_horizon_evolution_errors(
                corrected_report, three_contract, three_paths):
        bad += 1
        print("  WRONG  explicit three-layer minimum was not enforced/achievable")
    else:
        print("  ok     three structurally independent paths may share an active premix")

    broken = report([(0, [cable(1.0, 500)]),
                     (60, [cable(1.0, 500)]),
                     (594, [cable(0.0, 0.0)])])
    errors = P.long_horizon_evolution_errors(broken)
    if not errors or "594 seconds" not in errors[0]:
        bad += 1
        print(f"  WRONG  a patch that dies before the tail graduated: {errors}")
    else:
        print("  ok     silence at a late checkpoint fails with its time named")

    sparse = report([(start, [cable(.2, 500, duty=.1)])
                     for start in (0, 60, 300, 594)])
    sustained_only = P.compile_runtime_quality_contract(
        "A sustained drone for 10 minutes")
    if not any("80%" in error for error in
               P.long_horizon_evolution_errors(sparse, sustained_only)):
        bad += 1
        print("  WRONG  sparse impulses passed as sustained output")
    else:
        print("  ok     sustained output requires measured activity duty cycle")

    # A non-drone evolving control uses the same metrics and passes without
    # acquiring drone-only sustained/no-sequence/layer requirements.
    non_drone = P.compile_runtime_quality_contract(
        "A generative soundscape whose texture gradually transforms for 8 minutes")
    if not non_drone.evolving or non_drone.sustained or \
            P.long_horizon_evolution_errors(evolving, non_drone):
        bad += 1
        print(f"  WRONG  diverse non-drone evolution control failed: {non_drone}")
    else:
        print("  ok     non-drone evolution uses the shared measured contract")

    no_sequence = P.compile_runtime_quality_contract(
        "A texture that evolves for 10 minutes without obvious sequencing")
    sequence_errors = P.long_horizon_evolution_errors(
        m5, no_sequence, layer_paths)
    if no_sequence.sustained or not any(
            "fast event stream" in error for error in sequence_errors):
        bad += 1
        print("  WRONG  no-sequence was not enforced independently of sustained")
    else:
        print("  ok     no-sequence rejects material fast events independently")

    one_duplicated = report([
        (0, [cable(.4, 500, onsets=8), cable(.4, 500, onsets=8)]),
        (60, [cable(.4, 500), cable(.4, 500)]),
        (300, [cable(.4, 500), cable(.4, 500)]),
        (594, [cable(.4, 500), cable(.4, 500)]),
    ])
    if any("obvious sequence" in error for error in
           P.long_horizon_evolution_errors(one_duplicated, no_sequence)):
        bad += 1
        print("  WRONG  duplicated stereo lanes inflated sequence persistence")
    else:
        print("  ok     sequence persistence counts checkpoints, not duplicated lanes")

    regular = report([(start, [cable(.4, 500, onsets=2.0,
                                      periodicity=.8, interval_cv=.05)])
                      for start in (0, 60, 300, 594)])
    if not any("obvious sequence" in error for error in
               P.long_horizon_evolution_errors(regular, no_sequence)):
        bad += 1
        print("  WRONG  a regular 2 Hz sequence escaped the no-sequence contract")
    else:
        print("  ok     onset regularity catches obvious slower sequences")

    dual_mono = json.loads(stationary_broadband.split(
        P.LONG_HORIZON_MARKER, 1)[1])
    for checkpoint in dual_mono["checkpoints"]:
        checkpoint["report"]["pairwise"][0]["correlation"] = 1.0
    dual_mono_report = P.LONG_HORIZON_MARKER + json.dumps(dual_mono)
    if not any("correlation" in error for error in
               P.long_horizon_evolution_errors(dual_mono_report, contract)):
        bad += 1
        print("  WRONG  dual mono satisfied decorrelated/wide stereo")
    else:
        print("  ok     decorrelated stereo rejects byte-identical lanes")

    brief = P.runtime_quality_contract_prompt(contract)
    if not all(term in brief for term in
               ("sustained", "sequenced", "source", "spectrally", "stereo", "600")):
        bad += 1
        print(f"  WRONG  compiled constraints did not reach the model brief: {brief}")
    else:
        print("  ok     compiled constraints reach the model before generation")

    source = pathlib.Path(P.__file__).read_text()
    if "repair_long_horizon" in source or \
            "horizon_errors = long_horizon_evolution_errors" not in source or \
            "behaviour=diagnosis.behaviour + horizon_errors" not in source:
        bad += 1
        print("  WRONG  measured quality can be gamed by repair or bypass retry diagnosis")
    else:
        print("  ok     measured failures enter bounded retry; no artistic auto-repair")

    # Drive the real generation loop at its external seams. The first measured
    # rejection must appear in the second model prompt, consume only the
    # explicit one-retry budget, and return an unfinished Shortfall rather than
    # a GUI-eligible success.
    seen = []
    Claim = namedtuple("Claim", "slug gating")
    built = layer_patch(True)
    saved = {name: getattr(P, name) for name in (
        "find_claude", "ask_model", "library_brief", "catalog",
        "configure_audio", "audibility", "prepare_and_lint",
        "render_inventory", "intent_module_plan", "claim_idiom")}
    saved_attempts = P._ATTEMPTS_DIR
    try:
        with tempfile.TemporaryDirectory() as tmp:
            P._ATTEMPTS_DIR = os.path.join(tmp, "attempts")
            P.find_claude = lambda: "/usr/bin/true"
            P.library_brief = lambda *a, **k: ""
            P.catalog = lambda *a, **k: {}
            P.configure_audio = lambda *a, **k: None
            P.audibility = lambda *a, **k: (P.AUDIBLE, m5)
            P.prepare_and_lint = lambda patch, inv, **k: (patch, [])
            P.render_inventory = lambda *a, **k: ""
            P.intent_module_plan = lambda *a, **k: ""
            P.claim_idiom = lambda *a, **k: Claim("wandering-drone", True)

            def fake_model(_cli, model_prompt, _seconds, tick=8.0):
                seen.append(model_prompt)
                return 0, ("```json patch\n" + json.dumps(built) +
                           "\n```\n```json why\n{}\n```"), ""

            P.ask_model = fake_model
            _, _, shortfall = P.generate(
                "Several independent layers make a sustained drone with "
                "slow spectral evolution, no obvious sequencing, stereo, "
                "for 10 minutes", layer_inventory(), None, retries=1)
        retry = seen[1] if len(seen) > 1 else ""
        if len(seen) != 2 or shortfall is None or \
                "timbrally invariant" not in retry or \
                "fast event stream" not in retry:
            bad += 1
            print("  WRONG  measured M5 feedback did not drive bounded retry/shortfall")
        else:
            print("  ok     named M5 drone retains one redesign retry and blocks graduation")
    finally:
        for name, value in saved.items():
            setattr(P, name, value)
        P._ATTEMPTS_DIR = saved_attempts

    if P.long_horizon_evolution_errors("patch gate passed", contract) == [] or \
            "UNMEASURED" not in P.long_horizon_evolution_errors(
                "patch gate passed", contract)[0]:
        bad += 1
        print("  WRONG  absent real-DSP evidence could graduate to the GUI")
    else:
        print("  ok     missing runtime evidence fails closed before GUI graduation")
    return bad, ran


def main() -> int:
    bad, ran = check_long_horizon_contract()
    print(f"\n{ran - bad}/{ran} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
