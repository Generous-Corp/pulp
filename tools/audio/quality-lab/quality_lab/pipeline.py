"""The P0a pipeline: generate -> level-match -> align -> detect -> report.

Pure, testable stages (no CLI/IO coupling beyond reading the case) so each can be
exercised alone (§14.4). The CLI (`cli.py`) only parses args and calls `run_p0a`.
"""
from __future__ import annotations

from typing import Any

import numpy as np

from . import align, audio_io, generate, provenance
from .detectors import hf_fizz, hnr, spectral_centroid, spectral_flux, transient_sharpness
from .schema import QualityCase, build_report

# Registry: detector tag -> detect fn. New detectors plug in here; the pipeline stays
# detector-agnostic (the §14.4 boundary). onset_drift was prototyped and DEFERRED — a
# body-correlation timing measure cannot resolve a few-ms drift against a tonal kick's
# periodic body (it cannot tell identity from a 7 ms drift). See the README.
_DETECTORS = {
    "transient_sharpness": transient_sharpness.detect,
    "spectral_centroid": spectral_centroid.detect,
    "hf_fizz": hf_fizz.detect,
    "spectral_flux": spectral_flux.detect,
    "hnr": hnr.detect,
}

# Time-stretch family (percussive): the P0a drum break + transient/spectral detectors.
P0A_CASE = QualityCase(
    case_id="drumbreak_p0a",
    family="time-stretch",
    reference_policy="frozen-reference",
    alignment_policy="onset-map",
    detector_tags=["transient_sharpness", "spectral_centroid", "hf_fizz"],
    params={"ratio": 1.5, "sr": 48000, "bpm": 120.0, "seed": 0},
)

# Tonal family (sustained vocal/pad): identity-aligned, global spectral detectors —
# this is what proves the harness generalizes past drums (§3.5) and where spectral_flux
# (graininess) actually discriminates.
TONAL_CASE = QualityCase(
    case_id="tonal_pad",
    family="tonal",
    reference_policy="frozen-reference",
    alignment_policy="identity",
    detector_tags=["spectral_centroid", "hf_fizz", "spectral_flux", "hnr"],
    params={"sr": 48000, "dur_s": 2.5, "seed": 0},
)


def _render_reference(case: QualityCase):
    """Render a case's reference per its family. Returns (signal, onset_times)."""
    sr = int(case.params["sr"])
    if case.family == "tonal":
        return generate.render_tonal(sr, float(case.params["dur_s"]), int(case.params["seed"]))
    # time-stretch (percussive)
    return generate.render_drum_break(
        sr, case.params["bpm"], float(case.params["ratio"]), int(case.params["seed"])
    )


def _apply_degradation(degradation, reference, onsets, sr, smear_ms):
    """Apply a degradation to a reference. Returns (candidate, injected_onset_indices)."""
    if degradation == "smear":  # percussive only (needs onsets)
        return generate.smear_transients(reference, onsets, sr, smear_ms), list(range(len(onsets)))
    if degradation == "dull":
        return generate.dull(reference, sr), []
    if degradation == "fizz":
        return generate.add_fizz(reference, sr), []
    if degradation == "grainy":
        return generate.grainy(reference, sr), []
    if degradation == "noisy":
        return generate.noisy(reference, sr), []
    return reference.copy(), []  # identity


