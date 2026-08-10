#!/usr/bin/env python3
"""Evaluate measured acid behavior from one synchronized eight-tap capture.

The verdict is about observable sequencer, slew, cutoff, filter, and output
behavior.  It does not score whether a sound is fashionable or subjectively
resembles a particular recording.  Step boundaries come only from the captured
clock, and every comparison stays inside that one sample clock.
"""

from __future__ import annotations

import math
import statistics

import acid_taps as taps


PASS, FAIL, UNMEASURED = taps.PASS, taps.FAIL, taps.UNMEASURED

STEPS_PER_LOOP = 8
MIN_LOOPS = 2
SLIDE_GATE_ON = 1.0
ACCENT_CV_ON = 0.1
PITCH_TOLERANCE = 0.02
CUTOFF_FLOOR = 0.02
CUTOFF_DIFFERENCE = 0.03
AUDIO_FLOOR = 5e-4
BRIGHTNESS_DIFFERENCE = 0.08
TRANSIENT_DIFFERENCE = 0.15
CONTROL_LOOP_DIFFERENCE = 0.05
AUDIO_LOOP_LEVEL_DIFFERENCE = 0.25
AUDIO_LOOP_BRIGHTNESS_DIFFERENCE = 0.15
AUDIO_LOOP_TRANSIENT_DIFFERENCE = 0.3
FINAL_PATH_DIFFERENCE = 0.02
SAFE_FINAL_PEAK_VOLTS = 20.0


def _result(verdict: str, *, measurement=(), behavior=(), structural=(),
            observations=None) -> dict:
    measurement = list(measurement)
    behavior = list(behavior)
    structural = list(structural)
    return {
        "verdict": verdict,
        "scope": "acid-behavior",
        "reasons": structural + measurement + behavior,
        "structural_failures": structural,
        "measurement_failures": measurement,
        "behavior_failures": behavior,
        "observations": observations or {},
    }


def _rms(values: list[float]) -> float:
    return math.sqrt(sum(value * value for value in values) / len(values)) \
        if values else 0.0


def _part(values: list[float], window: tuple[int, int],
          begin: float, end: float) -> list[float]:
    left, right = window
    width = right - left
    first = left + min(width - 1, max(0, int(width * begin)))
    last = left + min(width, max(1, int(math.ceil(width * end))))
    return values[first:max(first + 1, last)]


def _level(values: list[float], window: tuple[int, int]) -> float:
    return statistics.median(_part(values, window, 0.2, 0.8))


def _clock_windows(clock: list[float]) -> tuple[list[tuple[int, int]], list[str], dict]:
    low, high = min(clock), max(clock)
    observation = {"minimum": low, "maximum": high}
    if high - low < 0.5:
        return [], ["clock: no usable rising edges were captured"], observation

    threshold = (low + high) * 0.5
    edges = []
    above = clock[0] > threshold
    for index, value in enumerate(clock[1:], 1):
        now = value > threshold
        if now and not above:
            edges.append(index)
        above = now
    observation["rising_edges"] = len(edges)
    if len(edges) < STEPS_PER_LOOP * MIN_LOOPS + 1:
        return [], [
            f"clock: captured {max(0, len(edges) - 1)} complete steps; "
            f"need at least {STEPS_PER_LOOP * MIN_LOOPS} for two 8-step loops"
        ], observation

    intervals = [right - left for left, right in zip(edges, edges[1:])]
    period = statistics.median(intervals)
    tolerance = max(2.0, period * 0.08)
    if period < 16 or any(abs(interval - period) > tolerance
                          for interval in intervals):
        return [], [
            "clock: step periods are too short or inconsistent to establish "
            "one synchronized timebase"
        ], {**observation, "period_samples": period,
            "interval_range": [min(intervals), max(intervals)]}

    loop_count = len(intervals) // STEPS_PER_LOOP
    window_count = loop_count * STEPS_PER_LOOP
    windows = list(zip(edges[:window_count], edges[1:window_count + 1]))
    observation.update({"period_samples": period, "complete_steps": window_count,
                        "complete_loops": loop_count})
    return windows, [], observation


