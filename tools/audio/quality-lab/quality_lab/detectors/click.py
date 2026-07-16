"""Click detector — an UNEXPECTED discontinuity in an oscillator's output.

Reference-free and standalone (like `stereo_width`, deliberately not in the mono
pipeline registry): it analyzes one render, not a reference/candidate pair.

## The rule

The naive rule — "a large sample-to-sample step is a click" — is wrong for this
domain, and wrong in the direction that matters: **a square wave's edge IS a large
step, by design.** So is a saw's wrap, and so is a hard-sync reset. A detector built
on step size flags every correct oscillator.

What separates a click from an edge is not the size of the discontinuity but whether
the waveform *predicted* it. A steady oscillator repeats at its period P: every
legitimate edge — however violent — recurs one period later, identically. A
band-switch seam, a crossfade seam, a voice-steal pop, or a block-rate zipper does
not. So:

> **A discontinuity is legitimate iff the previous period contains the same
> discontinuity at the same phase. The oscillator's own previous period is the frozen
> reference.**

That last sentence is the whole design. The plan's rewrite of this gate asks for the
residual against "a reference render with the morph parameter frozen"; for a
reference-free analyzer the preceding period *is* that frozen render, and it comes
free with the signal.

Concretely:

1. Seed the period from the caller's `f0_hint`, or from a YIN dip (`seed_period_samples`).
2. Fit P by minimizing the comb residual `y[n] - y[n-P]` (`fit_period`). P is
   fractional, and the fit — not the seed — is what makes it exact enough to null
   against: a 0.01-sample period error already reads as a -40 dB residual.
3. Delay by the fractional P with a Kaiser-windowed sinc and subtract.
4. Band-limit the residual, then report its largest excursion relative to peak, in dB.

A square edge cancels against its own previous edge and leaves ~0. A one-off seam has
no predecessor and survives at its full height.

### Why fractionally delaying a "discontinuity" is safe here

The natural objection is that P is not an integer, and interpolating a discontinuity
is where an interpolator is worst. That objection does not apply: the *underlying
waveform* has a jump, but the *sampled sequence* is bandlimited, and sinc
interpolation of a bandlimited sequence is exact. The jump is not in the data.

Where it does apply is an **aliased** oscillator — a naively-sampled saw is not
bandlimited, its periods genuinely differ, and the comb cannot null it. That is a real
coupling worth stating: **this detector's floor on a given oscillator is bounded by
that oscillator's own alias level.** It measures clicks on top of a bandlimited
oscillator; on an aliasing one, the alias analyzer is the gate that applies.

### Why the residual is band-limited (§2.1's band-qualification, applied here)

Measured, not assumed: with the full band the floor sits at **-30 dB** — useless. The
cause is the interpolator's error on harmonics within a few percent of Nyquist, where
any finite-length sinc is inaccurate. Because a fractional delay is LTI, its error at
frequency f stays at f, so band-limiting the residual removes that error *exactly*
rather than merely smoothing it. Below `RESIDUAL_BAND_FRACTION` x Nyquist the floor
drops to about **-60 dB or better**. A step is broadband with a 1/f tail, so
discarding the top of the band costs the detector almost none of its signal.

The gate this feeds must therefore be stated as: *unexpected discontinuity energy
below 0.8 x Nyquist*. Naming the band is the point — an unqualified "no clicks" claim
read through this measurement would be exactly the silent pass §2.8 warns about.

## What this rule cannot see

Stated up front, because a gate that passes because the measurement is blind is worse
than no gate. Each of these is measured by a test in `tests/test_click.py`:

* **A defect that repeats at exactly the oscillator period** is, by construction,
  indistinguishable from part of the waveform. A -20 dB period-locked glitch — loud
  enough to hear plainly — reads -229 dB, i.e. perfectly invisible. The same glitch one
  sample off-period reads -22 dB. This is not a tuning problem; it is what the rule
  means, and it is the price of not firing on square edges.
* **A pitch that moves during the render** — glide, vibrato, portamento, FM — breaks
  the periodicity premise outright. 1 cent of glide reads -28 dB on a clean
  oscillator. This is REFUSED rather than reported, via `period_drift` (see `detect`),
  because `period_confidence` does not catch it: a glide correlates at ~1.000 one
  period on. The detector is for steady-pitch renders and says so instead of guessing.
* **Content above 0.8 x Nyquist**, per the band qualification above.

Two things that might be expected to be blind spots, but measure otherwise: a defect
landing exactly ON a legitimate edge reads the same as one between edges (the comb
SUBTRACTS the edge rather than thresholding around it, so the edge does not mask
anything), and a hard-sync reset does not need special-casing — fitting the period
finds the master period on its own.

## Why not spectral flux

§2.4 offers "or a spectral-flux outlier detector" as an equivalent route. It is a real
contender — a steady oscillator's magnitude spectrum does not change at its own edges,
so flux does not false-fire on a square (+2.9 dB against a +12 dB rule), it catches a
one-shot seam to about -40 dB, and it is immune to the drift case above. But it has a
structural hole: a block-rate parameter zipper is STATIONARY churn, so it lifts the
median along with every hop and produces no outlier at any magnitude — a -20 dB zipper
reads +2.6 dB, indistinguishable from a clean square. The residual route covers all
four defect classes and reads in physical units (the step's height), so it is the
primary; flux remains a genuine complement precisely where this rule refuses.
`tests/test_click.py` pins both halves of that comparison.
"""
from __future__ import annotations

