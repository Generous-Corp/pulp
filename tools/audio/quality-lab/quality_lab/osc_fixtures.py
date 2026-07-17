"""Deterministic oscillator fixtures for falsifying the click detector.

Two families, both seeded and closed-form so ground truth is known by construction:

* **Clean** — waveforms that are *full of legitimate discontinuities*: square edges,
  saw wraps, hard-sync resets. A click detector that fires on these is worse than no
  detector, so these are the true negatives that carry the weight. Also the
  moving-pitch renders (`vibrato_square`, `tzfm_sine`), which are clean in exactly the
  same sense and additionally break the periodicity a comb-null rule rests on.
* **Defective** — a clean waveform plus ONE injected discontinuity of a known
  magnitude and at a known time: a wavetable band-switch seam, a block-rate parameter
  zipper, a crossfade seam, a voice-steal pop. These are the true positives.

The clean waveforms are built additively (a partial sum of the Fourier series up to
Nyquist) rather than by naive sampling. That is deliberate: a naively-sampled saw at a
non-integer period has a wrap height that varies period to period by up to ``2A/P``,
which is an aliasing artifact, not a click — but it is indistinguishable from one to
any period-synchronous analysis. Additive fixtures isolate the click question from the
alias question, and they model what a BLEP oscillator is trying to approximate.

Defect magnitudes are expressed as a fraction of full scale so a sweep over them is
directly readable as "an unexpected step this many dB below peak".
"""
from __future__ import annotations

import numpy as np


def _harmonic_count(sr: int, f0: float) -> int:
    """Partials that fit strictly below Nyquist."""
    return max(1, int(np.floor((sr / 2.0) / f0 - 1e-9)))


def bandlimited_saw(
    sr: int, f0: float, dur_s: float, amp: float = 0.7, phase: float = 0.0
) -> np.ndarray:
    """Additive sawtooth: ``-2A/pi * sum_k sin(2*pi*k*f0*t + phase)/k`` up to Nyquist.
    Rises through the period and wraps by 2A — the wrap is a legitimate discontinuity."""
    n = int(round(dur_s * sr))
    t = np.arange(n) / sr
    y = np.zeros(n, dtype=np.float64)
    for k in range(1, _harmonic_count(sr, f0) + 1):
        y += np.sin(2 * np.pi * k * f0 * t + phase) / k
    return (amp * (2.0 / np.pi)) * y


def bandlimited_square(
    sr: int, f0: float, dur_s: float, amp: float = 0.7, phase: float = 0.0
) -> np.ndarray:
    """Additive square: odd harmonics only. Two legitimate 2A edges per period — the
    canonical case the naive "big step ⇒ click" rule gets wrong."""
    n = int(round(dur_s * sr))
    t = np.arange(n) / sr
    y = np.zeros(n, dtype=np.float64)
    for k in range(1, _harmonic_count(sr, f0) + 1, 2):
        y += np.sin(2 * np.pi * k * f0 * t + phase) / k
    return (amp * (4.0 / np.pi)) * y


def _polyblep(phase: np.ndarray, dt: float) -> np.ndarray:
    """The 2-point polyBLEP correction for a unit downward step, evaluated at `phase` in
    [0,1) for a per-sample phase increment `dt`. Non-zero only within one sample of the
    wrap, where it replaces the naive jump with the sampled band-limited step."""
    b = np.zeros_like(phase)
    m = phase < dt
    t = phase[m] / dt
    b[m] = 2.0 * t - t * t - 1.0
    m = phase > 1.0 - dt
    t = (phase[m] - 1.0) / dt
    b[m] = t * t + 2.0 * t + 1.0
    return b


def polyblep_saw(sr: int, f0: float, dur_s: float, amp: float = 0.7, phase0: float = 0.0) -> np.ndarray:
    """A polyBLEP sawtooth — a REAL antialiased oscillator, the way a plugin renders one,
    as opposed to the exact additive `bandlimited_saw`.

    The distinction is the whole reason this fixture exists. `bandlimited_saw` is an exact
    Fourier sum: sampled, it is precisely sinc-periodic, so a fractional-period delay
    reproduces it and a comb self-reference nulls it to the interpolator floor. A polyBLEP
    saw only *approximates* the bandlimited waveform, and it anchors its correction to the
    wrap's sub-sample phase — which, at a non-integer period ``sr/f0``, advances every
    period. So successive periods differ slightly AT THE WRAP, and a comb self-reference
    canNOT null that: it leaves a per-edge approximation residual. That residual is a
    genuine property of a real oscillator, not a defect, and it is exactly what a
    click detector built on the additive fixtures fails to anticipate.

    The 2-point polyBLEP is genuinely clean (alias well below the residual) at a low
    ``f0/sr`` and degrades toward high ``f0/sr`` where a period spans few samples — so a
    caller asserting "the render is clean, the residual is edge smear not alias" should
    stay at a low ``f0`` and/or a high ``sr`` (e.g. 220 Hz at 96 kHz), where the alias
    floor sits tens of dB below the smear.
    """
    n = int(round(dur_s * sr))
    dt = f0 / sr
    phase = (phase0 + np.arange(n) * dt) % 1.0
    return amp * ((2.0 * phase - 1.0) - _polyblep(phase, dt))


