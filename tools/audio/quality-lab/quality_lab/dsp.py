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


class LagEstimate(NamedTuple):
    """Result of :func:`estimate_global_lag`. `lag_samples` is the candidate's constant offset
    relative to the reference (positive = candidate is LATER); `confidence` is the normalized
    cross-correlation peak in [0, 1] (1.0 = a perfect shifted match). A low confidence means no
    single constant lag reliably aligns the two — the caller must refuse to trim rather than trust
    a weak peak (a wrong alignment is worse than none)."""
    lag_samples: int
    confidence: float


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


def mean_spectral_flux(
    y: np.ndarray, sr: int, n_fft: int = 1024, hop: int = 256, *, hop_scale: float = 1.0
) -> float:
    """Mean frame-to-frame spectral flux (L1 magnitude change), energy-normalized so it
    is level-invariant. Higher = more frame-to-frame churn (graininess / instability). A
    global statistic, no alignment needed. NOTE: on transient-heavy material the
    onset flux dominates and this is not a good graininess discriminator — it is meant
    for sustained / tonal material (where graininess is actually heard).

    `hop_scale` (Tier 3, `spectral_flux.v2-warp`): the effective hop is `round(hop * hop_scale)`,
    `n_fft` unchanged. A pitch-preserving time-stretch by R slows content evolution, so a candidate
    measured at the reference hop reads a FALSE lower flux ("smoother"); measuring the R×-stretched
    candidate with `hop_scale=R` steps it through the same source-content interval per frame, undoing
    the deflation. `hop_scale=1.0` (default) is byte-identical to the base metric. (Because the metric
    is Σflux/Σnorm — a per-frame-step average — scaling the hop is the correct fix; a fixed `n_fft`
    leaves a small window-span residual that never flips the sign at meaningful ratios.)"""
    y = np.asarray(y, dtype=np.float64)
    hop = max(1, int(round(hop * hop_scale)))
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
    reference: np.ndarray, candidate: np.ndarray,
    present_frac: float = 0.01, abs_floor: float = 1e-6,
) -> DcOffset:
    """Per-signal DC offset (mean sample value) + its magnitude relative to RMS.

    A DC component concentrates energy in LTAS bin 0, which pulls the spectral centroid DOWN —
    so a nonzero offset can read as tonal 'dulling' when nothing timbral changed. This is a
    deterministic, algorithm-agnostic diagnostic for `compare`'s advisory namespace; it makes no
    good/bad judgment. `present` fires when either signal's |mean|/RMS reaches ``present_frac``
    (default 1%, ~-40 dB) AND the raw |mean| clears ``abs_floor`` (default 1e-6). The absolute
    floor is what stops a NEAR-SILENT signal — where the ratio is meaningful but the actual DC is
    negligible (and rounds to 0.0 in the report) — from raising a loud 'DC present' advisory over
    displayed zeros. Pure; operates on the raw (not level-matched) inputs."""
    ref = np.asarray(reference, dtype=np.float64)
    cand = np.asarray(candidate, dtype=np.float64)
    ref_mean = float(np.mean(ref)) if ref.size else 0.0
    cand_mean = float(np.mean(cand)) if cand.size else 0.0
    ref_rms = float(np.sqrt(np.mean(ref * ref))) if ref.size else 0.0
    cand_rms = float(np.sqrt(np.mean(cand * cand))) if cand.size else 0.0
    ref_frac = abs(ref_mean) / ref_rms if ref_rms > 1e-12 else 0.0
    cand_frac = abs(cand_mean) / cand_rms if cand_rms > 1e-12 else 0.0
    present = (max(ref_frac, cand_frac) >= present_frac
               and max(abs(ref_mean), abs(cand_mean)) >= abs_floor)
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


