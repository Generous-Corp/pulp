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


# ── Harmonic-to-noise ratio (tonal purity) ───────────────────────────────

def harmonic_to_noise_ratio_db(
    y: np.ndarray, sr: int, fmin: float = 70.0, fmax: float = 500.0,
    n_fft: int = 1024, hop: int = 512,
) -> float:
    """Mean autocorrelation-based harmonic-to-noise ratio (dB) over energetic frames.

    Boersma's method (as in Praat): for each frame the normalized autocorrelation is
    divided by the window's own autocorrelation (removing the window's lag bias), and
    the peak r in the pitch-lag range estimates the harmonic fraction; HNR =
    10*log10(r/(1-r)). Higher = cleaner/more tonal; added broadband noise or roughness
    lowers it. A shorter frame keeps the pitch ~constant within the frame (so vibrato
    doesn't depress the peak). Silent frames are skipped.
    """
    y = np.asarray(y, dtype=np.float64)
    win = np.hanning(n_fft)
    # The window's own autocorrelation — divided out so r reflects signal periodicity,
    # not the window's lag decay.
    win_ac = np.fft.irfft(np.abs(np.fft.rfft(win, 2 * n_fft)) ** 2)[:n_fft]
    win_ac = win_ac / win_ac[0]
    lag_min = max(1, int(sr / fmax))
    lag_max = min(n_fft - 1, int(sr / fmin))
    if lag_max <= lag_min:
        return 0.0
    n = max(0, (len(y) - n_fft) // hop + 1)
    vals: list[float] = []
    for i in range(n):
        seg = y[i * hop : i * hop + n_fft] * win
        ac = np.fft.irfft(np.abs(np.fft.rfft(seg, 2 * n_fft)) ** 2)[:n_fft]
        if ac[0] <= 1e-12:
            continue  # silent frame
        ac = ac / ac[0]  # normalized; ac[0] == 1
        lo, hi = lag_min, lag_max + 1
        wac = win_ac[lo:hi]
        deb = np.where(wac > 1e-3, ac[lo:hi] / np.where(wac > 1e-3, wac, 1.0), 0.0)
        r = float(np.max(deb))
        r = min(max(r, 1e-6), 1.0 - 1e-6)  # clamp away from the log singularities
        vals.append(10.0 * np.log10(r / (1.0 - r)))
    return float(np.mean(vals)) if vals else 0.0


# ── Stereo image ──────────────────────────────────────────────────────────

def _as_stereo(x: np.ndarray) -> np.ndarray:
    s = np.asarray(x, dtype=np.float64)
    if s.ndim != 2 or s.shape[1] != 2:
        raise ValueError("expected an (N, 2) stereo array")
    return s


def mid_side(stereo: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(N,2) L/R -> (mid, side) where mid=(L+R)/2, side=(L-R)/2."""
    s = _as_stereo(stereo)
    return 0.5 * (s[:, 0] + s[:, 1]), 0.5 * (s[:, 0] - s[:, 1])


def stereo_width_ratio(stereo: np.ndarray) -> float:
    """RMS(side) / RMS(mid): 0 = mono, larger = wider. Level-invariant."""
    mid, side = mid_side(stereo)
    m = np.sqrt(np.mean(mid ** 2) + 1e-20)
    sd = np.sqrt(np.mean(side ** 2) + 1e-20)
    return float(sd / m)


def interchannel_correlation(stereo: np.ndarray) -> float:
    """Pearson correlation of L and R in [-1, 1]. ~1 = mono-ish, <=0 = decorrelated, and
    strongly negative = out-of-phase (a mono-compatibility / phase defect)."""
    s = _as_stereo(stereo)
    a = s[:, 0] - s[:, 0].mean()
    b = s[:, 1] - s[:, 1].mean()
    d = np.sqrt(float(np.dot(a, a)) * float(np.dot(b, b))) + 1e-20
    return float(np.dot(a, b) / d)


def theil_sen_slope(x: np.ndarray, y: np.ndarray) -> float:
    """Robust slope = median of pairwise (y_j - y_i)/(x_j - x_i). Resistant to a few
    bad onset matches, unlike OLS — used for the onset-drift trend."""
    x = np.asarray(x, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)
    n = len(x)
    if n < 2:
        return 0.0
    slopes = []
    for i in range(n):
        for j in range(i + 1, n):
            dx = x[j] - x[i]
            if dx != 0.0:
                slopes.append((y[j] - y[i]) / dx)
    return float(np.median(slopes)) if slopes else 0.0
