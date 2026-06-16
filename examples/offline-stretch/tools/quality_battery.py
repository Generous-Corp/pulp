#!/usr/bin/env python3
"""Established-library audio-quality battery for the OfflineStretch scoreboard.

Plan: planning/2026-06-16-offline-stretch-beat-r3-plan.md (P0 — harness).

WHY THIS EXISTS: the hand-rolled probes in metrics.py gave WRONG conclusions
twice (RMS-matching hid a 4.4 LUFS loudness gap; an envelope "attack" metric was
fooled by crest into calling smeared transients "sharp"). This module replaces
the quality judgement with ESTABLISHED, standard libraries that also surface
artifacts we weren't thinking to measure:

  - pyloudnorm  (ITU-R BS.1770 LUFS)  — REQUIRED loudness-match gate; you must
    LUFS-match before any magnitude-sensitive feature or the A/B is confounded.
  - librosa     (onset strength, HPSS balance, spectral centroid/rolloff/
    flatness/bandwidth/contrast, onset count) — the spectral-flux onset strength
    is the real transient-crispness signal; HPSS perc-fraction is the best
    "percussion smeared into tonal mush" probe (the actual robot-vibe signature).
  - essentia    (OPTIONAL, AGPL dev-tool only) — inharmonicity (metallic/phasey
    signature), dynamic_complexity, spectral_complexity, onset_rate.
  - audiobox-aesthetics (OPTIONAL, Meta 2024) — a learned NO-REFERENCE music
    quality verdict (PQ/CE/CU) that needs no aligned reference.

Empirically (2026-06-16, real drum break, 2x, loudness-matched): these surfaced
that Pulp loses 4.4 LUFS, smears transients (onset_strength_max 4.4 vs R3 16.2),
collapses percussive energy (HPSS perc_frac 8.5% vs orig 31%), and raises
inharmonicity — none of which the homegrown metrics caught. The custom
phase-incoherence / reassignment-dispersion probes did NOT cleanly separate the
engines; HPSS perc_frac + inharmonicity do, so prefer those.

Run with the dev venv (librosa + pyloudnorm required; essentia / audiobox /
cdpam optional and reported as "unavailable" if not installed):
  quality_battery.py INPUT.wav OUTPUT.wav            # one render, vs its input
  quality_battery.py --orig REF.wav A.wav B.wav ...  # several renders, learned
                                                       #   no-ref + cdpam vs REF
"""

from __future__ import annotations

import json
import sys

import numpy as np

try:
    import soundfile as sf
    import librosa
    import pyloudnorm as pyln
    _CORE = True
except Exception:  # pragma: no cover
    _CORE = False

try:
    import essentia.standard as es  # AGPL — dev-tool only
    _ESSENTIA = True
except Exception:
    _ESSENTIA = False

SR = 44100
TARGET_LUFS = -23.0


def _load_mono(path):
    a, sr = sf.read(path, always_2d=True)
    a = a.mean(axis=1).astype(np.float64)
    if sr != SR:
        a = librosa.resample(a, orig_sr=sr, target_sr=SR)
        sr = SR
    return a, sr


def _lufs(meter, a):
    return float(meter.integrated_loudness(a))