from typing import NamedTuple

import numpy as np

from ..schema import DetectorResult, WorstRegion

TOLERANCE_CLASS = "click.v1"

# Residual band edge, as a fraction of Nyquist. Above this the fractional-delay error
# on near-Nyquist harmonics dominates the measurement (see the module docstring).
RESIDUAL_BAND_FRACTION = 0.8

# Fractional-delay kernel. 256 taps at beta=13 nulls a bandlimited saw to roughly
# -60 dB or better across the tested frequency/rate grid. The period fit uses this SAME
# kernel: fitting with a cheaper one biases the optimum toward whatever the cheap kernel
# cannot represent, which measured as a 0.03-sample period error and a -28 dB floor at
# 55 Hz. The fit must minimize the residual the detector actually reports.
_DELAY_TAPS = 256
_DELAY_BETA = 13.0

# YIN dip threshold for the no-hint period seed.
_YIN_THRESHOLD = 0.15

# How much worse than the best a shorter candidate period may score and still win in
# `_resolve_period`. Chosen from the measured gap, not derived: across the fixtures a
# genuine shortest period scores at most 3.2x the best multiple (the spread is just
# interpolator error varying with the fractional part — a period ending in .09
# interpolates better than one ending in .5), while a seed caught in the slave-period
# trap scores 11.5x or worse. 6.0 sits between, with ~2x headroom either side.
_MULTIPLE_MARGIN = 6.0


class ClickAnalysis(NamedTuple):
    """The measurement behind the detector, exposed so a caller can inspect the floor
    rather than trust the boolean."""

    click_db: float           # unexpected discontinuity, dB relative to signal peak
    click_time_s: float       # where the worst excursion sits
    period_samples: float     # the fitted period the expectation was built on
    period_confidence: float  # normalized correlation one period on, 0..1
    period_drift: float       # relative period difference between the two halves
    residual: np.ndarray      # the band-limited comb residual


def _delay_kernel(frac: float, taps: int, beta: float) -> np.ndarray:
    """Kaiser-windowed sinc for a delay of `frac` in [0,1).

    The kernel does not depend on n: for a constant delay, every output sample uses the
    same sub-sample offset, so the whole fractional delay is ONE convolution rather
    than a per-sample interpolation.
    """
    half = taps // 2
    tap = np.arange(-half + 1, half + 1, dtype=np.float64)  # tap position; the window rides on this
    return np.sinc(tap - frac) * (
        np.i0(beta * np.sqrt(np.maximum(0.0, 1.0 - (tap / half) ** 2))) / np.i0(beta)
    )


