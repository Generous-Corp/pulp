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


def render_tonal(
    sr: int = 48000, dur_s: float = 2.5, seed: int = 0, f0: float = 220.0
) -> tuple[np.ndarray, list[float]]:
    """A sustained vocal/pad-like tone — the TONAL corpus family (§3.5: the harness
    serves more than drums). Harmonic stack + two formants + vibrato + slow tremolo, so
    it has LOW baseline spectral flux (graininess shows) and a definite brightness/HF
    profile (dulling / fizz show). No onsets (sustained); returns []. Deterministic.
    """
    rng = np.random.default_rng(seed)
    n = int(dur_s * sr)
    t = np.arange(n) / sr
    vib = 1.0 + 0.01 * np.sin(2 * np.pi * 5.0 * t + rng.uniform(0, 6.28))  # ~5 Hz vibrato
    phase = 2 * np.pi * np.cumsum(f0 * vib) / sr
    y = np.zeros(n, dtype=np.float64)
    for k in range(1, 14):
        hz = f0 * k
        formant = np.exp(-(((hz - 700.0) / 400.0) ** 2)) + 0.7 * np.exp(-(((hz - 1800.0) / 600.0) ** 2))
        amp = (1.0 / k) * (0.35 + formant)
        y += amp * np.sin(k * phase)
    y *= 0.85 + 0.15 * np.sin(2 * np.pi * 0.7 * t)  # slow tremolo
    ar = int(0.05 * sr)
    env = np.ones(n)
    env[:ar] = np.linspace(0, 1, ar)
    env[-ar:] = np.linspace(1, 0, ar)
    y *= env
    return (y / (np.max(np.abs(y)) + 1e-12) * 0.5), []


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


def grainy(y: np.ndarray, sr: int, amount: float = 0.18, rate_hz: float = 500.0) -> np.ndarray:
    """Add fast-gated mid-band noise — a controlled stand-in for the frame-to-frame
    spectral churn (graininess) a phase vocoder / granular path adds, for the
    spectral_flux detector's positive control. Band-limited so it isn't HF fizz. On a
    SUSTAINED tone this raises spectral flux decisively (it doesn't on transient-heavy
    drums, where transient flux dominates — see the deferred-detector note). Seeded."""
    y = np.asarray(y, dtype=np.float64)
    rng = np.random.default_rng(777)
    noise = rng.standard_normal(len(y))
    dt, rc = 1.0 / sr, 1.0 / (2 * np.pi * 6000.0)
    a = dt / (rc + dt)
    lp = np.empty_like(noise)
    acc = 0.0
    for i in range(len(noise)):
        acc += a * (noise[i] - acc)
        lp[i] = acc
    n_gate = max(2, int(len(y) / sr * rate_hz))
    gate = (rng.uniform(0.0, 1.0, n_gate) > 0.5).astype(np.float64)
    gate_env = np.interp(np.linspace(0, n_gate - 1, len(y)), np.arange(n_gate), gate)
    env_y = float(np.sqrt(np.mean(y * y) + 1e-20))
    return y + amount * env_y * lp * gate_env


def dull(y: np.ndarray, sr: int, cutoff_hz: float = 3500.0) -> np.ndarray:
    """One-pole low-pass over the whole signal — a controlled stand-in for the
    brightness loss (STN dulling) the spectral-centroid detector must catch."""
    y = np.asarray(y, dtype=np.float64)
    dt = 1.0 / sr
    rc = 1.0 / (2 * np.pi * cutoff_hz)
    a = dt / (rc + dt)
    out = np.empty_like(y)
    acc = 0.0
    for i in range(len(y)):
        acc += a * (y[i] - acc)
        out[i] = acc
    return out


def add_fizz(y: np.ndarray, sr: int, amount: float = 0.10, cutoff_hz: float = 8000.0) -> np.ndarray:
    """Add high-pass-filtered noise — a controlled stand-in for the metallic HF sizzle
    a phase vocoder can birth, for the hf_fizz detector's positive control. Seeded."""
    y = np.asarray(y, dtype=np.float64)
    rng = np.random.default_rng(12345)
    noise = rng.standard_normal(len(y))
    # crude high-pass: subtract a one-pole low-pass of the noise
    dt = 1.0 / sr
    rc = 1.0 / (2 * np.pi * cutoff_hz)
    a = dt / (rc + dt)
    lp = np.empty_like(noise)
    acc = 0.0
    for i in range(len(noise)):
        acc += a * (noise[i] - acc)
        lp[i] = acc
    hp = noise - lp
    env = np.sqrt(np.mean(y * y) + 1e-20)
    return y + amount * env * hp