def ltas_log_spectral_distance_db(
    reference: np.ndarray,
    candidate: np.ndarray,
    sr: int,
    n_fft: int = 2048,
    hop: int = 512,
    floor_db: float = -100.0,
) -> float:
    """Long-term-average-spectrum log-spectral distance: the mean absolute dB difference between the
    two signals' LTAS bins.

    A warp-compatible companion to :func:`null_residual_db`. Where the sample-domain residual is
    time-domain and phase-SENSITIVE (a pure delay or an unaligned time-stretch/pitch-shift inflates
    it, so it is invalid as corroboration for ``stretch:R`` / ``pitch:S`` pairs), this measure is
    computed from magnitude spectra only — phase-BLIND — and from the LTAS, whose bin count depends on
    ``n_fft`` alone, not on signal length — so two DIFFERENT-length renders still compare with no
    alignment and no shape crash. It fills the corroboration slot the residual cannot: an independent,
    deterministic, algorithm-agnostic raw measure of *whether a material spectral change exists*
    across a time warp.

    Value: each signal's LTAS (:func:`ltas`) is converted to dB and the result is
    ``mean(|ref_db - cand_db|)`` over the shared bins — 0.0 for identical magnitude spectra, growing
    with the average per-bin dB deviation as the envelopes diverge. Each signal is RMS-normalized
    first, so a broadband gain cancels and only spectral SHAPE contributes. A shared magnitude floor
    ``floor_db`` below the louder peak bin bounds a real-vs-dead bin gap to ``<= |floor_db|`` (no
    single dead bin sends the mean to +inf) while keeping a real spectral hole material and monotonic.

    Blind spots (the honest inverse of the residual's): it sees only the long-term spectral ENVELOPE —
    a pure phase/all-pass defect, a time reversal, or a delay leaves the average magnitude spectrum
    intact → distance ≈ 0. It is bin-resolution- and level-guard-limited. That complementary blindness
    is exactly why the two comparators coexist rather than one replacing the other."""
    ref = np.asarray(reference, dtype=np.float64)
    cand = np.asarray(candidate, dtype=np.float64)
    ref_rms = np.sqrt(np.mean(ref * ref)) if ref.size else 0.0
    cand_rms = np.sqrt(np.mean(cand * cand)) if cand.size else 0.0
    if ref_rms > 1e-12:
        ref = ref / ref_rms
    if cand_rms > 1e-12:
        cand = cand / cand_rms
    _, m_ref = ltas(ref, sr, n_fft, hop)
    _, m_cand = ltas(cand, sr, n_fft, hop)
    n = min(m_ref.size, m_cand.size)  # identical for equal n_fft/sr; min() is a length-safety guard
    if n == 0:
        return 0.0
    m_ref, m_cand = m_ref[:n], m_cand[:n]
    peak = max(float(m_ref.max()), float(m_cand.max()), 1e-20)
    floor = peak * (10.0 ** (floor_db / 20.0))
    ref_db = 20.0 * np.log10(np.maximum(m_ref, floor))
    cand_db = 20.0 * np.log10(np.maximum(m_cand, floor))
    return float(np.mean(np.abs(ref_db - cand_db)))


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


