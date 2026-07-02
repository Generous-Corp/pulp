"""Shared DSP primitives for the Layer-A detectors (§14.4: factor shared DSP into
common primitives rather than copying per detector).

Pure numpy. No detector logic here — just the high-band/envelope/local-alignment
building blocks several detectors reuse.
"""
from __future__ import annotations

from typing import NamedTuple

import numpy as np


class NullResidual(NamedTuple):
    """Result of :func:`null_residual_db`. `residual_db` is the reference-relative residual level
    (or +inf when the reference can't be normalized); `ref_rms_db` is the reference's own RMS in
    dB; `level_matched` is False when the common region was too quiet to define a level-match gain,
    so the residual is raw (un-matched) — the reader must not read it as a calibrated difference."""
    residual_db: float
    ref_rms_db: float
    level_matched: bool


class DcOffset(NamedTuple):
    """Result of :func:`dc_offset_metrics`. `ref_mean`/`cand_mean` are the raw DC (mean sample
    value) of each signal; `ref_frac`/`cand_frac` are those magnitudes relative to each signal's
    own RMS; `present` is True when either fraction reaches the significance floor — i.e. the DC
    component is large enough to bias LTAS bin 0 and masquerade as a tonal change."""
    ref_mean: float
    cand_mean: float
    ref_frac: float
    cand_frac: float
    present: bool


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


def relative_centroid_shift(
    reference: np.ndarray, candidate: np.ndarray, sr: int
) -> tuple[float, float, float]:
    """LTAS spectral-centroid relative shift of candidate vs reference plus both centroids
    (Hz): ``rel = (c_cand - c_ref) / c_ref`` (negative = duller, positive = brighter), and
    ``0.0`` when the reference has no spectral energy. Shared by the ``spectral_centroid``
    detector and the ``compare`` report so the two can never drift apart."""
    f, m_ref = ltas(reference, sr)
    _, m_cand = ltas(candidate, sr)
    c_ref = spectral_centroid_hz(f, m_ref)
    c_cand = spectral_centroid_hz(f, m_cand)
    rel = (c_cand - c_ref) / c_ref if c_ref > 1e-9 else 0.0
    return rel, c_ref, c_cand


def hf_band_bin_count(sr: int, cutoff_hz: float = 8000.0, n_fft: int = 2048) -> int:
    """Number of LTAS bins at/above ``cutoff_hz`` for this sample rate + FFT size.

    How much spectral support the added-HF band actually has. As the Nyquist frequency
    approaches the cutoff the band collapses to one or two bins, where an energy *fraction*
    over it is meaningless — e.g. at ``sr=16 kHz`` the ``>=8 kHz`` band is the single Nyquist
    bin. `compare`'s added-hf axis uses this to declare `not_applicable` rather than report a
    confident fraction over a degenerate band. Must be called with the SAME ``n_fft`` the LTAS
    uses (`ltas` default 2048) or the count will not match the measured band."""
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
    return int(np.count_nonzero(freqs >= cutoff_hz))


def hf_energy_fraction(freqs: np.ndarray, mag: np.ndarray, cutoff_hz: float) -> float:
    """Fraction of summed LTAS magnitude-squared at/above ``cutoff_hz`` (0..1).

    ``mag`` is the long-term-average *magnitude* spectrum, so this squares the mean magnitude
    per bin — it is NOT a true mean frame-power LTAS. It is a scale-invariant brightness/fizz
    proxy and the established ``hf_fizz`` metric; thresholds are tuned to exactly this
    definition, so renaming it or switching to a power-LTAS would require re-tuning them.
    """
    energy = mag * mag
    total = float(np.sum(energy))
    if total <= 1e-20:
        return 0.0
    return float(np.sum(energy[freqs >= cutoff_hz]) / total)