def polyblep_square(sr: int, f0: float, dur_s: float, amp: float = 0.7, phase0: float = 0.0) -> np.ndarray:
    """A polyBLEP square — the two-edges-per-period counterpart of `polyblep_saw`.

    Same point: a real antialiased square, not an exact Fourier sum, so its two edges land
    at a fractional sub-sample phase that moves every period and its comb self-reference
    leaves a per-edge smear the additive `bandlimited_square` does not exhibit. See
    `polyblep_saw` for the alias-vs-``f0/sr`` caveat.
    """
    n = int(round(dur_s * sr))
    dt = f0 / sr
    phase = (phase0 + np.arange(n) * dt) % 1.0
    naive = np.where(phase < 0.5, 1.0, -1.0)
    return amp * (naive + _polyblep(phase, dt) - _polyblep((phase + 0.5) % 1.0, dt))


def _piecewise_linear_fourier(
    breaks: np.ndarray, slopes: np.ndarray, offsets: np.ndarray, period: float, n_harm: int
) -> np.ndarray:
    """Exact Fourier coefficients c_k, k = 0..n_harm, of a `period`-periodic function
    that equals ``slopes[m]*t + offsets[m]`` on ``[breaks[m], breaks[m+1])``.

    Closed-form per segment: ``int (a*t+b) e^{-jwt} dt`` is elementary. Exact means
    exact — no quadrature error to be mistaken later for a click.
    """
    coeffs = np.zeros(n_harm + 1, dtype=np.complex128)
    lo, hi = breaks[:-1], breaks[1:]
    coeffs[0] = np.sum(slopes * (hi**2 - lo**2) / 2.0 + offsets * (hi - lo)) / period
    for k in range(1, n_harm + 1):
        w = 2.0 * np.pi * k / period
        # F(t) = (a*t+b) e^{-jwt}/(-jw) + a e^{-jwt}/w^2  =>  integral = F(hi) - F(lo)
        def anti(t):
            e = np.exp(-1j * w * t)
            return (slopes * t + offsets) * e / (-1j * w) + slopes * e / (w * w)
        coeffs[k] = np.sum(anti(hi) - anti(lo)) / period
    return coeffs


def hard_synced_saw(
    sr: int, f_master: float, f_slave: float, dur_s: float, amp: float = 0.7
) -> np.ndarray:
    """A slave saw force-reset every master period — exactly bandlimited.

    The reset is an intentional, often violent discontinuity landing at an arbitrary
    point in the slave's own cycle: exactly the event a naive detector flags. The
    composite is periodic at the MASTER period, which is what makes it legitimate and
    what a period-synchronous rule has to discover on its own.

    Built from the exact Fourier series of the continuous-time waveform, which is
    piecewise linear over one master period (slave ramps, plus the reset). Neither
    naive sampling nor oversample-and-decimate is good enough here, and the difference
    is not academic — both quantize the reset INSTANT to their sample grid, so the
    reset lands on a different sub-sample phase each master period. That timing jitter
    is a genuine period-to-period difference: it measured as a -22 dB false reading,
    i.e. the fixture, not the detector, would have set the floor. A real BLEP sync
    oscillator places the reset at its exact fractional time, and so does this.
    """
    period = 1.0 / f_master
    # Segment boundaries over one master period: every slave wrap, plus the master reset.
    n_slave = int(np.floor(period * f_slave - 1e-12))
    breaks = np.concatenate(([0.0], (np.arange(1, n_slave + 1)) / f_slave, [period]))
    breaks = np.unique(np.clip(breaks, 0.0, period))
    # On segment m the slave ramps from its m-th wrap: s(t) = 2*(t*f_slave - m) - 1.
    m = np.floor(breaks[:-1] * f_slave + 1e-9)
    slopes = np.full(len(breaks) - 1, 2.0 * f_slave)
    offsets = -2.0 * m - 1.0

    n_harm = int(np.floor((sr / 2.0) / f_master - 1e-9))
    coeffs = _piecewise_linear_fourier(breaks, slopes, offsets, period, n_harm)

    t = np.arange(int(round(dur_s * sr))) / sr
    y = np.full(len(t), coeffs[0].real)
    for k in range(1, n_harm + 1):
        y += 2.0 * np.real(coeffs[k] * np.exp(1j * 2.0 * np.pi * k * f_master * t))
    return amp * y / (np.max(np.abs(y)) + 1e-12)