def estimate_global_lag(
    reference: np.ndarray, candidate: np.ndarray, sr: int, max_lag_s: float = 0.5
) -> LagEstimate:
    """Estimate the single constant lag that best aligns `candidate` to `reference`.

    Cross-correlates the two high-band (attack-emphasized) signals via FFT, unit-normalized so the
    peak is a match confidence in [0, 1]. Searches lags in ±``max_lag_s``; the returned
    ``lag_samples`` is the candidate offset (positive = candidate is later, so trimming its first
    ``lag`` samples aligns the shared content). Zero-padded to 2N so the correlation is linear (no
    circular wrap) over the search window. A low ``confidence`` means the difference is NOT a pure
    delay (a real timbral/structural change, not just a shift) — the caller must then refuse to
    trim. Pure numpy; deterministic."""
    ref = highband(np.asarray(reference, dtype=np.float64))
    cand = highband(np.asarray(candidate, dtype=np.float64))
    n = min(ref.size, cand.size)
    if n < 2:
        return LagEstimate(0, 0.0)
    ref, cand = ref[:n], cand[:n]
    rn = ref / (np.linalg.norm(ref) + 1e-20)
    cn = cand / (np.linalg.norm(cand) + 1e-20)
    m = 1 << int(np.ceil(np.log2(2 * n)))                 # >= 2N → linear (non-circular) xcorr
    xcorr = np.fft.irfft(np.fft.rfft(cn, m) * np.conj(np.fft.rfft(rn, m)), m)
    # xcorr[k] peaks at k = the lag where cand[i] ≈ ref[i-k]; positive lags at [0..], negatives wrap
    # to the tail [m-.. : m]. Assemble the ±max_lag window in increasing lag order.
    max_lag = min(int(max_lag_s * sr), n - 1)
    window = np.concatenate([xcorr[m - max_lag:], xcorr[: max_lag + 1]])  # lags -max_lag..+max_lag
    lags = np.arange(-max_lag, max_lag + 1)
    # Pick the POSITIVE-correlation peak (signed argmax), not abs(): a polarity-inverted or
    # half-period-shifted candidate produces a strong NEGATIVE peak that abs() would lock onto,
    # yielding a confident WRONG lag (e.g. aligning a sine to anti-phase).
    best = int(np.argmax(window))
    peak, lag = float(window[best]), int(lags[best])
    confidence = max(0.0, min(peak, 1.0))
    # Refuse two ambiguous cases (a wrong alignment is worse than none):
    #  (1) Periodicity: a periodic/tonal signal has a COMB of near-equal peaks, so no single lag is
    #      the true one. If a rival peak outside the main lobe rivals the best (>85%), the lag is
    #      not unique → confidence 0. (Drums etc. have one dominant peak, ~30%, and pass.)
    #  (2) Boundary: a best lag at the search edge means the true lag is probably OUTSIDE the
    #      ±max_lag window (a delay longer than max_lag_s), so the in-window peak is spurious.
    lobe = max(1, int(0.001 * sr))                        # ±1 ms main-lobe half-width
    masked = window.copy()
    masked[max(0, best - lobe): best + lobe + 1] = -np.inf
    rival = float(np.max(masked))
    if peak > 1e-9 and rival / peak > 0.85:
        confidence = 0.0
    if abs(lag) >= max_lag:
        confidence = 0.0
    return LagEstimate(lag, confidence)


def apply_lag_trim(
    reference: np.ndarray, candidate: np.ndarray, lag_samples: int
) -> tuple[np.ndarray, np.ndarray]:
    """Remove the constant lag (from :func:`estimate_global_lag`) by dropping the LEAD off whichever
    signal starts later, so aligned content lines up at index 0. The tails are LEFT INTACT (unequal
    lengths are fine): the downstream residual zero-pads the shorter, so a candidate that is a
    delayed reference PLUS extra tail content still reads as material — trimming only the lead never
    truncates a real difference into a false identity. Returns new arrays."""
    ref = np.asarray(reference, dtype=np.float64)
    cand = np.asarray(candidate, dtype=np.float64)
    if lag_samples > 0:        # candidate is later → drop its lead
        cand = cand[lag_samples:]
    elif lag_samples < 0:      # candidate is earlier → drop the reference's lead
        ref = ref[-lag_samples:]
    return ref.copy(), cand.copy()


def attack_rise(seg: np.ndarray, sr: int) -> float:
    """Attack sharpness of an anchored window = amplitude / 10-90% rise time of the smoothed
    high-band energy envelope (energy per sample). Higher = sharper. Shared by the
    `transient_sharpness` detector and `compare`'s `transient-integrity` axis so the two measure
    attacks identically (no forked DSP)."""
    env, hop = smooth_energy_env(seg, sr)
    if env.size < 4:
        return 0.0
    peak = int(np.argmax(env))
    if peak < 1:
        return 0.0
    base = float(np.min(env[: peak + 1]))
    amp = float(env[peak]) - base
    if amp <= 1e-12:
        return 0.0
    lo_th, hi_th = base + 0.1 * amp, base + 0.9 * amp
    pre = env[: peak + 1]
    below_lo = np.where(pre <= lo_th)[0]
    below_hi = np.where(pre <= hi_th)[0]
    i_lo = int(below_lo[-1]) if below_lo.size else 0
    i_hi = int(below_hi[-1]) if below_hi.size else peak
    rise_samples = max(1, (i_hi - i_lo) * hop)
    return (0.8 * amp) / rise_samples


# ── Resampling (varispeed alignment) ─────────────────────────────────────