def hf_fraction_delta(
    reference: np.ndarray, candidate: np.ndarray, sr: int, cutoff_hz: float = 8000.0
) -> tuple[float, float, float]:
    """Added-HF energy-fraction of candidate vs reference: ``(hf_cand - hf_ref)`` plus both
    fractions. Positive = candidate added high-frequency energy (metallic fizz/sizzle the
    source didn't have). Shared by the ``hf_fizz`` detector and the ``compare`` report."""
    f, m_ref = ltas(reference, sr)
    _, m_cand = ltas(candidate, sr)
    hf_ref = hf_energy_fraction(f, m_ref, cutoff_hz)
    hf_cand = hf_energy_fraction(f, m_cand, cutoff_hz)
    return hf_cand - hf_ref, hf_ref, hf_cand


def hf_fraction_ratio_db(
    reference: np.ndarray, candidate: np.ndarray, sr: int,
    cutoff_hz: float = 8000.0, floor: float = 1e-6,
) -> tuple[float, float, float]:
    """Band-relative high-frequency change: ``10*log10(frac_cand / frac_ref)`` in dB, plus both
    ≥``cutoff_hz`` energy *fractions*.

    Each ``frac`` is the ≥cutoff share of the signal's OWN total LTAS energy
    (:func:`hf_energy_fraction`), so the metric is a RATIO of fractions — invariant to a broadband
    gain and to the compare level-match (both numerator and denominator scale together). That is
    the fix for the absolute ≥8 kHz *fraction delta*, which is signal-dependent and effectively
    blind on a bass-heavy source (a harsh amp render's HF fraction is ~1e-4, so even a clearly
    harsh addition barely moves the absolute number). Positive dB = candidate is relatively
    brighter/harsher; negative = relatively duller.

    Both fractions are floor-clamped (default 1e-6, ~-60 dB, far below any real HF fraction) so a
    zero-HF reference (fizz added to a dark source) or a zero-HF candidate (brickwall low-pass,
    silent render) yields a large FINITE dB delta and a normal verdict — never ``-inf`` → an
    `invalid` report that would break the "nonzero exit only when we could not measure" contract.
    NOTE: a broadband gain cancels exactly, but an EQ move that redistributes energy across the
    cutoff (e.g. a large LF shelf) still moves the fraction somewhat — far less than it moves the
    absolute band energy through the level-match, but not to zero."""
    f, m_ref = ltas(reference, sr)
    _, m_cand = ltas(candidate, sr)
    hf_ref = hf_energy_fraction(f, m_ref, cutoff_hz)
    hf_cand = hf_energy_fraction(f, m_cand, cutoff_hz)
    ratio_db = 10.0 * np.log10(max(hf_cand, floor) / max(hf_ref, floor))
    return float(ratio_db), hf_ref, hf_cand


def dc_offset_metrics(
    reference: np.ndarray, candidate: np.ndarray, present_frac: float = 0.01
) -> DcOffset:
    """Per-signal DC offset (mean sample value) + its magnitude relative to RMS.

    A DC component concentrates energy in LTAS bin 0, which pulls the spectral centroid DOWN —
    so a nonzero offset can read as tonal 'dulling' when nothing timbral changed. This is a
    deterministic, algorithm-agnostic diagnostic for `compare`'s advisory namespace; it makes no
    good/bad judgment. `present` fires when either signal's |mean|/RMS reaches ``present_frac``
    (default 1%, ~-40 dB) — small enough to catch a real offset, large enough not to trip on the
    numeric floor. Pure; operates on the raw (not level-matched) inputs."""
    ref = np.asarray(reference, dtype=np.float64)
    cand = np.asarray(candidate, dtype=np.float64)
    ref_mean = float(np.mean(ref)) if ref.size else 0.0
    cand_mean = float(np.mean(cand)) if cand.size else 0.0
    ref_rms = float(np.sqrt(np.mean(ref * ref))) if ref.size else 0.0
    cand_rms = float(np.sqrt(np.mean(cand * cand))) if cand.size else 0.0
    ref_frac = abs(ref_mean) / ref_rms if ref_rms > 1e-12 else 0.0
    cand_frac = abs(cand_mean) / cand_rms if cand_rms > 1e-12 else 0.0
    present = max(ref_frac, cand_frac) >= present_frac
    return DcOffset(ref_mean, cand_mean, ref_frac, cand_frac, present)


