#!/usr/bin/env python3
"""Verify frozen A1 stereo samples with an independent scalar f32 model."""

import json
import math
import struct
from pathlib import Path


FIXTURE = Path(__file__).with_name("a1_reference_vectors.json")


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def close(actual, expected, tolerance):
    return math.isfinite(actual) and abs(actual - expected) <= (
        tolerance["absolute"] + tolerance["relative"] * abs(expected)
    )


def render(case, profile, voices, mutation=None):
    frames = len(case["expected_left"])
    if frames != len(case["expected_right"]):
        raise ValueError("stereo lengths differ")
    kind = case["kind"]
    active = case.get("active_slots", [0])
    if len(set(active)) != len(active) or any(slot not in range(8) for slot in active):
        raise ValueError("invalid active slot")
    if [voice["slot"] for voice in voices] != list(range(8)):
        raise ValueError("voice vectors must be ordered by slot")
    gain = [f32(v["gain"]) for v in voices] if kind == "sum" else [f32(1)] * 8
    if kind == "sum":
        samples = [(v["left"], v["right"]) for v in voices]
    elif kind == "sum_custom":
        samples = [([left], [right]) for left, right in zip(case["left"], case["right"])]
        if len(samples) != 8:
            raise ValueError("custom sum must define eight slots")
    elif kind == "release":
        samples = [(case["source_left"], case["source_right"])]
        note_off = case["note_off_sample"]
        release_frames = profile["release_frames"]
        if mutation == "release_note_off_shift":
            note_off += 1
        if mutation == "release_length_change":
            release_frames -= 1
        if mutation == "invalid_release_range":
            note_off = frames - 1
        if not 0 <= note_off < frames or release_frames <= 0 or note_off + release_frames > frames:
            raise ValueError("invalid release range")
        declared_factors = case["release_factors"]
        if len(declared_factors) != frames:
            raise ValueError("release factor length differs from audio")
        if mutation is None:
            for frame, declared in enumerate(declared_factors):
                elapsed = max(0, frame - note_off)
                derived = f32(max(0, release_frames - elapsed) / release_frames)
                if f32(declared) != derived:
                    raise ValueError("declared release curve differs from the reference profile")
    elif kind == "steal":
        samples = [(case["old_left"], case["old_right"])]
        steal_at = case["steal_sample"]
        if (not 0 < steal_at < frames or
                len(case["new_left"]) < frames - steal_at or
                len(case["new_right"]) < frames - steal_at):
            raise ValueError("invalid steal range")
    else:
        raise ValueError(f"unsupported case kind: {kind}")
    bank_gain = f32(profile["bank_gain"])
    if mutation == "wrong_gain_law":
        bank_gain = f32(1 / len(active))
    left_result = []
    right_result = []
    for frame in range(frames):
        mixed = [f32(0), f32(0)]
        order = list(active)
        if mutation in {"wrong_sum_order_high_dynamic", "wrong_sum_order_low_magnitude"}:
            order.reverse()
        for slot in order:
            if kind == "steal" and frame >= steal_at:
                source = (case["new_left"][frame - steal_at],
                          case["new_right"][frame - steal_at])
            else:
                source = (samples[slot][0][frame], samples[slot][1][frame])
            if kind == "release":
                elapsed = max(0, frame - note_off)
                factor = f32(max(0, release_frames - elapsed) / release_frames)
                if mutation == "wrong_release_policy" and frame >= note_off:
                    factor = f32(0)
                if mutation == "unbounded_or_steal_tail" and frame >= note_off + release_frames:
                    factor = f32(0.25)
            else:
                factor = f32(1)
            effective = f32(gain[slot] * bank_gain)
            for channel in range(2):
                contribution = f32(f32(f32(source[channel]) * factor) * effective)
                mixed[channel] = f32(mixed[channel] + contribution)
        if mutation == "wrong_channel_swap":
            mixed.reverse()
        if mutation == "wrong_mono_duplication":
            mixed[1] = mixed[0]
        if mutation == "unbounded_or_steal_tail" and kind == "steal" and frame >= steal_at:
            mixed[0] = f32(mixed[0] + f32(f32(case["old_left"][frame]) * bank_gain))
        left_result.append(mixed[0])
        right_result.append(mixed[1])
    return left_result, right_result