def sinc_delay(y: np.ndarray, delay: float, taps: int = _DELAY_TAPS, beta: float = _DELAY_BETA) -> np.ndarray:
    """`y(n - delay)` for fractional `delay`, by Kaiser-windowed sinc interpolation.

    Exact for a bandlimited sequence up to the kernel's accuracy, which degrades near
    Nyquist — hence the residual band limit. Boundary samples are wrong by construction
    (no data to interpolate from) and the caller guards them.
    """
    y = np.asarray(y, dtype=np.float64)
    d_int = int(np.floor(delay))
    frac = float(delay - d_int)
    # `mode="same"` already centres the kernel, so the convolution alone realizes the
    # fractional part; the integer part is a plain shift.
    frac_delayed = np.convolve(y, _delay_kernel(frac, taps, beta), mode="same")
    return np.roll(frac_delayed, d_int)


def band_limit(r: np.ndarray, sr: int, cutoff_hz: float) -> np.ndarray:
    """Zero every bin above `cutoff_hz`. A brickwall in the FFT domain: this is an
    offline analyzer, and an exact band edge beats a filter-design-dependent one when
    the band edge is part of the gate's stated meaning."""
    spec = np.fft.rfft(r)
    freqs = np.fft.rfftfreq(len(r), 1.0 / sr)
    spec[freqs > cutoff_hz] = 0.0
    return np.fft.irfft(spec, n=len(r))


def _normalized_difference(y: np.ndarray, max_lag: int) -> np.ndarray:
    """YIN's cumulative-mean-normalized difference function, via FFT autocorrelation.

    Raw autocorrelation is the wrong seed here: it is ~1 at small lags for ANY smooth
    signal, so a plain argmax on a low-frequency oscillator returns a handful of
    samples. The cumulative-mean normalization removes that small-lag bias.

    It does NOT resolve every ambiguity — under hard sync with a near-integer
    slave:master ratio the signal almost repeats at the slave period and YIN dips
    there. `_resolve_period` is what catches that.
    """
    y = y - np.mean(y)
    n = len(y)
    size = 1 << int(np.ceil(np.log2(2 * n)))
    spec = np.fft.rfft(y, size)
    ac = np.fft.irfft(spec * np.conj(spec), size)[: max_lag + 1]

    # d(tau) = sum_n (y[n] - y[n+tau])^2 over the overlap, expanded into running sums:
    #   d(tau) = S[N-tau] + (S[N] - S[tau]) - 2*ac[tau],  S[m] = sum_{n<m} y[n]^2
    cum = np.concatenate(([0.0], np.cumsum(y * y)))
    lags = np.arange(max_lag + 1)
    diff = np.maximum(0.0, cum[n - lags] + (cum[n] - cum[lags]) - 2 * ac)

    cmnd = np.ones_like(diff)
    running = np.cumsum(diff[1:])
    positive = running > 1e-20
    cmnd[1:][positive] = (diff[1:] * np.arange(1, max_lag + 1) / running)[positive]
    return cmnd