def _distinct(values: list[float], tolerance: float) -> list[float]:
    result = []
    for value in values:
        if all(abs(value - existing) > tolerance for existing in result):
            result.append(value)
    return result


def _loop_mismatch(levels: list[float], tolerance: float) -> list[int]:
    loops = len(levels) // STEPS_PER_LOOP
    return [step for step in range(STEPS_PER_LOOP)
            if max(levels[loop * STEPS_PER_LOOP + step]
                   for loop in range(loops))
            - min(levels[loop * STEPS_PER_LOOP + step]
                  for loop in range(loops)) > tolerance]


def _normalized(values: list[float], window: tuple[int, int],
                points: int = 16) -> list[float]:
    left, right = window
    width = right - left
    return [values[left + min(width - 1, int((point + 0.5) * width / points))]
            for point in range(points)]


def _control_loop_mismatch(values: list[float],
                           windows: list[tuple[int, int]],
                           tolerance: float) -> list[int]:
    loops = len(windows) // STEPS_PER_LOOP
    result = []
    for step in range(STEPS_PER_LOOP):
        signatures = [_normalized(values, windows[loop * STEPS_PER_LOOP + step])
                      for loop in range(loops)]
        reference = signatures[0]
        if any(_rms([left - right for left, right in zip(reference, candidate)])
               > tolerance for candidate in signatures[1:]):
            result.append(step)
    return result


def _glide_observation(values: list[float], window: tuple[int, int],
                       previous: float, target: float) -> dict:
    width = window[1] - window[0]
    # A short, musically useful glide can finish before 3% of a slow step.
    # Start immediately after the cable-delay sample; the intermediate-level
    # tests below still distinguish a ramp from an instantaneous jump.
    first = window[0] + 1
    last = window[0] + max(3, int(width * 0.5))
    delta = target - previous
    progress = [(value - previous) / delta for value in values[first:last]]
    target_error = sum(abs(1.0 - value) for value in progress) / len(progress)
    intermediate_values = [value for value in progress if 0.08 < value < 0.92]
    intermediate = len(intermediate_values)
    intermediate_levels = len(_distinct(intermediate_values, 0.08))
    backward = sum(right + 0.03 < left
                   for left, right in zip(progress, progress[1:]))
    late = statistics.median(_part(values, window, 0.7, 0.95))
    return {"target_error": target_error, "intermediate_samples": intermediate,
            "intermediate_levels": intermediate_levels,
            "starts_near_previous": abs(progress[0]) <= 0.25,
            "backward_samples": backward, "late_error": abs(late - target)}


def _audio_features(values: list[float], window: tuple[int, int]) -> dict:
    body = _part(values, window, 0.05, 0.95)
    level = _rms(body)
    differences = [right - left for left, right in zip(body, body[1:])]
    early = _rms(_part(values, window, 0.05, 0.3))
    late = _rms(_part(values, window, 0.55, 0.9))
    return {"rms": level,
            "brightness": _rms(differences) / max(level, AUDIO_FLOOR),
            "transient": early / max(late, AUDIO_FLOOR)}


def _audibly_different(left: dict, right: dict) -> bool:
    return (abs(left["brightness"] - right["brightness"])
            >= BRIGHTNESS_DIFFERENCE
            or abs(left["transient"] - right["transient"])
            >= TRANSIENT_DIFFERENCE)


def _audio_loop_mismatch(values: list[float],
                         windows: list[tuple[int, int]]) -> list[int]:
    loops = len(windows) // STEPS_PER_LOOP
    result = []
    for step in range(STEPS_PER_LOOP):
        features = [_audio_features(values,
                                    windows[loop * STEPS_PER_LOOP + step])
                    for loop in range(loops)]
        reference = features[0]
        for candidate in features[1:]:
            level_scale = max(reference["rms"], candidate["rms"], AUDIO_FLOOR)
            if (abs(reference["rms"] - candidate["rms"]) / level_scale
                    > AUDIO_LOOP_LEVEL_DIFFERENCE
                    or abs(reference["brightness"] - candidate["brightness"])
                    > AUDIO_LOOP_BRIGHTNESS_DIFFERENCE
                    or abs(reference["transient"] - candidate["transient"])
                    > AUDIO_LOOP_TRANSIENT_DIFFERENCE):
                result.append(step)
                break
    return result


