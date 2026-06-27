"""The P0a pipeline: generate -> level-match -> align -> detect -> report.

Pure, testable stages (no CLI/IO coupling beyond reading the case) so each can be
exercised alone (§14.4). The CLI (`cli.py`) only parses args and calls `run_p0a`.
"""
from __future__ import annotations

from typing import Any

import numpy as np

from . import align, audio_io, generate, provenance
from .detectors import transient_sharpness
from .schema import QualityCase, build_report

P0A_CASE = QualityCase(
    case_id="drumbreak_p0a",
    family="time-stretch",
    reference_policy="frozen-reference",
    alignment_policy="onset-map",
    detector_tags=["transient_sharpness"],
    params={"ratio": 1.5, "sr": 48000, "bpm": 120.0, "seed": 0},
)


def make_p0a_signals(
    smear: bool, latency_ms: float = 5.0, smear_ms: float = 8.0, case: QualityCase = P0A_CASE
) -> tuple[np.ndarray, np.ndarray, int, list[float]]:
    """Build (reference, candidate, sr, injected_onsets).

    reference  = transient-preserving stretch of the source to `ratio` (sharp attacks).
    candidate  = reference, optionally smeared, optionally delayed by `latency_ms` so
                 alignment is genuinely required (candidate onsets != reference onsets).
    injected_onsets are the reference onset times (the ground-truth smear locations,
    pre-latency) — the bar the localization must hit within +/-20 ms (after latency).
    """
    sr = int(case.params["sr"])
    ratio = float(case.params["ratio"])
    _, _ = generate.render_drum_break(sr, case.params["bpm"], 1.0, case.params["seed"])
    reference, ref_onsets = generate.render_drum_break(
        sr, case.params["bpm"], ratio, case.params["seed"]
    )

    candidate = generate.smear_transients(reference, ref_onsets, sr, smear_ms) if smear else reference.copy()

    lat = int(latency_ms * sr / 1000.0)
    if lat > 0:
        candidate = np.concatenate([np.zeros(lat, dtype=np.float64), candidate])

    injected = [t + latency_ms / 1000.0 for t in ref_onsets]
    return reference, candidate, sr, injected


def run_p0a(
    smear: bool, latency_ms: float = 5.0, smear_ms: float = 8.0, case: QualityCase = P0A_CASE
) -> dict[str, Any]:
    """Run the full P0a pipeline and return the report dict."""
    reference, candidate, sr, _ = make_p0a_signals(smear, latency_ms, smear_ms, case)

    # Stage: level-match (rule #1) before any measurement.
    candidate = audio_io.level_match(candidate, reference)

    # Stage: align (onset-map) — required because candidate carries latency.
    ref_onsets = align.detect_onsets(reference, sr)
    cand_onsets = align.detect_onsets(candidate, sr)
    pairs = align.map_onsets(
        ref_onsets, cand_onsets, len(reference) / sr, len(candidate) / sr
    )

    # Stage: detect.
    result = transient_sharpness.detect(reference, candidate, sr, pairs)

    # Stage: report.
    determinism = {
        "level_match": "rms",
        "alignment": case.alignment_policy,
        "onset_detector": {"win": 256, "hop": 128, "thresh_rel": 0.15},
        "sample_rate": sr,
        "tolerance_class": result.tolerance_class,
    }
    recipe = {
        "case": case.case_id,
        "ratio": case.params["ratio"],
        "smear": smear,
        "smear_ms": smear_ms,
        "latency_ms": latency_ms,
        "seed": case.params["seed"],
    }
    verdict = "FIRED" if result.fired else "CLEAN"
    return build_report(
        case,
        [result],
        provenance.build(recipe, determinism),
        determinism,
        verdict,
    )