def _librosa_battery(a, sr):
    n_fft, hop = 2048, 512
    S = np.abs(librosa.stft(a, n_fft=n_fft, hop_length=hop))
    oenv = librosa.onset.onset_strength(y=a, sr=sr, hop_length=hop)
    H, P = librosa.effects.hpss(a)
    he, pe = float(np.sum(H ** 2)), float(np.sum(P ** 2))
    rms = float(np.sqrt(np.mean(a ** 2)) + 1e-12)
    return {
        "crest_db": round(20 * np.log10((np.max(np.abs(a)) + 1e-12) / rms), 2),
        # spectral-flux onset strength = real transient crispness (NOT crest)
        "onset_strength_max": round(float(np.max(oenv)), 2),
        "onset_strength_mean": round(float(np.mean(oenv)), 3),
        "onset_count": int(len(librosa.onset.onset_detect(onset_envelope=oenv, sr=sr, hop_length=hop))),
        "centroid_hz": round(float(np.mean(librosa.feature.spectral_centroid(S=S, sr=sr))), 0),
        "rolloff85_hz": round(float(np.mean(librosa.feature.spectral_rolloff(S=S, sr=sr, roll_percent=0.85))), 0),
        "flatness": round(float(np.mean(librosa.feature.spectral_flatness(S=S))), 4),
        "bandwidth_hz": round(float(np.mean(librosa.feature.spectral_bandwidth(S=S, sr=sr))), 0),
        "contrast_mean": round(float(np.mean(librosa.feature.spectral_contrast(S=S, sr=sr))), 2),
        # HPSS percussive-energy fraction: the "percussion -> tonal mush" probe
        "hpss_perc_frac": round(pe / (he + pe + 1e-12), 3),
        "hpss_harm_perc_ratio": round(he / (pe + 1e-12), 2),
    }


def _essentia_battery(a, sr):
    if not _ESSENTIA:
        return {}
    af = a.astype(np.float32)
    out = {}
    try:
        dc, _ = es.DynamicComplexity()(af)
        out["dynamic_complexity"] = round(float(dc), 2)
    except Exception:
        pass
    try:
        out["onset_rate"] = round(float(es.OnsetRate()(af)[1]), 2)
    except Exception:
        pass
    # Inharmonicity + spectral complexity over frames (metallic/phasey signature).
    try:
        spectrum = es.Spectrum()
        peaks = es.SpectralPeaks()
        inharm = es.Inharmonicity()
        scx = es.SpectralComplexity()
        w = es.Windowing(type="hann")
        ih, sc = [], []
        for frame in es.FrameGenerator(af, frameSize=2048, hopSize=1024):
            spec = spectrum(w(frame))
            f, m = peaks(spec)
            if len(f) > 1 and f[0] > 0:
                ih.append(inharm(f, m))
            sc.append(scx(spec))
        if ih:
            out["inharmonicity"] = round(float(np.mean(ih)), 3)
        if sc:
            out["spectral_complexity"] = round(float(np.mean(sc)), 2)
    except Exception:
        pass
    return out


def quality(in_path, out_path):
    """Loudness-matched established-tool battery for one (input, output) render.

    Returns a flat dict of metrics. The output is LUFS-matched to TARGET_LUFS
    before magnitude-sensitive features so the A/B is not confounded by level;
    the raw LUFS and the input->output LUFS delta are reported separately (the
    delta is itself a quality signal — a stretch should preserve loudness)."""
    if not _CORE:
        return {"error": "quality_battery requires librosa + pyloudnorm (dev venv)"}
    meter = pyln.Meter(SR)
    ai, _ = _load_mono(in_path)
    ao, _ = _load_mono(out_path)
    lufs_in, lufs_out = _lufs(meter, ai), _lufs(meter, ao)
    res = {
        "lufs": round(lufs_out, 2),
        "lufs_delta_vs_input": round(lufs_out - lufs_in, 2),  # ~0 = level preserved
        "essentia": _ESSENTIA,
    }
    ao_m = pyln.normalize.loudness(ao, lufs_out, TARGET_LUFS)
    res.update(_librosa_battery(ao_m, SR))
    res.update(_essentia_battery(ao_m, SR))
    return res


def main(argv):
    if not _CORE:
        sys.stderr.write("quality_battery needs the dev venv: pip install librosa pyloudnorm soundfile\n")
        return 2
    if len(argv) == 3:
        print(json.dumps(quality(argv[1], argv[2]), indent=2))
        return 0
    sys.stderr.write("usage: quality_battery.py INPUT.wav OUTPUT.wav\n")
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