def null_residual_db(reference: np.ndarray, candidate: np.ndarray) -> NullResidual:
    """Sample-domain null-difference residual of a candidate against the reference.

    ``residual_db`` is ``20*log10(rms(candidate - reference) / rms(reference))`` — the residual
    energy *relative to the reference's own level*. A very negative value (e.g. -80 dB) means the
    two signals are nearly identical sample-for-sample; near 0 dB means they differ as much as the
    signal itself.

    Level: the candidate is RMS-matched to the reference over the **common region** (the overlap),
    not the full length — length-robust, since matching over the full length would let trailing
    silence on one side dilute its RMS and spuriously amplify the shared content. When the common
    region is too quiet to define a gain (e.g. a full-render delay pushes content out of the
    overlap, or a truncation keeps only a silent head), the residual is still computed with NO
    level match and ``level_matched=False`` — never dropped, because that silent-overlap case is
    exactly a material dropped/shifted tail the residual must surface.

    Length: the shorter signal is **zero-padded to the longer length** (never truncated), so a
    dropped/added tail is measured, not silently ignored. A candidate that is the first half of
    the reference includes the missing second half as residual energy and reads as material; a
    difference that is only trailing silence contributes ~0 residual and stays immaterial. This is
    what lets the residual back a truthful identity claim regardless of render length.

    This is a deterministic, algorithm-agnostic, global measure — it makes NO perceptual claim
    and NO judgment of *which* signal is better. It is time-domain and phase-sensitive by
    construction: a pure delay or all-pass/phase difference between two perceptually similar
    renders inflates the residual, so it corroborates *materiality* (a real change exists), never
    audibility. Only a fully silent reference (or empty input) yields ``residual_db=inf``; callers
    treat that as not-applicable."""
    ref = np.asarray(reference, dtype=np.float64)
    cand = np.asarray(candidate, dtype=np.float64)
    if ref.size == 0 or cand.size == 0:
        return NullResidual(float("inf"), float("-inf"), False)
    ref_rms = np.sqrt(np.mean(ref * ref))  # the reference's TRUE level (never diluted by padding)
    if ref_rms <= 1e-12:
        return NullResidual(float("inf"), float("-inf"), False)
    n_common = min(ref.size, cand.size)
    cand_common_rms = np.sqrt(np.mean(cand[:n_common] * cand[:n_common]))
    ref_common_rms = np.sqrt(np.mean(ref[:n_common] * ref[:n_common]))
    if cand_common_rms > 1e-12 and ref_common_rms > 1e-12:
        gain, level_matched = ref_common_rms / cand_common_rms, True  # length-robust overlap match
    else:
        # Overlap too quiet to define a gain — the difference lives entirely outside the overlap
        # (a shifted/dropped loud tail). Keep the residual RAW (no match) and flag it, rather than
        # dropping the advisory exactly when a material tail needs surfacing.
        gain, level_matched = 1.0, False
    matched = cand * gain
    n = max(ref.size, cand.size)
    ref_p = np.zeros(n); ref_p[: ref.size] = ref
    cand_p = np.zeros(n); cand_p[: matched.size] = matched
    residual = cand_p - ref_p
    res_rms = np.sqrt(np.mean(residual * residual))
    ref_rms_db = 20.0 * np.log10(ref_rms)
    if res_rms <= 1e-12:
        # Bit-identical (or below the numeric floor) — report a deep, finite floor rather than
        # -inf so the value stays JSON-friendly and orderable.
        return NullResidual(-160.0, float(ref_rms_db), level_matched)
    return NullResidual(float(20.0 * np.log10(res_rms / ref_rms)), float(ref_rms_db), level_matched)


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
