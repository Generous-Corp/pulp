"""Reference-free measurements and expectation-grounded A/B scoring.

Pairwise artifact detectors answer whether one render changed relative to another.
They cannot answer whether a synth moved toward a requested sound when neither render
is known-good. This module measures each WAV independently, converts explicit target
expectations into normalized lower-is-better errors, then reuses the tuning loop's
Goodhart guard across those errors.
"""
from __future__ import annotations

import json
import math
import os
from typing import Any

import numpy as np

from . import audio_io, dsp, loop

SCHEMA = "quality_lab.reference_free.v1"
AB_SCHEMA = "quality_lab.catalogue_ab.v1"


def _amplitude_modulation(y: np.ndarray, sr: int) -> tuple[float, float]:
    """Dominant 0.5..20 Hz amplitude-envelope rate and peak confidence.

    A 10 ms moving average removes carrier ripple from the rectified waveform.
    Confidence is the dominant modulation bin's share of energy in the search band;
    it is evidence strength, not a quality verdict.
    """
    x = np.abs(np.asarray(y, dtype=np.float64))
    if x.size < sr // 2 or audio_io.rms(x) <= 1e-8:
        return 0.0, 0.0
    win = max(1, int(round(sr * 0.010)))
    # A cumulative moving average is O(n); direct convolution makes a normal
    # multi-second render needlessly O(n * 10 ms worth of samples).
    total = np.cumsum(np.concatenate((np.zeros(1), x)))
    env = (total[win:] - total[:-win]) / win
    # Downsample the slow envelope before its FFT. The mean is removed so DC
    # cannot win merely because the patch has a steady output level.
    hop = max(1, int(sr // 200))
    env = env[::hop]
    env = (env - np.mean(env)) * np.hanning(len(env))
    mag = np.abs(np.fft.rfft(env)) ** 2
    freqs = np.fft.rfftfreq(len(env), hop / sr)
    band = (freqs >= 0.5) & (freqs <= 20.0)
    energy = float(np.sum(mag[band]))
    if energy <= 1e-20:
        return 0.0, 0.0
    indices = np.flatnonzero(band)
    at = int(indices[np.argmax(mag[band])])
    return float(freqs[at]), float(mag[at] / energy)


def analyze_samples(y: np.ndarray, sr: int) -> dict[str, Any]:
    """Measure a mono render without treating another render as truth."""
    x = np.asarray(y, dtype=np.float64)
    if x.size == 0 or sr <= 0:
        raise ValueError("audio is empty or has an invalid sample rate")
    freqs, mag = dsp.ltas(x, sr)
    am_hz, am_confidence = _amplitude_modulation(x, sr)
    metrics = {
        "spectral_centroid_hz": dsp.spectral_centroid_hz(freqs, mag),
        "hf_energy_fraction": dsp.hf_energy_fraction(freqs, mag, 8000.0),
        "spectral_flux": dsp.mean_spectral_flux(x, sr),
        "hnr_db": dsp.harmonic_to_noise_ratio_db(x, sr),
        "amplitude_modulation_hz": am_hz,
    }
    return {
        "schema": SCHEMA,
        "sample_rate": int(sr),
        "frames": int(x.size),
        "duration_s": float(x.size / sr),
        "metrics": {k: float(v) for k, v in metrics.items()},
        "confidence": {"amplitude_modulation_hz": am_confidence},
    }


def analyze_file(path: str) -> dict[str, Any]:
    y, sr = audio_io.load_wav(path)
    report = analyze_samples(y, sr)
    report["input"] = os.path.abspath(path)
    return report


def _metric_error(value: float, rule: dict[str, Any]) -> float:
    if not math.isfinite(value):
        raise ValueError("measured metric must be finite")
    tolerance = float(rule.get("tolerance", 0.0))
    if not math.isfinite(tolerance) or tolerance <= 0.0:
        raise ValueError("every expectation needs a finite tolerance > 0")
    modes = sum(k in rule for k in ("target", "min", "max"))
    if modes != 1:
        raise ValueError("every expectation needs exactly one of target, min, or max")
    if "target" in rule:
        boundary = float(rule["target"])
        if not math.isfinite(boundary):
            raise ValueError("expectation bounds must be finite")
        return abs(value - boundary) / tolerance
    if "min" in rule:
        boundary = float(rule["min"])
        if not math.isfinite(boundary):
            raise ValueError("expectation bounds must be finite")
        return max(0.0, boundary - value) / tolerance
    boundary = float(rule["max"])
    if not math.isfinite(boundary):
        raise ValueError("expectation bounds must be finite")
    return max(0.0, value - boundary) / tolerance


def score(report: dict[str, Any], expectations: dict[str, Any],
          label: str) -> loop.CandidateScore:
    """Convert explicit physical expectations into normalized target errors."""
    rules = expectations.get("metrics") or {}
    if not rules:
        raise ValueError("expectations must contain a non-empty metrics object")
    measured = report.get("metrics") or {}
    unknown = sorted(set(rules) - set(measured))
    if unknown:
        raise ValueError(f"unknown reference-free metric(s): {', '.join(unknown)}")
    scores = {name: _metric_error(float(measured[name]), rule)
              for name, rule in rules.items()}
    confidences = report.get("confidence") or {}
    used_confidences = [float(confidences[name]) for name in rules
                        if name in confidences]
    confidence = min(used_confidences) if used_confidences else 1.0
    return loop.CandidateScore(label=label, scores=scores, confidence=confidence)


def compare_files(without_wav: str, with_wav: str, expectations_path: str,
                  holdout_without: str | None = None,
                  holdout_with: str | None = None) -> dict[str, Any]:
    """Compare catalogue-off/on renders against the same explicit expectations."""
    with open(expectations_path) as fh:
        expectations = json.load(fh)
    source = expectations.get("source") or {}
    if not source.get("path") or not ("page" in source or source.get("locator")):
        raise ValueError(
            "catalogue expectations need source.path and source.page or source.locator")
    without = analyze_file(without_wav)
    with_catalogue = analyze_file(with_wav)
    champ = score(without, expectations, "catalogue-off")
    cand = score(with_catalogue, expectations, "catalogue-on")
    holdout = None
    holdout_cand = holdout_champ = None
    if bool(holdout_without) != bool(holdout_with):
        raise ValueError("holdout-without and holdout-with must be supplied together")
    if holdout_without and holdout_with:
        ho_without = analyze_file(holdout_without)
        ho_with = analyze_file(holdout_with)
        holdout_champ = score(ho_without, expectations, "catalogue-off-holdout")
        holdout_cand = score(ho_with, expectations, "catalogue-on-holdout")
        holdout = {
            "without": ho_without,
            "with": ho_with,
            "scores": {"without": holdout_champ.to_dict(),
                       "with": holdout_cand.to_dict()},
        }
    thresholds = {name: 1.0 for name in cand.scores}
    guard = loop.goodhart_guard(cand, champ, holdout_cand, holdout_champ,
                                thresholds=thresholds)
    return {
        "schema": AB_SCHEMA,
        "expectations": expectations,
        "working": {
            "without": without,
            "with": with_catalogue,
            "scores": {"without": champ.to_dict(), "with": cand.to_dict()},
        },
        "holdout": holdout,
        "goodhart_guard": guard,
        "verdict": ("CATALOGUE_IMPROVES" if guard["accepted"] and holdout
                    else "WORKING_SET_IMPROVES" if guard["accepted"]
                    else "NOT_PROVEN"),
    }
