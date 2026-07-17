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

That last sentence is the whole design. The textbook way to find a click is to diff
against a reference render with the offending parameter held frozen; for a
reference-free analyzer the preceding period *is* that frozen render, and it arrives
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

### Why the residual is band-limited

Measured, not assumed: with the full band the floor sits at **-30 dB** — useless. The
cause is the interpolator's error on harmonics within a few percent of Nyquist, where
any finite-length sinc is inaccurate. Because a fractional delay is LTI, its error at
frequency f stays at f, so band-limiting the residual removes that error *exactly*
rather than merely smoothing it. Below `RESIDUAL_BAND_FRACTION` x Nyquist the floor
drops to about **-60 dB or better**. A step is broadband with a 1/f tail, so
discarding the top of the band costs the detector almost none of its signal.

The gate this feeds must therefore be stated as: *unexpected discontinuity energy
below 0.8 x Nyquist*. Naming the band is the point: a gate that passes because the
measurement could not see the failure is the worst kind, since it is silent, and an
unqualified "no clicks" claim read through a band-limited measurement is exactly that.

## What this rule cannot see

Stated up front, because a gate that passes because the measurement is blind is worse
than no gate. Each of these is measured by a test in `tests/test_click.py`:

* **A defect that repeats at exactly the oscillator period** is, by construction,
  indistinguishable from part of the waveform. A -20 dB period-locked glitch — loud
  enough to hear plainly — reads -229 dB, i.e. perfectly invisible. The same glitch one
  sample off-period reads -22 dB. This is not a tuning problem; it is what the rule
  means, and it is the price of not firing on square edges.
* **A defect locked to a MULTIPLE of the period, when no `f0_hint` is given.** Period
  doubling — alternate cycles differing, an audible f0/2 subharmonic — is a real
  oscillator defect, but unaided it is not even ill-posed, it is genuinely the same
  signal as "an oscillator whose period is 2P". Nothing in the waveform distinguishes
  them, so the unaided path reads it clean. With `f0_hint` the caller has named the
  commanded period and the ambiguity disappears, so the hint path DOES catch it
  (`analyze` fits the hint directly and never searches multiples). Pass `f0_hint` when
  gating; the unaided path is a convenience.
* **A pitch that moves FAST and SHALLOW can produce a confident false positive.** Slow
  movement (glide, slow vibrato) is refused via `period_drift`; fast movement defeats
  the guard, because it fits the period on segments and a segment averages over any
  modulation cycle it contains. Vibrato of 0.08 cents at 40 Hz reads -43.7 dB — a false
  positive on a CLEAN oscillator — while scoring a drift of 8.6e-6, below the clean
  fixtures' own worst. No threshold separates that; see `detect` for the measured sweep.
  Deep modulation IS refused (a full-semitone vibrato scores 1.4e-2, ~500x the guard),
  so the exposure is narrow — pitch jitter too small to hear as modulation but large
  enough to break the null. **Renders whose pitch genuinely moves need a reference; see
  "Which detector to reach for" below.** Note this does NOT include hard sync or harmonic
  FM, both of which are periodic and fully covered.
* **Content above 0.8 x Nyquist**, per the band qualification above.

Two things that might be expected to be blind spots, but measure otherwise: a defect
landing exactly ON a legitimate edge reads the same as one between edges (the comb
SUBTRACTS the edge rather than thresholding around it, so the edge does not mask
anything), and a hard-sync reset does not need special-casing — fitting the period
finds the master period on its own.

## Which detector to reach for

**Steady pitch — including hard sync — use this module.** Sync is steady-pitch despite
appearances: it retriggers at the master rate, but the composite repeats at the master
period, so the rule covers it unchanged. Hint the master.

**FM — use this module, but hint the MODULATOR, not the carrier.** A TZFM render is
periodic at the modulator period whenever carrier/modulator is an integer (the classic
harmonic-FM case): integrating the instantaneous frequency gives
`phase(t + 1/f_mod) - phase(t) = 2*pi*f_carrier/f_mod`, so the waveform repeats once per
modulator cycle. Hinted with the modulator it reads -75 to -88 dB on a clean render and
catches seams; hinted with the CARRIER, confidence collapses to 0.18 and the reading is
worthless. `osc_fixtures.tzfm_is_periodic` is the test for which case you are in.

