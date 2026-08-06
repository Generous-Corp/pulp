"""Reference-free measurements and expectation-grounded A/B scoring.

Pairwise artifact detectors answer whether one render changed relative to another.
They cannot answer whether a synth moved toward a requested sound when neither render
is known-good. This module measures each WAV independently, converts explicit target
expectations into normalized lower-is-better errors, then reuses the tuning loop's
Goodhart guard across those errors.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
from typing import Any

import numpy as np

from . import audio_io, dsp, loop

SCHEMA = "quality_lab.reference_free.v1"
AB_SCHEMA = "quality_lab.catalogue_ab.v1"
EXPERIMENT_SCHEMA = "quality_lab.catalogue_experiment.v1"
# Below -80 dBFS, detector values are too easily dominated by render noise,
# denormals, or a leaked control/DC value to support a perceptual claim.
MIN_AUDIBLE_RMS = 1e-4
HF_MIN_BINS = 8


def _audible_band_rms(y: np.ndarray, sr: int) -> float:
    """RMS energy between 20 Hz and 20 kHz, excluding DC and infrasonics."""
    x = np.asarray(y, dtype=np.float64)
    if x.size == 0 or sr <= 0:
        return 0.0
    spectrum = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(x.size, 1.0 / sr)
    band = (freqs >= 20.0) & (freqs <= min(20000.0, sr / 2.0))
    if not np.any(band):
        return 0.0
    weights = np.full(spectrum.size, 2.0)
    weights[0] = 1.0
    if x.size % 2 == 0:
        weights[-1] = 1.0
    mean_square = float(np.sum(weights[band] * np.abs(spectrum[band]) ** 2)
                        / (x.size * x.size))
    return math.sqrt(max(0.0, mean_square))


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
        "spectral_flux": dsp.mean_spectral_flux(x, sr),
        "hnr_db": dsp.harmonic_to_noise_ratio_db(x, sr),
        "amplitude_modulation_hz": am_hz,
    }
    if dsp.hf_band_bin_count(sr, 8000.0) >= HF_MIN_BINS:
        metrics["hf_energy_fraction"] = dsp.hf_energy_fraction(
            freqs, mag, 8000.0)
    return {
        "schema": SCHEMA,
        "sample_rate": int(sr),
        "frames": int(x.size),
        "duration_s": float(x.size / sr),
        "metrics": {k: float(v) for k, v in metrics.items()},
        "confidence": {"amplitude_modulation_hz": am_confidence},
        "audibility": {"rms": audio_io.rms(x),
                        "audible_band_rms": _audible_band_rms(x, sr),
                        "minimum_rms": MIN_AUDIBLE_RMS},
    }


def analyze_file(path: str) -> dict[str, Any]:
    y, sr, channels = audio_io.load_wav_multichannel(path)
    selected = 0
    if channels > 1:
        # Catalogue targets describe timbre, not spatial image. A mean downmix
        # can erase an anti-phase stereo signal and manufacture silence, so use
        # the loudest actual listener channel and disclose that bounded choice.
        levels = [audio_io.rms(y[:, channel]) for channel in range(channels)]
        selected = int(np.argmax(levels))
        y = y[:, selected]
    report = analyze_samples(y, sr)
    report["input"] = os.path.abspath(path)
    report["channel_handling"] = {
        "input_channels": channels,
        "analysis": "mono" if channels == 1 else "loudest-listener-channel",
        "selected_channel": selected,
    }
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
    measured_rms = float((report.get("audibility") or {}).get(
        "audible_band_rms", 0.0))
    if not math.isfinite(measured_rms) or measured_rms < MIN_AUDIBLE_RMS:
        raise ValueError(f"{label} is silent or below the audibility floor")
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


def _sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _pcm_sha256(path: str) -> str:
    """Hash decoded samples so a rewrapped/re-encoded copy is not independent."""
    samples, sr, channels = audio_io.load_wav_multichannel(path)
    digest = hashlib.sha256()
    digest.update(f"{sr}:{channels}:".encode("ascii"))
    digest.update(np.asarray(samples, dtype="<f8").tobytes())
    return digest.hexdigest()


def _experiment(path: str, wavs: dict[str, str], has_holdout: bool) -> dict:
    """Validate a content-bound record of the controlled generation arms."""
    with open(path) as source:
        doc = json.load(source)
    if doc.get("schema") != EXPERIMENT_SCHEMA:
        raise ValueError(f"experiment manifest schema must be {EXPERIMENT_SCHEMA}")
    splits = ["working"] + (["holdout"] if has_holdout else [])
    for split in splits:
        row = doc.get(split) or {}
        missing = [key for key in
                   ("pair_id", "prompt", "inventory_sha256", "model",
                    "audibility_gate", "arms")
                   if not row.get(key)]
        if missing:
            raise ValueError(f"experiment {split} lacks {', '.join(missing)}")
        for arm, state in (("without", "off"), ("with", "on")):
            arm_doc = (row.get("arms") or {}).get(arm) or {}
            attempt = arm_doc.get("attempt")
            seed = arm_doc.get("seed")
            generation_id = arm_doc.get("generation_id")
            if (arm_doc.get("guidance") != state
                    or not isinstance(attempt, int) or isinstance(attempt, bool)
                    or attempt <= 0
                    or not isinstance(seed, int) or isinstance(seed, bool)
                    or not isinstance(generation_id, str)
                    or not generation_id.strip()):
                raise ValueError(
                    f"experiment {split}.{arm} must record guidance={state}, "
                    "a positive integer attempt, integer seed, and generation_id")
            actual = _sha256(wavs[f"{split}.{arm}"])
            if arm_doc.get("wav_sha256") != actual:
                raise ValueError(f"experiment {split}.{arm} WAV digest does not match")
        without = row["arms"]["without"]
        with_guidance = row["arms"]["with"]
        if without["attempt"] != with_guidance["attempt"]:
            raise ValueError(f"experiment {split} arms must use the same attempt")
        if without["seed"] != with_guidance["seed"]:
            raise ValueError(f"experiment {split} arms must use the same seed")
        gate = row["audibility_gate"]
        if gate != {"analysis": "audible-band-rms-v1",
                    "minimum_rms": MIN_AUDIBLE_RMS}:
            raise ValueError(
                f"experiment {split} audibility_gate must record the exact "
                "Quality Lab audible-band RMS configuration")
    if has_holdout:
        for key in ("prompt", "inventory_sha256", "model", "audibility_gate"):
            if doc["working"][key] != doc["holdout"][key]:
                raise ValueError(f"working and holdout must use the same {key}")
        if doc["working"]["pair_id"] == doc["holdout"]["pair_id"]:
            raise ValueError("holdout pair_id must be independent of the working pair")
        if (doc["working"]["arms"]["without"]["seed"]
                == doc["holdout"]["arms"]["without"]["seed"]):
            raise ValueError("holdout must use a seed independent of the working pair")
        digests = [doc[split]["arms"][arm]["wav_sha256"]
                   for split in splits for arm in ("without", "with")]
        if len(set(digests)) != len(digests):
            raise ValueError("working and holdout WAV bodies must all be distinct")
        pcm_digests = [_pcm_sha256(wavs[f"{split}.{arm}"])
                       for split in splits for arm in ("without", "with")]
        if len(set(pcm_digests)) != len(pcm_digests):
            raise ValueError("working and holdout decoded audio must all be distinct")
    generation_ids = [doc[split]["arms"][arm]["generation_id"]
                      for split in splits for arm in ("without", "with")]
    if len(set(generation_ids)) != len(generation_ids):
        raise ValueError("every experiment arm needs a distinct generation_id")
    doc["manifest_sha256"] = _sha256(path)
    return doc


def compare_files(without_wav: str, with_wav: str, expectations_path: str,
                  experiment_path: str,
                  holdout_without: str | None = None,
                  holdout_with: str | None = None) -> dict[str, Any]:
    """Compare catalogue-off/on renders against the same explicit expectations."""
    with open(expectations_path) as fh:
        expectations = json.load(fh)
    source = expectations.get("source") or {}
    page = source.get("page")
    locator = source.get("locator")
    has_page = isinstance(page, int) and not isinstance(page, bool) and page > 0
    has_locator = isinstance(locator, str) and bool(locator.strip())
    source_path = source.get("path")
    has_path = isinstance(source_path, str) and bool(source_path.strip())
    if not has_path or not (has_page or has_locator):
        raise ValueError(
            "catalogue expectations need source.path and a positive source.page "
            "or non-empty source.locator")
    if bool(holdout_without) != bool(holdout_with):
        raise ValueError("holdout-without and holdout-with must be supplied together")
    wavs = {"working.without": without_wav, "working.with": with_wav}
    if holdout_without and holdout_with:
        wavs.update({"holdout.without": holdout_without,
                     "holdout.with": holdout_with})
    experiment = _experiment(experiment_path, wavs, bool(holdout_without))
    without = analyze_file(without_wav)
    with_catalogue = analyze_file(with_wav)
    champ = score(without, expectations, "catalogue-off")
    cand = score(with_catalogue, expectations, "catalogue-on")
    holdout = None
    holdout_cand = holdout_champ = None
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
        "experiment": experiment,
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