def vibrato_square(
    sr: int, f0: float, cents: float, rate_hz: float, dur_s: float, amp: float = 0.7
) -> np.ndarray:
    """A CLEAN bandlimited square whose pitch swings +/-`cents` at `rate_hz`.

    Nothing is injected: every discontinuity here is a legitimate square edge, exactly
    as in `bandlimited_square`. The pitch simply moves. That makes it a true negative
    for any click rule, and a hard one — the periodicity a comb-null rule depends on is
    gone, so it must REFUSE rather than report.

    Bandlimited at the swing's top, so the partial count never crosses Nyquist mid-render
    (which would itself be a band-switch seam — the very defect under test).
    """
    n = int(round(dur_s * sr))
    t = np.arange(n) / sr
    inst = f0 * 2.0 ** ((cents * np.sin(2 * np.pi * rate_hz * t)) / 1200.0)
    phase = 2 * np.pi * np.cumsum(inst) / sr
    y = np.zeros(n, dtype=np.float64)
    for k in range(1, _harmonic_count(sr, f0 * 2.0 ** (cents / 1200.0)) + 1, 2):
        y += np.sin(k * phase) / k
    return (amp * (4.0 / np.pi)) * y


def tzfm_sine(
    sr: int, f_carrier: float, f_mod: float, index: float, dur_s: float, amp: float = 0.7
) -> np.ndarray:
    """Through-zero FM with a sine carrier — a CLEAN render.

    Instantaneous frequency is ``f_carrier + index * f_mod * sin(2*pi*f_mod*t)``, which
    goes NEGATIVE when ``index * f_mod > f_carrier``: that is what "through zero" means,
    and it is the case a wrapping phase accumulator gets wrong.

    **This render is periodic at the MODULATOR period whenever f_carrier/f_mod is an
    integer**, which is not obvious and matters a lot. Integrating the expression above
    gives ``phase(t + 1/f_mod) - phase(t) = 2*pi*f_carrier/f_mod``, so the waveform
    repeats exactly once per modulator cycle for integer ratios — the classic harmonic-FM
    case. A period-synchronous analyzer therefore handles harmonic TZFM with no changes
    at all, provided it is told the MODULATOR rate rather than the carrier. At
    non-integer ratios the render never repeats and no such analyzer applies.

    Keep ``f_carrier + f_mod * (index + 1)`` (the Carson bandwidth edge) below Nyquist,
    or the sidebands alias and the fixture stops being a clean reference.
    """
    n = int(round(dur_s * sr))
    t = np.arange(n) / sr
    inst = f_carrier + index * f_mod * np.sin(2 * np.pi * f_mod * t)
    return amp * np.sin(2 * np.pi * np.cumsum(inst) / sr)


def tzfm_is_periodic(f_carrier: float, f_mod: float, tol: float = 1e-9) -> bool:
    """Whether `tzfm_sine` repeats at the modulator period — i.e. f_carrier/f_mod is an
    integer. The dividing line between "a period-synchronous rule applies" and "nothing
    reference-free applies"."""
    ratio = f_carrier / f_mod
    return abs(ratio - round(ratio)) < tol


def inject_step(y: np.ndarray, at_sample: int, delta: float) -> np.ndarray:
    """A crossfade seam / voice-steal discontinuity: the waveform jumps by `delta` and
    stays jumped. One unexpected step of a known height at a known time."""
    out = np.array(y, dtype=np.float64, copy=True)
    out[at_sample:] += delta
    return out


def inject_pop(y: np.ndarray, sr: int, at_sample: int, delta: float, decay_ms: float = 3.0) -> np.ndarray:
    """A voice-steal pop: a step of `delta` that decays back to zero. The step is the
    click; the decay is what makes it audible as a thud rather than a DC shift."""
    out = np.array(y, dtype=np.float64, copy=True)
    tail = len(out) - at_sample
    t = np.arange(tail) / sr
    out[at_sample:] += delta * np.exp(-t / (decay_ms / 1000.0))
    return out


def band_switch_seam(
    sr: int, f0: float, dur_s: float, at_s: float, delta: float, amp: float = 0.7
) -> np.ndarray:
    """A wavetable band-switch seam: the oscillator swaps to a neighboring band's table
    mid-cycle without a crossfade, so the output jumps by `delta` at the switch and the
    waveform continues from the wrong value.

    Modeled as a saw whose amplitude is scaled after the switch such that the step at
    the seam is exactly `delta`. Keeping the step exact (rather than deriving it from a
    harmonic-count difference) is what makes a magnitude sweep meaningful.
    """
    y = bandlimited_saw(sr, f0, dur_s, amp)
    at = int(round(at_s * sr))
    return inject_step(y, at, delta)


def zipper(y: np.ndarray, delta: float, block: int = 64) -> np.ndarray:
    """A block-rate parameter zipper: a parameter change applied as a staircase at the
    audio block boundary instead of being smoothed, so every block edge carries a step
    of `delta`.

    The staircase alternates rather than accumulating, which keeps the injected offset
    bounded at `delta` while still placing a step of exactly `delta` at every block
    boundary — so a magnitude sweep reads directly as step height. Aperiodic with
    respect to the oscillator, periodic at the BLOCK rate: the case that proves the
    rule keys on the oscillator's period, not on periodicity as such.
    """
    out = np.array(y, dtype=np.float64, copy=True)
    steps = np.arange(len(out)) // block
    return out + delta * (steps % 2).astype(np.float64)