def _kaiser_sinc_resample(y: np.ndarray, target_len: int, zeros: int, beta: float) -> np.ndarray:
    """Windowed-sinc (Kaiser) arbitrary-ratio resampler — the shared core of
    :func:`resample_to_length` / :func:`resample_by_ratio`. See those for the contract."""
    y = np.asarray(y, dtype=np.float64)
    if y.ndim != 1:
        raise ValueError("resampling expects a 1-D signal")
    n = y.size
    m = int(target_len)
    if m < 0:
        raise ValueError(f"target_len must be non-negative, got {target_len}")
    if m == 0 or n == 0:
        return np.zeros(m, dtype=np.float64)
    if m == n:
        return y.copy()                      # identity ratio — no filtering, exact passthrough
    if n == 1:
        return np.full(m, y[0], dtype=np.float64)
    if m == 1:
        # A single output sample from a longer signal — the endpoint map (÷ m-1) is undefined here;
        # the bandlimited-to-one-sample value is the DC (mean), so no ZeroDivisionError on degenerate
        # (e.g. 1-sample-reference) input. The downstream length gate then handles it as not_applicable.
        return np.array([float(np.mean(y))], dtype=np.float64)

    # Endpoint-anchored mapping: output sample j reads input coordinate t_j = j*(n-1)/(m-1), so the
    # first/last samples pin exactly. This makes a forward-then-inverse round trip sample-aligned
    # (the two linear maps compose to the identity) — critical for the phase-sensitive null-residual
    # to read identity on a transparent varispeed round trip.
    scale = m / n
    cutoff = min(1.0, scale)                 # anti-alias cutoff (input-Nyquist units): lowpass only
    #                                          when DOWN-sampling (scale < 1); pass-through when up.
    step = (n - 1) / (m - 1)
    t = np.arange(m, dtype=np.float64) * step
    base = np.floor(t).astype(np.int64)
    frac = t - base                          # fractional offset in [0, 1) per output sample
    half_width = zeros / cutoff              # kernel half-support in INPUT samples (widens as cutoff
    #                                          shrinks, so the lobe count `zeros` is preserved)
    k_max = int(np.ceil(half_width))
    i0_beta = np.i0(beta)
    out = np.zeros(m, dtype=np.float64)
    wsum = np.zeros(m, dtype=np.float64)     # per-output weight sum → normalized for exact DC gain
    #                                          and clean edge roll-off (no boundary darkening).
    for k in range(-k_max, k_max + 1):
        arg = k - frac                       # kernel argument = (base + k) - t, in input samples
        idx = base + k
        within = np.abs(arg) <= half_width
        w = np.zeros(m, dtype=np.float64)
        a = arg[within]
        # Kaiser-windowed sinc at the anti-alias cutoff. sinc bandlimits; the Kaiser window bounds
        # the support with a deep, tunable stopband (beta) so no imaging/aliasing leaks into the band
        # the compare axes read.
        kaiser = np.i0(beta * np.sqrt(np.maximum(0.0, 1.0 - (a / half_width) ** 2))) / i0_beta
        w[within] = cutoff * np.sinc(cutoff * a) * kaiser
        valid = (idx >= 0) & (idx < n)       # taps off the signal ends contribute nothing …
        out += np.where(valid, y[np.clip(idx, 0, n - 1)] * w, 0.0)
        wsum += np.where(valid, w, 0.0)      # … and are excluded from the normalizer, so the edge
    #                                          filter renormalizes over only its in-range taps.
    nz = wsum != 0.0
    out[nz] /= wsum[nz]
    return out


