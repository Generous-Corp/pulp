"""The P0a pipeline: generate -> level-match -> align -> detect -> report.

Pure, testable stages (no CLI/IO coupling beyond reading the case) so each can be
exercised alone (§14.4). The CLI (`cli.py`) only parses args and calls `run_p0a`.
"""
from __future__ import annotations

from typing import Any

import numpy as np

from . import align, audio_io, generate, provenance
from .detectors import hf_fizz, spectral_centroid, transient_sharpness
from .schema import QualityCase, build_report

# Registry: detector tag -> detect fn. New detectors plug in here; the pipeline stays
# detector-agnostic (the §14.4 boundary). onset_drift was prototyped and DEFERRED — a
# body-correlation timing measure cannot resolve a few-ms drift against a tonal kick's
# periodic body (it cannot tell identity from a 7 ms drift); it needs a better timing
# method before it can be trusted. See the README "Deferred detectors" note.
_DETECTORS = {
    "transient_sharpness": transient_sharpness.detect,
    "spectral_centroid": spectral_centroid.detect,
    "hf_fizz": hf_fizz.detect,
}

P0A_CASE = QualityCase(
    case_id="drumbreak_p0a",
    family="time-stretch",
    reference_policy="frozen-reference",
    alignment_policy="onset-map",
    detector_tags=["transient_sharpness", "spectral_centroid", "hf_fizz"],
    params={"ratio": 1.5, "sr": 48000, "bpm": 120.0, "seed": 0},
)

def make_signals(
    degradation: str,
    latency_ms: float = 5.0,
    smear_ms: float = 8.0,
    case: QualityCase = P0A_CASE,
) -> tuple[np.ndarray, np.ndarray, int, list[float]]:
    """Build (reference, candidate, sr, injected_onsets) for a degradation.

    degradation: "identity" (negative control) | "smear".
    reference  = transient-preserving stretch to `ratio` (sharp, on-grid).
    candidate  = reference + degradation + `latency_ms` delay (so alignment is required).
    injected_onsets = reference onset times shifted by the latency: the ground-truth
                      defect locations the localization must hit within +/-20 ms.
    """
    sr = int(case.params["sr"])
    ratio = float(case.params["ratio"])
    reference, ref_onsets = generate.render_drum_break(
        sr, case.params["bpm"], ratio, case.params["seed"]
    )

    if degradation == "smear":
        candidate = generate.smear_transients(reference, ref_onsets, sr, smear_ms)
        injected_idx = list(range(len(ref_onsets)))
    elif degradation == "dull":
        candidate = generate.dull(reference, sr)
        injected_idx = []
    elif degradation == "fizz":
        candidate = generate.add_fizz(reference, sr)
        injected_idx = []
    else:  # identity
        candidate = reference.copy()
        injected_idx = []

    lat = int(latency_ms * sr / 1000.0)
    if lat > 0:
        candidate = np.concatenate([np.zeros(lat, dtype=np.float64), candidate])

    injected = [ref_onsets[i] + latency_ms / 1000.0 for i in injected_idx]
    return reference, candidate, sr, injected


# Back-compat for the P0a gate test (smear/identity only).
def make_p0a_signals(smear: bool, latency_ms: float = 5.0, smear_ms: float = 8.0,
                     case: QualityCase = P0A_CASE):
    return make_signals("smear" if smear else "identity", latency_ms, smear_ms, case)


def _execute(degradation, detectors, latency_ms, smear_ms, case):
    """Core pipeline shared by run() and run_and_export(): generate -> level-match ->
    align -> detect. Returns everything the report (and artifact export) needs."""
    reference, candidate, sr, _ = make_signals(degradation, latency_ms, smear_ms, case)
    detectors = detectors or case.detector_tags

    candidate = audio_io.level_match(candidate, reference)  # rule #1, before any measurement

    ref_onsets = align.detect_onsets(reference, sr)
    cand_onsets = align.detect_onsets(candidate, sr)
    pairs = align.map_onsets(ref_onsets, cand_onsets, len(reference) / sr, len(candidate) / sr)

    results = [_DETECTORS[name](reference, candidate, sr, pairs) for name in detectors]

    determinism = {
        "level_match": "rms",
        "alignment": case.alignment_policy,
        "onset_detector": {"win": 256, "hop": 128, "thresh_rel": 0.15},
        "sample_rate": sr,
        "onset_match": {
            "ref_onsets": len(ref_onsets),
            "cand_onsets": len(cand_onsets),
            "matched_pairs": len(pairs),
        },
    }
    recipe = {
        "case": case.case_id,
        "ratio": case.params["ratio"],
        "degradation": degradation,
        "smear_ms": smear_ms,
        "latency_ms": latency_ms,
        "seed": case.params["seed"],
    }
    # A "clean" verdict is only trustworthy if the detectors saw enough onsets. Low
    # coverage reads UNCERTAIN, not CLEAN — a detector that measured nothing must never
    # masquerade as a pass.
    if any(r.fired for r in results):
        verdict = "FIRED"
    elif any(r.low_coverage for r in results):
        verdict = "UNCERTAIN"
    else:
        verdict = "CLEAN"
    return {
        "reference": reference,
        "candidate": candidate,
        "sr": sr,
        "results": results,
        "determinism": determinism,
        "recipe": recipe,
        "verdict": verdict,
    }


def run(
    degradation: str,
    detectors: list[str] | None = None,
    latency_ms: float = 5.0,
    smear_ms: float = 8.0,
    case: QualityCase = P0A_CASE,
) -> dict[str, Any]:
    """Run the pipeline (generate -> level-match -> align -> detect -> report)."""
    x = _execute(degradation, detectors, latency_ms, smear_ms, case)
    return build_report(
        case, x["results"], provenance.build(x["recipe"], x["determinism"]),
        x["determinism"], x["verdict"],
    )


def run_and_export(
    degradation: str,
    out_dir: str,
    detectors: list[str] | None = None,
    latency_ms: float = 5.0,
    smear_ms: float = 8.0,
    case: QualityCase = P0A_CASE,
) -> dict[str, Any]:
    """Run the pipeline AND write listenable artifacts to `out_dir`: full reference /
    candidate WAVs, plus a short clip pair around each localized worst region so a
    developer (or an audio-capable model) can hear exactly what's wrong. Adds a
    `listening` block of relative paths to the report."""
    from . import regions

    x = _execute(degradation, detectors, latency_ms, smear_ms, case)
    listening = regions.export_artifacts(
        out_dir, x["reference"], x["candidate"], x["sr"], x["results"]
    )
    report = build_report(
        case, x["results"], provenance.build(x["recipe"], x["determinism"]),
        x["determinism"], x["verdict"],
    )
    report["listening"] = listening
    return report


def run_p0a(smear: bool, latency_ms: float = 5.0, smear_ms: float = 8.0,
            case: QualityCase = P0A_CASE) -> dict[str, Any]:
    """The P0a gate: the drum-break with just the transient-sharpness detector."""
    return run("smear" if smear else "identity", ["transient_sharpness"],
               latency_ms, smear_ms, case)