def seed_period_samples(
    y: np.ndarray, sr: int, f0_hint: float | None = None, min_hz: float = 20.0, max_hz: float = 5000.0
) -> tuple[float, float]:
    """Seed for the period fit, plus a 0..1 confidence.

    With `f0_hint`, the hint IS the seed — the caller knows what it asked the
    oscillator to render, and searching around a known answer only invites the octave
    errors below. Confidence is still measured from the signal at that lag, so a wrong
    hint reads as low confidence rather than as a silently wrong period.

    Without a hint, seed from the first YIN dip below `_YIN_THRESHOLD`. "First dip", not
    "global minimum", because the octave error matters asymmetrically: a signal with
    period P is also periodic at 2P, so seeding an octave too LOW still nulls and costs
    only some usable length, whereas seeding at P/2 nulls nothing and reads as a click.
    """
    y = np.asarray(y, dtype=np.float64)
    n = len(y)
    if n < 4 or float(np.max(np.abs(y))) <= 1e-12:
        return 0.0, 0.0

    if f0_hint is not None and f0_hint > 0:
        seed = sr / float(f0_hint)
    else:
        max_lag = min(n - 2, int(np.ceil(sr / min_hz)))
        lo = max(2, int(sr / max_hz))
        if max_lag <= lo:
            return 0.0, 0.0
        cmnd = _normalized_difference(y, max_lag)
        below = np.nonzero(cmnd[lo:] < _YIN_THRESHOLD)[0]
        if len(below):
            start = int(below[0]) + lo
            # Walk to the bottom of that first dip rather than stopping at its edge.
            while start + 1 <= max_lag and cmnd[start + 1] < cmnd[start]:
                start += 1
            seed = float(start)
        else:
            seed = float(lo + int(np.argmin(cmnd[lo:])))
    if seed < 2.0 or seed > n / 4.0:
        return float(seed), 0.0

    # Confidence: normalized correlation between the signal and itself one seed period
    # on. Measured at the seed the fit will actually use, whatever produced it.
    lag = int(round(seed))
    a, b = y[:-lag], y[lag:]
    denom = float(np.sqrt(np.sum(a * a) * np.sum(b * b)))
    conf = float(np.sum(a * b) / denom) if denom > 1e-20 else 0.0
    return float(seed), float(np.clip(conf, 0.0, 1.0))


def _residual_rms(y: np.ndarray, period: float, guard: int) -> float:
    r = (y - sinc_delay(y, period))[guard : len(y) - guard]
    return float(np.sqrt(np.mean(r * r))) if len(r) else np.inf


def fit_period(y: np.ndarray, seed: float, guard: int, span: float = 0.75, tol: float = 1e-6) -> float:
    """The period that best explains the signal: golden-section on the L2 comb residual.

    L2 (not max) is what is minimized, so one localized defect among a hundred periods
    cannot pull the fit toward hiding itself — the fit follows the bulk of the signal,
    and the defect is then measured as what the bulk cannot explain.

    The seed is only accurate to about a sample, and at a bandlimited edge the waveform
    moves by most of its amplitude within a sample — so a 0.01-sample period error reads
    as a -40 dB residual. The fit, not the seed, is what buys the floor.
    """
    golden = (np.sqrt(5.0) - 1.0) / 2.0
    a, b = seed - span, seed + span
    c, d = b - golden * (b - a), a + golden * (b - a)
    fc, fd = _residual_rms(y, c, guard), _residual_rms(y, d, guard)
    while b - a > tol:
        if fc < fd:
            b, d, fd = d, c, fc
            c = b - golden * (b - a)
            fc = _residual_rms(y, c, guard)
        else:
            a, c, fc = c, d, fd
            d = a + golden * (b - a)
            fd = _residual_rms(y, d, guard)
    return (a + b) / 2.0