**Moving pitch that never repeats — vibrato, inharmonic FM — use a REFERENCE.** Nothing
here covers it, and that is not a gap awaiting a cleverer rule (below). Diff against the
same patch rendered without the defect: `dsp.null_residual_db` reads the seam's height
with an exact 1:1 slope down to about -100 dB on precisely the carriers this module
refuses. That is the lab's frozen-reference policy, and it is the answer.

## Why there is no reference-free complement for moving pitch

A spectral-flux outlier rule (per-hop flux, flag hops exceeding median + 12 dB) looks
like the obvious complement, since it does not false-fire on moving pitch: a clean
vibrato of a full semitone reads +2.6 dB against a +12 dB rule, and clean through-zero FM
reads +1.3 dB. **But that immunity IS its blindness — the same fact, not a trade to be
tuned.** A modulated carrier's whole harmonic stack slides every hop, so every hop's flux
is high, so the median rises with any outlier. Measured: a -20 dB seam reads +30.2 dB on
a steady square and +4.4 dB on a +/-50 cent vibrato, against that vibrato's own clean
+2.7. It never fires on a moving carrier at ANY magnitude or defect class, and a local
median in place of the global one does not recover it — the seam's flux is genuinely
swamped, not mis-normalized.

On steady pitch flux would add nothing either: it reaches -45 dB where this module
reaches -44 dB in physical units, and it is blind to a block-rate zipper at every level
(stationary churn lifts the median with the outlier — a -20 dB zipper reads +2.6 dB).

`tests/test_click.py` pins all of it, so "just add a flux detector for the FM case" is
answered with a number rather than re-litigated.
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
# kernel, because a cheaper one biases the optimum toward whatever it cannot represent:
# at 32 taps the fit lands ~0.03 samples off and the floor at 55 Hz collapses to -28 dB.
# The fit must minimize the residual the detector actually reports.
_DELAY_TAPS = 256
_DELAY_BETA = 13.0

# YIN dip threshold for the no-hint period seed.
_YIN_THRESHOLD = 0.15

# Segment counts the drift guard fits the period on. Sharing no common factor is the
# point: a segmentation cannot see a modulation whose cycle count across the render is
# an exact multiple of its segment count, so two counts must alias simultaneously to
# hide one. See `measure_period_drift`.
_DRIFT_SEGMENT_COUNTS = (4, 7)

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
    # `mode="same"` already centers the kernel, so the convolution alone realizes the
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

    ONLY for an unaided seed. A caller who supplies `f0_hint` has stated the commanded
    period, and searching multiples of it is not just unnecessary but actively wrong —
    see `analyze`.

    An unaided seed can land on a period the signal only *nearly* repeats at, and the
    failure is silent: under hard sync with a near-integer slave:master ratio
    (661/220 = 3.005) the reset is almost a no-op, so YIN dips at the SLAVE period.
    Fitting there nulls nothing, and the leftover sync discontinuity reads as a -10 dB
    click — at a confidence of 0.992. A confident false positive on a clean oscillator.

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


def _segment_spread(y: np.ndarray, seed: float, count: int) -> float | None:
    """Max-minus-min of the period fitted independently on `count` equal segments,
    relative to their mean. `None` when the segments are too short to fit."""
    length = len(y) // count
    # A period ESTIMATE only needs the comb to have data to chew on (one period plus the
    # interpolation kernel); it does not need the 2-period run-up the FLOOR measurement
    # guards with. Using the smaller guard here is what lets a low-frequency render be
    # segmented at all.
    guard = int(np.ceil(seed)) + _DELAY_TAPS
    if length <= 2 * guard + 16:
        return None
    fits = [fit_period(y[i * length : (i + 1) * length], seed, guard) for i in range(count)]
    mean = float(np.mean(fits))
    return (max(fits) - min(fits)) / mean if mean > 1e-12 else 0.0