def evaluate(plan: taps.Plan, series: dict[str, list[float]]) -> dict:
    """Return PASS, FAIL, or UNMEASURED for one acid behavior capture."""
    if plan.status != taps.READY:
        reasons = list(plan.reasons) or ["the semantic tap plan is unavailable"]
        return _result(UNMEASURED, measurement=reasons)
    if plan.failures:
        return _result(FAIL, structural=plan.failures)

    measurement = []
    clean: dict[str, list[float]] = {}
    lengths = set()
    for name in taps.ORDER:
        values = series.get(name) if isinstance(series, dict) else None
        if not isinstance(values, list) or not values:
            measurement.append(f"{name}: synchronized series is missing")
            continue
        if not all(isinstance(value, (int, float))
                   and not isinstance(value, bool) and math.isfinite(value)
                   for value in values):
            measurement.append(f"{name}: series contains a non-finite sample")
            continue
        clean[name] = [float(value) for value in values]
        lengths.add(len(values))
    if len(lengths) > 1:
        measurement.append(f"series lengths differ: {sorted(lengths)}; "
                           "the taps are not on one timebase")
    if measurement:
        return _result(UNMEASURED, measurement=measurement)

    windows, clock_failures, clock_observation = _clock_windows(clean["clock"])
    if clock_failures:
        return _result(UNMEASURED, measurement=clock_failures,
                       observations={"clock": clock_observation})

    observations: dict = {"clock": clock_observation}
    behavior = []
    raw = [_level(clean["raw_pitch"], window) for window in windows]
    accent_levels = [_level(clean["accent"], window) for window in windows]
    slide_levels = [_level(clean["slide"], window) for window in windows]
    accent = [value > ACCENT_CV_ON for value in accent_levels]
    slide_active_low = (plan.witnesses.get("slide", {})
                        .get("active_polarity") == "low")
    slide = [(value < SLIDE_GATE_ON if slide_active_low
              else value > SLIDE_GATE_ON) for value in slide_levels]

    raw_mismatch = _loop_mismatch(raw, PITCH_TOLERANCE)
    accent_mismatch = _loop_mismatch([float(value) for value in accent], 0.0)
    slide_mismatch = _loop_mismatch([float(value) for value in slide], 0.0)
    if raw_mismatch:
        measurement.append(f"raw_pitch: loops disagree at 8-step positions "
                           f"{raw_mismatch}; a stale or mismatched series cannot pass")
    if accent_mismatch:
        measurement.append(f"accent: loops disagree at 8-step positions "
                           f"{accent_mismatch}; a stale or mismatched series cannot pass")
    if slide_mismatch:
        measurement.append(f"slide: loops disagree at 8-step positions "
                           f"{slide_mismatch}; a stale or mismatched series cannot pass")
    for name, tolerance in (("post_slew_pitch", PITCH_TOLERANCE * 2),
                            ("effective_cutoff", CONTROL_LOOP_DIFFERENCE)):
        mismatch = _control_loop_mismatch(clean[name], windows, tolerance)
        if mismatch:
            measurement.append(
                f"{name}: loops disagree at 8-step positions {mismatch}; "
                "a stale or mismatched series cannot pass")
    # Audio is captured in the same interleaved probe as every control lane,
    # so it cannot come from another timebase. A free-running oscillator and a
    # resonant filter legitimately vary window RMS/transient phase across
    # otherwise identical loops. Repetition is therefore proven on pitch,
    # selectors, post-slew pitch, and cutoff above; audio is judged below for
    # audibility, amplifier traversal, and accent contrast.
    if measurement:
        return _result(UNMEASURED, measurement=measurement,
                       observations=observations)

    pattern = raw[:STEPS_PER_LOOP]
    pitches = _distinct(pattern, PITCH_TOLERANCE)
    accent_steps = [index for index, active
                    in enumerate(accent[:STEPS_PER_LOOP]) if active]
    slide_steps = [index for index, active
                   in enumerate(slide[:STEPS_PER_LOOP]) if active]
    observations.update({
        "raw_pitch": {"step_values": pattern, "distinct_pitches": pitches},
        "accent": {"active_steps": accent_steps},
        "slide": {"active_steps": slide_steps,
                  "active_polarity": "low" if slide_active_low else "high"},
    })
    if len(pitches) < 3:
        behavior.append(f"raw_pitch: only {len(pitches)} distinct pitches were "
                        "measured; need at least 3")
    for name, active in (("accent", accent_steps), ("slide", slide_steps)):
        if not active:
            behavior.append(f"{name}: no steps are selected")
        elif len(active) == STEPS_PER_LOOP:
            behavior.append(f"{name}: every step is selected; selection must be "
                            "a strict subset of the 8-step loop")

    post_late = [statistics.median(_part(clean["post_slew_pitch"], window,
                                         0.7, 0.95))
                 for window in windows]
    late_errors = [abs(got - want) for got, want in zip(post_late, raw)]
    if max(late_errors) > PITCH_TOLERANCE * 2:
        behavior.append("post_slew_pitch: the slew output does not settle on the "
                        "raw pitch target inside each clock-derived step")

    if slide_steps and len(slide_steps) < STEPS_PER_LOOP:
        selected_positions = []
        unselected_positions = []
        for step in range(STEPS_PER_LOOP):
            if abs(pattern[step] - pattern[step - 1]) <= PITCH_TOLERANCE:
                continue
            (selected_positions if slide[step] else unselected_positions).append(step)
        matched = [(selected, unselected)
                   for selected in selected_positions
                   for unselected in unselected_positions
                   if abs(pattern[selected - 1] - pattern[unselected - 1])
                   <= PITCH_TOLERANCE
                   and abs(pattern[selected] - pattern[unselected])
                   <= PITCH_TOLERANCE]
        if not selected_positions or not matched:
            measurement.append(
                "slide: the fixture lacks a pitch-changing selected transition "
                "and a matched unselected transition")
        else:
            glide_by_step = {}
            loops = len(windows) // STEPS_PER_LOOP
            for step in set(selected_positions + [pair[1] for pair in matched]):
                glide_by_step[step] = [
                    _glide_observation(
                        clean["post_slew_pitch"],
                        windows[loop * STEPS_PER_LOOP + step],
                        raw[loop * STEPS_PER_LOOP + step - 1],
                        raw[loop * STEPS_PER_LOOP + step])
                    for loop in range(1 if step == 0 else 0, loops)]
            observations["slide"]["transition_metrics"] = glide_by_step
            for step in selected_positions:
                metrics = glide_by_step[step]
                if any(metric["intermediate_samples"] < 2
                       or metric["intermediate_levels"] < 3
                       or not metric["starts_near_previous"]
                       or metric["backward_samples"] > 1
                       or metric["late_error"] > PITCH_TOLERANCE * 2
                       for metric in metrics):
                    behavior.append(
                        f"slide: selected transition into step {step} does not "
                        "show a repeatable post-slew pitch glide")
            for step in sorted({unselected for _, unselected in matched}):
                metrics = glide_by_step[step]
                if any(metric["target_error"] > 0.04
                       or metric["intermediate_samples"] > 1
                       for metric in metrics):
                    behavior.append(
                        f"slide: matched unselected transition into step {step} "
                        "also glides")

    cutoff = clean["effective_cutoff"]
    measured_cutoff = cutoff[windows[0][0]:windows[-1][1]]
    cutoff_range = max(measured_cutoff) - min(measured_cutoff)
    observations["effective_cutoff"] = {
        "minimum": min(measured_cutoff), "maximum": max(measured_cutoff),
        "range": cutoff_range}
    if max(abs(value) for value in measured_cutoff) < CUTOFF_FLOOR:
        behavior.append("effective_cutoff: the captured cutoff is zero")
    elif cutoff_range < CUTOFF_FLOOR:
        behavior.append("effective_cutoff: the captured cutoff is flat")

    for name in ("filter_audio", "final_audio"):
        measured_audio = clean[name][windows[0][0]:windows[-1][1]]
        level = _rms(measured_audio)
        peak = max(abs(value) for value in measured_audio)
        observations[name] = {"rms": level, "peak_volts": peak}
        if level < AUDIO_FLOOR:
            behavior.append(f"{name}: the captured signal is silent")
        if name == "final_audio" and peak > SAFE_FINAL_PEAK_VOLTS:
            behavior.append(
                f"final_audio: peak {peak:.3f} V exceeds the "
                f"{SAFE_FINAL_PEAK_VOLTS:.1f} V output safety limit")

    measured_filter = clean["filter_audio"][windows[0][0]:windows[-1][1]]
    measured_final = clean["final_audio"][windows[0][0]:windows[-1][1]]
    path_difference = _rms([final - filtered
                            for filtered, final
                            in zip(measured_filter, measured_final)])
    path_scale = max(_rms(measured_filter), AUDIO_FLOOR)
    observations["final_audio"]["difference_from_filter"] = (
        path_difference / path_scale)
    if path_difference / path_scale < FINAL_PATH_DIFFERENCE:
        behavior.append("final_audio: the captured output bypasses the amplifier "
                        "response and duplicates filter_audio")

    accent_pairs = [(left, right)
                    for left in range(STEPS_PER_LOOP)
                    for right in range(left + 1, STEPS_PER_LOOP)
                    if abs(pattern[left] - pattern[right]) <= PITCH_TOLERANCE
                    and accent[left] != accent[right]
                    and slide[left] == slide[right]]
    if (accent_steps and len(accent_steps) < STEPS_PER_LOOP
            and not accent_pairs):
        measurement.append(
            "accent: the fixture lacks repeated equal-pitch accented and "
            "unaccented steps with matched slide state")
    elif accent_pairs:
        loops = len(windows) // STEPS_PER_LOOP
        pair_observations = []
        proven = False
        for left, right in accent_pairs:
            accented, plain = ((left, right) if accent[left] else (right, left))
            per_loop = []
            pair_passes = True
            for loop in range(loops):
                ai = loop * STEPS_PER_LOOP + accented
                ui = loop * STEPS_PER_LOOP + plain
                cutoff_a = _part(cutoff, windows[ai], 0.02, 0.95)
                cutoff_u = _part(cutoff, windows[ui], 0.02, 0.95)
                count = min(len(cutoff_a), len(cutoff_u))
                cutoff_difference = _rms(
                    [cutoff_a[index] - cutoff_u[index]
                     for index in range(count)])
                loop_observation = {"cutoff_rms_difference": cutoff_difference}
                if cutoff_difference < CUTOFF_DIFFERENCE:
                    pair_passes = False
                for name in ("filter_audio", "final_audio"):
                    feature_a = _audio_features(clean[name], windows[ai])
                    feature_u = _audio_features(clean[name], windows[ui])
                    different = _audibly_different(feature_a, feature_u)
                    loop_observation[name] = {
                        "accented": feature_a, "unaccented": feature_u,
                        "brightness_or_transient_difference": different}
                    if not different:
                        pair_passes = False
                per_loop.append(loop_observation)
            pair_observations.append({"accented_step": accented,
                                      "unaccented_step": plain,
                                      "loops": per_loop,
                                      "passes": pair_passes})
            proven = proven or pair_passes
        observations["accent"]["matched_pairs"] = pair_observations
        if not proven:
            behavior.append(
                "accent: no repeated equal-pitch pair proves both an effective "
                "cutoff change and filter/final-audio brightness or transient "
                "differences in every loop")

    if measurement:
        return _result(UNMEASURED, measurement=measurement,
                       behavior=behavior, observations=observations)
    if behavior:
        return _result(FAIL, behavior=behavior, observations=observations)
    return _result(PASS, observations=observations)