def _resolve_period(y: np.ndarray, seed: float, guard: int, max_multiple: int = 4) -> float:
    """The shortest period that actually EXPLAINS the signal, searching seed multiples.

    A seed can land on a period the signal only *nearly* repeats at, and the failure is
    silent: under hard sync with a near-integer slave:master ratio (661/220 = 3.005) the
    reset is almost a no-op, so YIN dips at the SLAVE period. Fitting there nulls
    nothing, and the leftover sync discontinuity reads as a -10 dB click — at a
    confidence of 0.992. A confident false positive on a clean oscillator.

    So: fit at each multiple of the seed and keep the SHORTEST whose residual is within
    `_MULTIPLE_MARGIN` of the best. Shortest-within-margin, not simply best, because a
    signal periodic at P is also periodic at 2P — without the preference the search
    would drift to longer periods for no gain and burn usable length on guards.

    Every candidate is scored on the same interior (a guard sized for the LONGEST
    candidate), or the comparison would reward long periods for measuring less signal.
    """
    usable = (len(y) - 16) // 2 - _DELAY_TAPS
    max_multiple = max(1, min(max_multiple, int(usable / (2 * seed)) if seed > 0 else 1))
    if max_multiple == 1:
        return fit_period(y, seed, guard)

    wide_guard = int(np.ceil(2 * seed * max_multiple)) + _DELAY_TAPS
    # The span scales with k: the seed is an integer lag, so it carries up to half a
    # sample of error, and the k-th multiple inherits k times that. A fixed span leaves
    # the true period outside the bracket for k >= 2 and the fit silently clamps to the
    # bracket edge — which reads as a click.
    fits = [fit_period(y, seed * k, wide_guard, span=0.75 * k) for k in range(1, max_multiple + 1)]
    scores = [_residual_rms(y, p, wide_guard) for p in fits]
    best = min(scores)
    for period, score in zip(fits, scores):
        if score <= best * _MULTIPLE_MARGIN:
            return period
    return fits[int(np.argmin(scores))]


def measure_period_drift(y: np.ndarray, seed: float, guard: int) -> float:
    """Relative period difference between the first and second half of `y`.

    The premise of the whole rule is a STEADY period, and `period_confidence` does not
    police it: a 1-cent glide still correlates at ~1.000 one period on, yet reads as a
    -28 dB click. Confidence answers "is this periodic at all", which is the wrong
    question — a glide is locally periodic and globally not.

    Fitting the period on each half and comparing answers the right one. On the clean
    fixtures this is exactly 0.0; a 0.5-cent glide reads 1.4e-4. That gap is what lets
    a drifting render be REFUSED rather than silently reported as clicky.
    """
    half = len(y) // 2
    if half <= 2 * guard + 16:
        # Too short to fit each half independently. Returning 0.0 would assert
        # steadiness that was never measured — the silent pass this guard exists to
        # prevent. NaN fails the caller's comparison and refuses the reading instead.
        return float("nan")
    first = fit_period(y[:half], seed, guard)
    second = fit_period(y[half:], seed, guard)
    mean = (first + second) / 2.0
    return abs(second - first) / mean if mean > 1e-12 else 0.0


def analyze(y: np.ndarray, sr: int, f0_hint: float | None = None) -> ClickAnalysis:
    """Measure the largest period-unexpected discontinuity in `y`."""
    y = np.asarray(y, dtype=np.float64)
    peak = float(np.max(np.abs(y))) if len(y) else 0.0
    seed, confidence = seed_period_samples(y, sr, f0_hint)
    guard = int(np.ceil(2 * seed)) + _DELAY_TAPS
    if peak <= 1e-12 or seed < 2.0 or len(y) < 2 * guard + 16:
        return ClickAnalysis(-np.inf, 0.0, seed, confidence, 0.0, np.zeros(len(y)))

    period = _resolve_period(y, seed, guard)
    # The guard must follow the period actually used, not the seed. When the seed is the
    # slave period and the resolved period is a multiple of it, a seed-sized guard is
    # too small: the comb's first valid output moves out with the period, and reading
    # inside that leaves start-up transient in the measurement. It read as an -8 dB
    # worse floor on hard sync — a false positive's worth, from an off-by-one guard.
    guard = int(np.ceil(2 * period)) + _DELAY_TAPS
    if len(y) < 2 * guard + 16:
        return ClickAnalysis(-np.inf, 0.0, period, confidence, float("nan"), np.zeros(len(y)))

    drift = measure_period_drift(y, period, guard)
    residual = band_limit(y - sinc_delay(y, period), sr, RESIDUAL_BAND_FRACTION * sr / 2.0)

    interior = residual[guard : len(residual) - guard]
    if not len(interior):
        return ClickAnalysis(-np.inf, 0.0, period, confidence, drift, residual)
    idx = int(np.argmax(np.abs(interior))) + guard
    worst = float(abs(residual[idx]))
    click_db = 20.0 * np.log10(worst / peak) if worst > 0 else -np.inf
    return ClickAnalysis(click_db, idx / sr, period, confidence, drift, residual)