def measure_period_drift(y: np.ndarray, seed: float, guard: int) -> float:
    """How much the fitted period MOVES across the render, relative to its mean.

    The premise of the whole rule is a steady period, and `period_confidence` does not
    police it: a 1-cent glide still correlates at ~1.000 one period on, yet reads as a
    -28 dB click. Confidence answers "is this periodic at all", which is the wrong
    question — modulated pitch is locally periodic and globally not.

    Fitting on several segments and taking the max PAIRWISE spread answers the right
    one. Two halves is not enough, and the failure is silent: two halves measure NET
    drift, so any symmetric modulation — vibrato, FM — averages to nearly zero and sails
    through: vibrato of +/-0.5 cents at 6.67 Hz measures a NET drift of 2.8e-09 while
    reading -27.4 dB — a confident false positive on a clean oscillator, which is the
    worst failure this detector can have.

    Why more than one segment COUNT: a segmentation is blind to a modulation whose cycle
    count across the render is an exact multiple of the segment count, because then every
    segment averages the same value. That is precisely what happened above — 2 vibrato
    cycles over 2 halves. Using counts that share no factor pushes the blind case to
    modulation rates that need to alias against BOTH, which for a 0.3 s render lands near
    93 Hz — far above vibrato, though NOT above audio-rate FM. See the module docstring.

    Returns NaN when no segmentation fits (a render too short to measure steadiness).
    Returning 0.0 there would assert steadiness that was never measured.
    """
    spreads = [s for s in (_segment_spread(y, seed, c) for c in _DRIFT_SEGMENT_COUNTS) if s is not None]
    return max(spreads) if spreads else float("nan")


def analyze(y: np.ndarray, sr: int, f0_hint: float | None = None) -> ClickAnalysis:
    """Measure the largest period-unexpected discontinuity in `y`."""
    y = np.asarray(y, dtype=np.float64)
    # Peak of the AC content: a DC offset raises max|y| without raising any
    # discontinuity, so including it silently shrinks every reading. At +2.0 DC a -40 dB
    # seam read -50.0 and did not fire. The scale must be the waveform's, not the rail's.
    peak = float(np.max(np.abs(y - np.mean(y)))) if len(y) else 0.0
    seed, confidence = seed_period_samples(y, sr, f0_hint)
    guard = int(np.ceil(2 * seed)) + _DELAY_TAPS
    if peak <= 1e-12 or seed < 2.0 or len(y) < 2 * guard + 16:
        return ClickAnalysis(-np.inf, 0.0, seed, confidence, 0.0, np.zeros(len(y)))

    # A hint is the COMMANDED period, so it is fitted directly. Searching its multiples
    # would be worse than pointless: a period-doubling defect (alternate cycles
    # differing — an audible f0/2 subharmonic) makes the commanded period score badly, so
    # the search prefers 2P/3P/4P, which explain the defect away and null it to below
    # -230 dB — a clean pass on a -20 dB defect. A signal that only repeats at a multiple
    # of the commanded period deserves to read dirty, and only the hint can say so.
    period = fit_period(y, seed, guard) if f0_hint else _resolve_period(y, seed, guard)
    # The guard must follow the period actually used, not the seed. When the seed is the
    # slave period and the resolved period is a multiple of it, a seed-sized guard is
    # too small: the comb's first valid output moves out with the period, and reading
    # inside that leaves start-up transient in the measurement — worth ~8 dB of floor on
    # hard sync, which is a false positive's worth.
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
      Not fussiness: an oscillator gliding by as little as 1 cent reads -28 dB, and
      `period_confidence` stays at 1.000 throughout. Without this guard the detector
      reports a confident false positive on a perfectly clean oscillator. The clean
      fixtures measure below 7e-6.

    `max_period_drift=3e-5` is a measured trade with no clean answer, and the shape of
    the trade matters more than the number. The guard cannot distinguish a moving pitch
    from an interfering component: both make a segment fit a different period. So every
    real defect strong enough to disturb the fit competes with every modulation mild
    enough to be worth refusing, and tightening the guard buys fewer false positives at
    the cost of refusing real defects. Sweeping the guard over the fixtures:

        guard    false positives through    real defects refused
        1.0e-5           1/41                      6/21
        1.5e-5           1/41                      5/21
        3.0e-5           3/41                      2/21
        5.0e-5           5/41                      1/21
        1.5e-4          16/41                      0/21

    Chasing the false-positive count to zero is not on the menu at ANY setting — see the
    fast-modulation blind spot in the module docstring — so the tightest settings pay
    real coverage for almost nothing. 3e-5 is where the curve turns: it sits 4.5x above
    the worst steady-clean fixture (6.6e-6), and the three false positives that leak are
    all vibrato of 0.15 cents or less at 25 Hz or faster, reading 1-3 dB over threshold.
    Its cost is precise: a -20 dBFS block-rate zipper (drift 8.9e-5) is REFUSED rather
    than diagnosed. The gate still does not pass it, and quieter zippers — nearer the
    detector's actual sensitivity — score proportionally lower and ARE reported.

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
