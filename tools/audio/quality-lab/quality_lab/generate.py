"""Deterministic, self-labeling stimulus generation for P0a (§5 P0a, §5 P0b).

A synthetic drum break with sharp transients at known onset times, a
spacing-preserving "stretch" that keeps attacks verbatim (the *good* reference), and
a transient-smear degradation (the *known-bad* candidate). Everything here is
seeded — no random_device, no clocks — so a render is reproducible and its ground
truth is known by construction. License-clean (synthesized, not sampled).
"""
from __future__ import annotations

import numpy as np


def _env_exp(n: int, sr: int, decay_ms: float) -> np.ndarray:
    t = np.arange(n) / sr
    return np.exp(-t / (decay_ms / 1000.0))


def _kick(sr: int) -> np.ndarray:
    n = int(0.18 * sr)
    t = np.arange(n) / sr
    # Pitch-swept low sine with a sharp (no fade-in) attack.
    f = 110.0 * np.exp(-t / 0.03) + 45.0
    phase = 2 * np.pi * np.cumsum(f) / sr
    return (np.sin(phase) * _env_exp(n, sr, 120.0)).astype(np.float64)


def _snare(sr: int, rng: np.random.Generator) -> np.ndarray:
    n = int(0.14 * sr)
    noise = rng.standard_normal(n)
    tone = np.sin(2 * np.pi * 185.0 * np.arange(n) / sr)
    body = 0.7 * noise + 0.3 * tone
    return (body * _env_exp(n, sr, 90.0)).astype(np.float64)


def _hat(sr: int, rng: np.random.Generator) -> np.ndarray:
    n = int(0.04 * sr)
    noise = rng.standard_normal(n)
    # crude high-pass: first difference emphasizes HF
    hp = np.diff(noise, prepend=0.0)
    return (hp * _env_exp(n, sr, 25.0)).astype(np.float64)


# A two-bar 4/4 pattern as (beat_position_in_quarter_notes, voice).
_PATTERN = [
    (0.0, "kick"), (0.5, "hat"), (1.0, "snare"), (1.5, "hat"),
    (2.0, "kick"), (2.5, "hat"), (2.5, "kick"), (3.0, "snare"), (3.5, "hat"),
    (4.0, "kick"), (4.5, "hat"), (5.0, "snare"), (5.5, "hat"),
    (6.0, "kick"), (6.5, "hat"), (7.0, "snare"), (7.5, "hat"),
]


def render_drum_break(
    sr: int = 48000, bpm: float = 120.0, ratio: float = 1.0, seed: int = 0
) -> tuple[np.ndarray, list[float]]:
    """Render the pattern. `ratio` scales onset *spacing* (a transient-preserving
    stretch: attacks stay verbatim, only the spacing grows) so source (ratio 1.0) and
    reference (ratio 1.5) share identical hit waveforms at scaled times.

    Returns (signal, onset_times_seconds). Onset times are the *true* labels.
    """
    rng = np.random.default_rng(seed)
    beat_dur = 60.0 / bpm
    voices = {"kick": _kick(sr), "snare": _snare(sr, rng), "hat": _hat(sr, rng)}

    onsets = sorted({round(beat * beat_dur * ratio, 6) for beat, _ in _PATTERN})
    last = max(beat for beat, _ in _PATTERN) * beat_dur * ratio
    total = int((last + 0.5) * sr)
    y = np.zeros(total, dtype=np.float64)

    for beat, voice in _PATTERN:
        start = int(round(beat * beat_dur * ratio * sr))
        hit = voices[voice]
        end = min(start + len(hit), total)
        y[start:end] += hit[: end - start]

    peak = np.max(np.abs(y)) + 1e-12
    return (y / peak * 0.7), onsets


def smear_transients(
    y: np.ndarray, onset_times: list[float], sr: int, ms: float = 8.0
) -> np.ndarray:
    """Degrade each attack by low-pass + temporal-spread over a ±`ms` window — a
    controlled stand-in for the phase-vocoder attack smear the real detector must
    catch. Reduces the onset's rise slope and peak; localized to each onset.
    """
    out = np.array(y, dtype=np.float64, copy=True)
    half = max(1, int(ms * sr / 1000.0))
    klen = 2 * half + 1
    kernel = np.hanning(klen)
    kernel /= kernel.sum()
    for t in onset_times:
        c = int(round(t * sr))
        lo = max(0, c - half)
        hi = min(len(out), c + 2 * half)
        if hi - lo < klen:
            continue
        seg = out[lo:hi]
        out[lo:hi] = np.convolve(seg, kernel, mode="same")
    return out