def detect(
    signal: np.ndarray,
    sr: int,
    f0_hint: float | None = None,
    fire_threshold_db: float = -45.0,
    min_period_confidence: float = 0.5,
    max_period_drift: float = 3e-5,
) -> DetectorResult:
    """Fire when `signal` contains a discontinuity its own period does not predict.

    `scalar` is the unexpected discontinuity's height in dB relative to the signal's
    peak (more negative = cleaner); `fired` is `scalar >= fire_threshold_db`.

    The default threshold sits above the MEASURED worst-case false-positive floor of
    the clean fixtures, which `tests/test_click.py` pins — it is not a derived bound,
    and it is not the detector's sensitivity on friendlier material. Tighten it only
    against a floor measured for the material at hand.

    Two preconditions make the reading REFUSED rather than reported, each surfacing as
    `low_coverage` with `fired=False` — a caller must read that as "not proven clean",
    never as "clean":

    * `period_confidence` below `min_period_confidence` — no period at all, so the rule
      has no premise.
    * `period_drift` above `max_period_drift` — the pitch moves during the render.
      This one is not fussiness: an oscillator gliding by as little as 1 cent reads
      -28 dB, well above any useful threshold, and `period_confidence` stays at 1.000
      throughout. Without this guard the detector reports a confident false positive on
      a perfectly clean oscillator. The clean fixtures measure exactly 0.0 drift.

    `max_period_drift=3e-5` is measured, and the window it sits in is NARROW — worth
    knowing before retuning it. The guard cannot distinguish a drifting pitch from a
    strong aperiodic component, because both make the two halves fit different periods.
    So the ceiling is set by the mildest glide that would false-fire (0.15 cents, drift
    4.3e-5) and the floor by the loudest defect that must still be reported (a -20 dB
    block-rate zipper, drift 1.9e-5). 3e-5 is geometrically centred between them, about
    1.5x clear on each side. Loosening it past 4.3e-5 lets glide false-fire; tightening
    it below 1.9e-5 turns a loud zipper into a refusal.

    A refusal is not a pass. `low_coverage` means "not proven clean", and a caller that
    reads it as clean has reintroduced the silent pass this all exists to prevent.
    """
    a = analyze(signal, sr, f0_hint)
    steady = a.period_drift <= max_period_drift
    trustworthy = (
        a.period_confidence >= min_period_confidence and steady and np.isfinite(a.click_db)
    )
    scalar = a.click_db if np.isfinite(a.click_db) else -200.0
    fired = bool(trustworthy and scalar >= fire_threshold_db)

    regions = (
        [WorstRegion(time_s=a.click_time_s, severity=scalar, detector="click",
                     label="unexpected discontinuity")]
        if fired else []
    )
    note = (
        f"period {a.period_samples:.4f} samples (confidence {a.period_confidence:.2f}, "
        f"drift {a.period_drift:.1e}); worst unexpected discontinuity {scalar:.1f} dB "
        f"below peak at {a.click_time_s:.4f}s; measured below "
        f"{RESIDUAL_BAND_FRACTION:.2f}x Nyquist"
    )
    if not np.isfinite(a.click_db) or a.period_confidence < min_period_confidence:
        note += " — no stable period found; reading not trustworthy"
    elif not steady:
        note += " — pitch drifts during the render; the periodicity premise does not hold"
    return DetectorResult(
        name="click",
        scalar=scalar,
        unit="unexpected_step_db",
        fired=fired,
        time_domain="raw-output",
        measured=1 if trustworthy else 0,
        expected=1,
        worst_regions=regions,
        tolerance_class=TOLERANCE_CLASS,
        notes=note,
        maturity="experimental",
    )