def make_signals(
    degradation: str,
    latency_ms: float = 5.0,
    smear_ms: float = 8.0,
    case: QualityCase = P0A_CASE,
) -> tuple[np.ndarray, np.ndarray, int, list[float]]:
    """Build (reference, candidate, sr, injected_onsets) for a degradation on a case.

    Family-agnostic (§3.5): the reference is rendered per `case.family`, and a latency
    pad is added only when the case needs time alignment (onset-map). Identity-aligned
    families (tonal) get no pad.
    """
    sr = int(case.params["sr"])
    reference, onsets = _render_reference(case)
    candidate, injected_idx = _apply_degradation(degradation, reference, onsets, sr, smear_ms)

    lat = int(latency_ms * sr / 1000.0) if case.alignment_policy != "identity" else 0
    if lat > 0:
        candidate = np.concatenate([np.zeros(lat, dtype=np.float64), candidate])

    injected = [onsets[i] + lat / sr for i in injected_idx]
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

    # Alignment stage runs per the case's policy (§4.5.1). Identity-aligned families
    # (e.g. tonal) need no onset map; the global spectral detectors don't use pairs.
    determinism = {
        "level_match": "rms",
        "alignment": case.alignment_policy,
        "sample_rate": sr,
    }
    if case.alignment_policy == "onset-map":
        ref_onsets = align.detect_onsets(reference, sr)
        cand_onsets = align.detect_onsets(candidate, sr)
        pairs = align.map_onsets(ref_onsets, cand_onsets, len(reference) / sr, len(candidate) / sr)
        determinism["onset_detector"] = {"win": 256, "hop": 128, "thresh_rel": 0.15}
        determinism["onset_match"] = {
            "ref_onsets": len(ref_onsets),
            "cand_onsets": len(cand_onsets),
            "matched_pairs": len(pairs),
        }
    else:
        pairs = []

    results = [_DETECTORS[name](reference, candidate, sr, pairs) for name in detectors]
    recipe = {
        "case": case.case_id,
        "family": case.family,
        "degradation": degradation,
        "smear_ms": smear_ms,
        "latency_ms": latency_ms,
        "params": dict(case.params),
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
    import os

    from . import perceptual, regions

    x = _execute(degradation, detectors, latency_ms, smear_ms, case)
    listening = regions.export_artifacts(
        out_dir, x["reference"], x["candidate"], x["sr"], x["results"]
    )
    prov = provenance.build(x["recipe"], x["determinism"])

    # Self-describing samples: drop a provenance sidecar next to the rendered WAVs so a
    # liked sample maps back to its commit + recipe even if separated from the folder.
    ref_wav = os.path.join(out_dir, listening["reference"])
    cand_wav = os.path.join(out_dir, listening["candidate"])
    provenance.write_sidecar(ref_wav, prov)
    provenance.write_sidecar(cand_wav, prov)

    report = build_report(case, x["results"], prov, x["determinism"], x["verdict"])
    report["listening"] = listening
    # Layer B perceptual models (advisory; skipped unless the developer opts in via env).
    report["perceptual"] = perceptual.evaluate(ref_wav, cand_wav)
    return report


def run_real_engine(
    ratio: float = 2.0,
    character: str = "clean",
    case: QualityCase = P0A_CASE,
) -> dict[str, Any]:
    """Validate the REAL Pulp stretch engine (stretchcli): render a source, stretch it
    with the actual `pulp::signal::OfflineStretch`, and run the detectors on its output
    vs a transient-faithful reference. Returns the report (with an `engine` block), or a
    skip report when stretchcli isn't built. The strongest credibility path — the
    detectors run on the product engine's real artifacts, not a synthetic degradation."""
    import os
    import tempfile

    from . import engine

    if not engine.available():
        skip = engine.stretch("", "", ratio)  # carries the actionable not-found reason
        return {"verdict": "SKIPPED", "engine": skip, "reason": skip["reason"]}

    sr = int(case.params["sr"])
    with tempfile.TemporaryDirectory() as d:
        src_wav = os.path.join(d, "source.wav")
        out_wav = os.path.join(d, "engine.wav")
        source, _ = generate.render_drum_break(sr, case.params["bpm"], 1.0, int(case.params["seed"]))
        audio_io.save_wav(src_wav, source, sr)
        eng_res = engine.stretch(src_wav, out_wav, ratio, character=character)
        if eng_res["status"] != "ok":
            return {"verdict": "ERROR", "engine": eng_res}

        reference, _ = generate.render_drum_break(sr, case.params["bpm"], ratio, int(case.params["seed"]))
        candidate, _ = audio_io.load_wav(out_wav)
        candidate = audio_io.level_match(candidate, reference)

    ref_onsets = align.detect_onsets(reference, sr)
    cand_onsets = align.detect_onsets(candidate, sr)
    pairs = align.map_onsets(ref_onsets, cand_onsets, len(reference) / sr, len(candidate) / sr)
    detectors = [t for t in case.detector_tags if t in _DETECTORS]
    results = [_DETECTORS[name](reference, candidate, sr, pairs) for name in detectors]

    determinism = {"level_match": "rms", "alignment": "onset-map", "sample_rate": sr,
                   "onset_match": {"ref_onsets": len(ref_onsets), "cand_onsets": len(cand_onsets),
                                   "matched_pairs": len(pairs)}}
    recipe = {"case": case.case_id, "family": case.family, "source": "real-engine",
              "engine": "stretchcli", "character": character, "ratio": ratio}
    verdict = "FIRED" if any(r.fired for r in results) else "CLEAN"
    report = build_report(case, results, provenance.build(recipe, determinism), determinism, verdict)
    report["engine"] = eng_res
    return report


def run_real_audio(
    input_wav: str, ratio: float = 2.0, character: str = "clean",
) -> dict[str, Any]:
    """Run the real engine on a REAL audio file and check it reference-free (dry-input
    policy): a faithful time-stretch preserves the SOURCE spectrum (timing changes,
    timbre shouldn't), so the global spectral detectors compare the engine output's LTAS
    to the source's. Works on any WAV the developer supplies — the path to real-audio
    evidence (the committed corpus stays synthetic; real audio is developer-supplied)."""
    import os

    from . import engine
    from .detectors import hf_fizz, spectral_centroid, spectral_flux

    if not engine.available():
        skip = engine.stretch("", "", ratio)  # carries the actionable not-found reason
        return {"verdict": "SKIPPED", "engine": skip, "reason": skip["reason"]}
    if not os.path.exists(input_wav):
        return {"verdict": "ERROR", "reason": f"input not found: {input_wav}"}

    source, sr = audio_io.load_wav(input_wav)
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        out_wav = os.path.join(d, "engine.wav")
        eng_res = engine.stretch(input_wav, out_wav, ratio, character=character)
        if eng_res["status"] != "ok":
            return {"verdict": "ERROR", "engine": eng_res}
        candidate, _ = audio_io.load_wav(out_wav)
    candidate = audio_io.level_match(candidate, source)

    # Global spectral detectors (no alignment): dry-input reference is the source itself.
    results = [
        spectral_centroid.detect(source, candidate, sr),
        hf_fizz.detect(source, candidate, sr),
        spectral_flux.detect(source, candidate, sr),
    ]
    determinism = {"level_match": "rms", "alignment": "identity", "sample_rate": sr,
                   "reference_policy": "dry-input"}
    recipe = {"source": "real-audio", "input": os.path.basename(input_wav),
              "engine": "stretchcli", "character": character, "ratio": ratio}
    case = QualityCase(case_id=f"realaudio:{os.path.basename(input_wav)}", family="real-audio",
                       reference_policy="dry-input", alignment_policy="identity",
                       detector_tags=["spectral_centroid", "hf_fizz", "spectral_flux", "hnr"], params=recipe)
    verdict = "FIRED" if any(r.fired for r in results) else "CLEAN"
    report = build_report(case, results, provenance.build(recipe, determinism), determinism, verdict)
    report["engine"] = eng_res
    return report


def run_p0a(smear: bool, latency_ms: float = 5.0, smear_ms: float = 8.0,
            case: QualityCase = P0A_CASE) -> dict[str, Any]:
    """The P0a gate: the drum-break with just the transient-sharpness detector."""
    return run("smear" if smear else "identity", ["transient_sharpness"],
               latency_ms, smear_ms, case)