def matches(actual, case, tolerance):
    expected_channels = (case["expected_left"], case["expected_right"])
    if len(actual) != 2 or any(len(channel) != len(expected)
                               for channel, expected in zip(actual, expected_channels)):
        return False
    return all(close(sample, f32(expected), tolerance)
               for channel, expected_channel in zip(actual, expected_channels)
               for sample, expected in zip(channel, expected_channel))


def check_profile(profile):
    if (profile["slot_count"] != 8 or profile["sample_rate_hz"] != 48000 or
            profile["gain_law"] != "fixed_rms_bank_v1" or
            profile["channel_shape"] != "stereo_identity" or
            profile["release_curve"] != "linear" or
            profile["tail_policy"] != "hard_termination_v1"):
        raise ValueError("unsupported A1 reference profile")
    # The gain law is defined here, not by the fixture: fixed_rms_bank_v1 is
    # 1/sqrt(slot_count). A fixture that rescales bank_gain together with its
    # expected samples must not pass on its own say-so.
    if f32(profile["bank_gain"]) != f32(1 / math.sqrt(profile["slot_count"])):
        raise ValueError("A1 bank_gain differs from the fixed_rms_bank_v1 law")


def main():
    data = json.loads(FIXTURE.read_text(encoding="utf-8"))
    profile = data["reference_profile"]
    try:
        check_profile(profile)
    except ValueError as error:
        raise SystemExit(str(error))
    try:
        check_profile(dict(profile, bank_gain=profile["bank_gain"] * 2))
    except ValueError:
        pass
    else:
        raise SystemExit("A1 planted negative escaped the gain-law check: rescaled_bank_gain")
    tolerance = profile["numeric_tolerance"]
    if any(not math.isfinite(tolerance[key]) or tolerance[key] <= 0
           for key in ("absolute", "relative")):
        raise SystemExit("invalid A1 numeric tolerance")
    cases = {case["id"]: case for case in data["cases"]}
    for case in cases.values():
        actual = render(case, profile, data["voice_vectors"])
        if not matches(actual, case, tolerance):
            raise SystemExit(
                f"frozen A1 vector differs from independent scalar reference: "
                f"{case['id']}: {actual}")
    controls = {
        "wrong_sum_order_high_dynamic": "sum_order_stress",
        "wrong_sum_order_low_magnitude": "sum_order_bounded_low_magnitude",
        "wrong_gain_law": "sparse_slots_gain_and_channel",
        "wrong_channel_swap": "all_eight_active_fixed_rms",
        "wrong_mono_duplication": "all_eight_active_fixed_rms",
        "wrong_release_policy": "note_off_linear_release_and_bounded_tail",
        "release_note_off_shift": "note_off_linear_release_and_bounded_tail",
        "release_length_change": "note_off_linear_release_and_bounded_tail",
        "invalid_release_range": "note_off_linear_release_and_bounded_tail",
        "unbounded_or_steal_tail": "steal_hard_reset_no_tail",
    }
    if {item["id"] for item in data["negative_controls"]} != set(controls):
        raise SystemExit("A1 negative control list differs from the reference checks")
    for mutation, case_id in controls.items():
        case = cases[case_id]
        try:
            actual = render(case, profile, data["voice_vectors"], mutation)
        except ValueError:
            if mutation != "invalid_release_range":
                raise
        else:
            if matches(actual, case, tolerance):
                raise SystemExit(f"A1 planted negative escaped the frozen oracle: {mutation}")
    release_case = cases["note_off_linear_release_and_bounded_tail"]
    if matches(render(release_case, profile, data["voice_vectors"],
                      "unbounded_or_steal_tail"), release_case, tolerance):
        raise SystemExit("A1 planted negative escaped the release-tail oracle")
    print(f"A1 scalar reference PASS positives={len(cases)} planted_negatives={len(controls)}")


if __name__ == "__main__":
    main()