def resample_to_length(y: np.ndarray, target_len: int, *, zeros: int = 32, beta: float = 13.0) -> np.ndarray:
    """Resample a 1-D signal to ``target_len`` samples with a Kaiser-windowed-sinc kernel.

    High-quality bandlimited resampling in pure numpy (no scipy). For each output sample the value
    is a windowed-sinc interpolation over the ``2*zeros(/cutoff)+1`` nearest input samples; when
    DOWN-sampling (``target_len < len(y)``) the sinc cutoff drops to the new Nyquist so no aliasing
    folds back into the band, and when UP-sampling the kernel is a plain bandlimiting interpolator
    (cutoff at the input Nyquist, so no imaging above it). Per-output weight normalization gives
    exact DC gain and clean edge roll-off (no boundary darkening).

    This is the varispeed-alignment primitive (spec §3b / §4.1): a varispeed candidate is *exactly*
    a resample of the reference, so resampling it back to the reference length undoes the speed
    change and lets the entire alignment-free compare pipeline — including the phase-sensitive
    sample-domain null-residual — measure it unchanged. The round trip is transparent to the compare
    axes: resampling a signal by a ratio and back reads ``no_material_change`` on every axis, and a
    lossless (up-sample-first) round trip returns to within numerical noise (residual < -100 dB).

    Quality is high enough not to itself trip an axis: a windowed-sinc round trip does NOT add HF
    fizz, roughness, or graininess (a naive linear interpolation would, via imaging/roll-off). The
    one honest limit is physical, not a resampler defect: a DOWN-sample to below the source length
    (a speed-up varispeed, ratio < 1) bandlimits to the new lower Nyquist, so any source energy above
    it is unrecoverable — a signal with substantial near-Nyquist energy reads as duller after such a
    round trip. That is the correct behavior of a speed-up (nothing generates the lost top octave),
    matching an ideal brickwall to within ~1%.

    ``zeros`` is the sinc lobe count per side (more = sharper transition, slower); ``beta`` is the
    Kaiser shape (higher = deeper stopband, wider transition). Defaults (32, 13.0) sit well below the
    measurement floor. Cost is ``O(target_len * zeros / cutoff)`` — ~0.25 s per direction on a
    2 s / 48 kHz render. Endpoints are pinned, so a forward+inverse pair is sample-aligned. Raises
    ValueError on non-1-D input or a negative length."""
    return _kaiser_sinc_resample(y, target_len, zeros, beta)


def resample_by_ratio(y: np.ndarray, ratio: float, *, zeros: int = 32, beta: float = 13.0) -> np.ndarray:
    """Resample a 1-D signal by a length ``ratio`` — output length ``round(len(y) * ratio)``.

    Convenience wrapper over :func:`resample_to_length` in the varispeed vocabulary of spec §4.1: a
    ``varispeed:R`` candidate is ``resample_by_ratio(reference, R)`` (``R > 1`` slows/lengthens and
    lowers pitch; ``R < 1`` speeds up/shortens and raises pitch), and resampling it back —
    ``resample_by_ratio(candidate, 1 / R)`` or, for an exact length match against the reference,
    ``resample_to_length(candidate, len(reference))`` — returns it to the reference time base. The
    endpoint-anchored mapping makes ``round(len*R)`` then ``round(.../R)`` recover the original length
    for the ratios the varispeed lane uses. Raises ValueError on a non-positive ratio or non-1-D
    input."""
    if not ratio > 0.0:
        raise ValueError(f"ratio must be positive, got {ratio}")
    y = np.asarray(y, dtype=np.float64)
    return _kaiser_sinc_resample(y, int(round(y.size * ratio)), zeros, beta)


def onset_attack_deficit(
    reference: np.ndarray, candidate: np.ndarray, sr: int, ref_t: float, cand_t: float
) -> float | None:
    """Attack-smear deficit in [0,1] for ONE onset pair (0 = faithful/sharper, 1 = fully softened),
    or None when the window can't be measured (boundary, or a silent reference attack). Locks the
    pair with sub-hop cross-correlation (:func:`local_align`) then compares the high-band attack rise
    (:func:`attack_rise`). Clipping to [0,1] tames the small-``s_ref`` blow-up a raw ratio has. The
    single home for the per-onset attack measurement — shared by the transient_sharpness detector and
    compare's transient-integrity axis (no forked loop)."""
    ref_seg, cand_seg, _lag = local_align(reference, candidate, sr, ref_t, cand_t)
    if ref_seg is None:
        return None
    s_ref, s_cand = attack_rise(ref_seg, sr), attack_rise(cand_seg, sr)
    if s_ref <= 1e-9:
        return None
    return float(np.clip(1.0 - s_cand / s_ref, 0.0, 1.0))


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