def noisy(y: np.ndarray, sr: int, amount: float = 0.12, seed: int = 0) -> np.ndarray:
    """Add broadband white noise scaled to the signal RMS — the canonical HNR defect
    (tonal purity loss / roughness). Deterministic for a given seed."""
    rng = np.random.default_rng(seed)
    env = np.sqrt(np.mean(np.asarray(y, dtype=np.float64) ** 2) + 1e-20)
    return np.asarray(y, dtype=np.float64) + amount * env * rng.standard_normal(len(y))


def render_stereo_pad(
    sr: int = 48000, dur_s: float = 2.5, seed: int = 0, f0: float = 220.0, width: float = 0.6
) -> np.ndarray:
    """A wide stereo pad: a shared mono tonal core (`render_tonal`) plus per-channel
    decorrelated side content, so RMS(side)/RMS(mid) is non-trivial and the channels are
    only partly correlated. Deterministic. Returns an (N, 2) L/R array."""
    mono, _ = render_tonal(sr, dur_s, seed, f0)
    rng = np.random.default_rng(seed + 1)
    rms = np.sqrt(np.mean(mono ** 2) + 1e-20)
    side = width * rms * rng.standard_normal(len(mono))
    return np.stack([mono + 0.5 * side, mono - 0.5 * side], axis=1)


def narrow_stereo(stereo: np.ndarray, amount: float = 0.8) -> np.ndarray:
    """Collapse stereo width toward mono by attenuating the side signal by `amount`
    (1.0 -> full mono). Returns an (N, 2) array."""
    s = np.asarray(stereo, dtype=np.float64)
    mid = 0.5 * (s[:, 0] + s[:, 1])
    side = 0.5 * (s[:, 0] - s[:, 1]) * (1.0 - amount)
    return np.stack([mid + side, mid - side], axis=1)


def invert_phase_right(stereo: np.ndarray) -> np.ndarray:
    """Flip the polarity of the right channel — an out-of-phase / mono-incompatibility
    defect. Returns an (N, 2) array."""
    s = np.asarray(stereo, dtype=np.float64).copy()
    s[:, 1] = -s[:, 1]
    return s


def drift(reference, onsets, sr, lag_ms_start=0.0, lag_ms_end=0.0):
    """Inject a per-onset TIMING drift and nothing else. Onset k is delayed by a lag
    interpolated linearly from `lag_ms_start` to `lag_ms_end` across the onsets; each
    inter-onset segment is shifted by its lag so the attack waveform stays verbatim (no
    smear / brightness / level change — only the event time moves). Returns a candidate
    the same length as `reference`. A uniform start==end is a pure latency (no groove
    drift); start!=end is a ramp."""
    y = np.asarray(reference, dtype=np.float64)
    n = len(y)
    out = np.zeros(n, dtype=np.float64)
    os = sorted(float(o) for o in onsets)
    N = len(os)
    if N == 0:
        return y.copy()
    # Segment boundaries at the midpoints between consecutive onsets (quiet points for a
    # drum break), so each segment carries exactly one transient.
    bounds = [0]
    for k in range(1, N):
        bounds.append(int(round((os[k - 1] + os[k]) / 2.0 * sr)))
    bounds.append(n)
    for k in range(N):
        frac = (k / (N - 1)) if N > 1 else 0.0
        lag = int(round((lag_ms_start + (lag_ms_end - lag_ms_start) * frac) * sr / 1000.0))
        s, e = bounds[k], bounds[k + 1]
        seg = y[s:e]
        ds, de = s + lag, s + lag + len(seg)
        a, b = max(0, ds), min(n, de)
        if b > a:
            out[a:b] = seg[a - ds: a - ds + (b - a)]  # later segments win at any overlap
    return out
