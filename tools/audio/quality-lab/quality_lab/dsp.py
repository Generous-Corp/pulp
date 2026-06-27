"""Shared DSP primitives for the Layer-A detectors (§14.4: factor shared DSP into
common primitives rather than copying per detector).

Pure numpy. No detector logic here — just the high-band/envelope/local-alignment
building blocks several detectors reuse.
"""
from __future__ import annotations

import numpy as np


def highband(y: np.ndarray) -> np.ndarray:
    """Cheap high-pass via first difference — emphasizes attack edges (>~300 Hz)."""
    return np.diff(np.asarray(y, dtype=np.float64), prepend=0.0)


def smooth_energy_env(seg: np.ndarray, sr: int, hop_s: float = 0.00025, smooth_s: float = 0.001):
    """Smoothed high-band energy envelope + hop length (samples). The smoothing is what
    keeps envelope-based measures stable on noise-based attacks (snare/hat)."""
    hb = highband(seg)
    hop = max(1, int(hop_s * sr))
    win = max(1, int(smooth_s * sr))
    if hb.size < hop + win + 1:
        return np.zeros(0), hop
    e2 = hb * hb
    env = np.array(
        [np.sqrt(np.mean(e2[i : i + win]) + 1e-20) for i in range(0, len(e2) - win, hop)]
    )
    return env, hop


def ltas(y: np.ndarray, sr: int, n_fft: int = 2048, hop: int = 512):
    """Long-Term Average Spectrum: mean magnitude per bin over Hann-windowed frames,
    plus the bin frequencies (Hz). Alignment-free — a global spectral fingerprint that
    is robust where per-onset measures are fragile (it never needs a time map)."""
    y = np.asarray(y, dtype=np.float64)
    win = np.hanning(n_fft)
    n = max(0, (len(y) - n_fft) // hop + 1)
    if n < 1:
        return np.fft.rfftfreq(n_fft, 1.0 / sr), np.zeros(n_fft // 2 + 1)
    acc = np.zeros(n_fft // 2 + 1)
    for i in range(n):
        s = i * hop
        acc += np.abs(np.fft.rfft(y[s : s + n_fft] * win))
    return np.fft.rfftfreq(n_fft, 1.0 / sr), acc / n


def mean_spectral_flux(y: np.ndarray, sr: int, n_fft: int = 1024, hop: int = 256) -> float:
    """Mean frame-to-frame spectral flux (L1 magnitude change), energy-normalized so it
    is level-invariant. Higher = more frame-to-frame churn (graininess / instability). A
    global statistic, no alignment needed. NOTE: on transient-heavy material the
    onset flux dominates and this is not a good graininess discriminator — it is meant
    for sustained / tonal material (where graininess is actually heard)."""
    y = np.asarray(y, dtype=np.float64)
    win = np.hanning(n_fft)
    n = max(0, (len(y) - n_fft) // hop + 1)
    if n < 2:
        return 0.0
    prev = None
    flux = 0.0
    norm = 0.0
    for i in range(n):
        s = i * hop
        mag = np.abs(np.fft.rfft(y[s : s + n_fft] * win))
        if prev is not None:
            flux += float(np.sum(np.abs(mag - prev)))
            norm += float(np.sum(mag))
        prev = mag
    return flux / (norm + 1e-20)


def spectral_centroid_hz(freqs: np.ndarray, mag: np.ndarray) -> float:
    """Energy-weighted mean frequency (brightness). Scale-invariant, so silence padding
    and level differences don't move it — only timbre does."""
    total = float(np.sum(mag))
    return float(np.sum(freqs * mag) / total) if total > 1e-20 else 0.0


def normalized_correlate(long: np.ndarray, short: np.ndarray) -> np.ndarray:
    """Sliding normalized cross-correlation of `short` within `long` (values in ~[-1,1]).

    Normalizing by the local window energy stops a loud body/tail from outscoring the
    actual attack match — the bias an unnormalized `np.correlate` has on real material.
    The peak value doubles as a match-confidence score.
    """
    long = np.asarray(long, dtype=np.float64)
    short = np.asarray(short, dtype=np.float64)
    L = len(short)
    if len(long) < L or L == 0:
        return np.zeros(0)
    xc = np.correlate(long, short, mode="valid")
    sliding = np.sqrt(np.convolve(long * long, np.ones(L), mode="valid")[: len(xc)]) + 1e-12
    return xc / (sliding * (np.linalg.norm(short) + 1e-12))


def local_align(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    ref_t: float,
    cand_t: float,
    pre_ms: float = 6.0,
    post_ms: float = 26.0,
    search_ms: float = 12.0,
):
    """Cross-correlate the reference attack window against the candidate to find the
    exact local lag, and return sample-identical aligned windows.

    Returns (ref_seg, cand_seg, lag_samples) where lag_samples is the candidate offset,
    relative to the nominal `cand_t`, at which the reference best matches (positive =
    candidate attack is later than nominal). Returns (None, None, 0) at a boundary.
    """
    pre = int(pre_ms * sr / 1000.0)
    post = int(post_ms * sr / 1000.0)
    search = int(search_ms * sr / 1000.0)
    rc = int(round(ref_t * sr))
    cc = int(round(cand_t * sr))
    if rc - pre < 0 or rc + post > len(reference):
        return None, None, 0
    ref_seg = reference[rc - pre : rc + post]
    c0 = max(0, cc - pre - search)
    c1 = min(len(candidate), cc + post + search)
    cwin = candidate[c0:c1]
    if len(cwin) < len(ref_seg):
        return None, None, 0
    ncc = normalized_correlate(highband(cwin), highband(ref_seg))
    if ncc.size == 0:
        return None, None, 0
    best = int(np.argmax(ncc))
    cand_seg = cwin[best : best + len(ref_seg)]
    if len(cand_seg) != len(ref_seg):
        return None, None, 0
    # lag relative to nominal cand_t: cand window start (c0+best) vs nominal (cc-pre)
    lag_samples = (c0 + best) - (cc - pre)
    return ref_seg, cand_seg, lag_samples
