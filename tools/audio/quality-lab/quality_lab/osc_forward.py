"""Faithful Python port of the OSC-VCO forward model (``core/signal/include/pulp/signal/osc/vco.hpp``).

WP-4 F1 fits parameters of the in-tree ``VcoOscillator``. The render bridge
(``pulp-osc-render-wav``) only exposes shape/freq/drift/jitter, so it cannot reach
the deterministic character parameters the fitting tool recovers (bow, waveshaper,
level tilt, reset, AC corner, tuning, pulse width). Per
``planning/2026-07-16-wp4-fitting-tool-scope-bound.md`` §4, when the render tool
cannot hit the parameters, Gate 1 renders from a Python re-implementation of the
forward model that MATCHES the header. This module is that re-implementation.

It ports, line-for-line with the C++ where it matters:

* ``VcoTuning`` — the exponential pitch converter (``frequency_hz``).
* ``VaOscillator`` — the bandlimited core: ``PhaseAccumulator`` + polyBLEP/BLAMP
  (``phase.hpp`` + ``blep.hpp`` + ``va.hpp``). This is the only non-trivial part;
  it reproduces the wrap/threshold discontinuity correction so a rendered saw or
  triangle carries the same two-sample BLEP ripple the engine produces.
* ``VcoOscillator``'s deterministic character stages — bow, per-shape waveshaper,
  level-vs-pitch (tilt + finite reset), AC coupling (``DcBlocker``).

F1 renders the measurement kit with drift and jitter OFF (they are the stage-2
stochastic tail, deferred), so ``pitch_noise_factor()`` is exactly 1.0 and the
seeded RNG is intentionally NOT ported here — a neutral-noise VCO is bit-for-bit
the deterministic path, which is all F1 measures.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field, replace
from enum import IntEnum

import numpy as np

# ── Shapes (mirror VaShape's enum ORDER — it indexes the shaper array) ────────


class Shape(IntEnum):
    sine = 0
    saw = 1
    square = 2
    triangle = 3


SHAPE_NAMES = {"sine": Shape.sine, "saw": Shape.saw,
               "square": Shape.square, "triangle": Shape.triangle}


# ── Parameter containers (mirror the C++ structs) ─────────────────────────────


@dataclass
class VcoTuning:
    """The analog exponential pitch converter — ``VcoTuning`` in vco.hpp."""

    reference_hz: float = 261.625565           # C4.
    volts_per_octave: float = 1.0
    tune_offset_cents: float = 0.0
    scale_error: float = 1.0
    hf_compression: float = 0.0
    hf_knee_octaves: float = 3.0

    def frequency_hz(self, control_volts: float) -> float:
        octaves = (control_volts / self.volts_per_octave) * self.scale_error \
            + self.tune_offset_cents / 1200.0
        if self.hf_compression > 0.0 and octaves > self.hf_knee_octaves:
            octaves -= self.hf_compression * (octaves - self.hf_knee_octaves)
        return self.reference_hz * (2.0 ** octaves)


@dataclass
class WaveshaperParams:
    """A per-shape lumped memoryless nonlinearity — ``WaveshaperParams``."""

    amount: float = 0.0
    drive: float = 1.0
    asymmetry: float = 0.0


@dataclass
class VcoProfile:
    """Everything a fitted OSC-VCO profile holds (deterministic path).

    Field defaults are the engine's neutral defaults, so a default profile is
    bit-for-bit a ``VaOscillator``. ``shapers`` is indexed by ``Shape``.
    """

    tuning: VcoTuning = field(default_factory=VcoTuning)
    bow: float = 0.0
    shapers: dict[Shape, WaveshaperParams] = field(
        default_factory=lambda: {s: WaveshaperParams() for s in Shape})
    level_tilt_db_per_octave: float = 0.0
    core_reset_seconds: float = 0.0
    ac_corner_hz: float = 0.0
    pulse_width: float = 0.5
    # Stochastic tail — hand-set in F1 (stage 2 deferred); never fit here.
    drift_depth: float = 0.0
    drift_rate_hz: float = 0.4
    jitter_depth: float = 0.0

    def copy(self) -> "VcoProfile":
        return replace(self, tuning=replace(self.tuning),
                       shapers={s: replace(p) for s, p in self.shapers.items()})


LEVEL_REFERENCE_HZ = 1000.0  # VcoOscillator::kLevelReferenceHz.


# ── Deterministic character stages (memoryless / scalar) ──────────────────────


def apply_bow(bow: float, v: float, shape: Shape) -> float:
    """``VcoOscillator::apply_bow`` — the saw-only integrator-leak reshape."""
    if bow == 0.0 or shape != Shape.saw:
        return v
    p = 0.5 * (v + 1.0)
    p = 0.0 if p < 0.0 else (1.0 if p > 1.0 else p)
    charged = (1.0 - math.exp(-bow * p)) / (1.0 - math.exp(-bow))
    return 2.0 * charged - 1.0


def apply_waveshaper(w: WaveshaperParams, v: float) -> float:
    """``VcoOscillator::apply_waveshaper`` — endpoint-anchored, DC-free tanh."""
    amount = min(max(w.amount, 0.0), 1.0)
    if amount == 0.0:
        return v
    lo = math.tanh(-w.drive + w.asymmetry)
    hi = math.tanh(w.drive + w.asymmetry)
    mid = 0.5 * (hi + lo)
    half = 0.5 * (hi - lo)
    shaped = (math.tanh(w.drive * v + w.asymmetry) - mid) / half if half != 0.0 else v
    return (1.0 - amount) * v + amount * shaped


def apply_waveshaper_array(w: WaveshaperParams, v: np.ndarray) -> np.ndarray:
    """Vectorized ``apply_waveshaper`` — same math over an array of core values."""
    amount = min(max(w.amount, 0.0), 1.0)
    if amount == 0.0:
        return v
    lo = math.tanh(-w.drive + w.asymmetry)
    hi = math.tanh(w.drive + w.asymmetry)
    mid = 0.5 * (hi + lo)
    half = 0.5 * (hi - lo)
    if half == 0.0:
        return v
    shaped = (np.tanh(w.drive * v + w.asymmetry) - mid) / half
    return (1.0 - amount) * v + amount * shaped


def level_for(profile: VcoProfile, frequency: float, sample_rate: float) -> float:
    """``VcoOscillator::level_for`` — level-vs-pitch tilt + finite-reset sag."""
    tilt = profile.level_tilt_db_per_octave
    reset = profile.core_reset_seconds
    if tilt == 0.0 and reset == 0.0:
        return 1.0
    if not (frequency > 0.0):
        return 1.0
    gain = 1.0
    if tilt != 0.0:
        octaves = math.log2(frequency / LEVEL_REFERENCE_HZ)
        gain *= 10.0 ** (-tilt * octaves / 20.0)
    if reset != 0.0:
        eaten = reset * frequency
        gain *= (1.0 - eaten) if eaten < 1.0 else 0.0
    return gain


def ac_pole(corner_hz: float, sample_rate: float) -> float:
    """``VcoOscillator::update_ac_pole`` — realized DC-blocker pole (1.0 = bypass)."""
    if corner_hz > 0.0:
        return math.exp(-2.0 * math.pi * corner_hz / sample_rate)
    return 1.0


def ac_magnitude(corner_hz: float, sample_rate: float, frequency: float) -> float:
    """|H(f)| of the ``DcBlocker`` high-pass ``H(z)=(1-z^-1)/(1-p z^-1)``.

    Exact steady-state gain the engine applies to a sine at ``frequency`` — a pure
    sine through the linear DC blocker stays a pure sine, scaled by this. Used by
    the level-curve fit; the time-domain path runs the recurrence instead.
    """
    if not (corner_hz > 0.0):
        return 1.0
    p = ac_pole(corner_hz, sample_rate)
    w = 2.0 * math.pi * frequency / sample_rate
    num = 2.0 - 2.0 * math.cos(w)                      # |1 - e^{-jw}|^2.
    den = 1.0 - 2.0 * p * math.cos(w) + p * p          # |1 - p e^{-jw}|^2.
    return math.sqrt(num / den)


class DcBlocker:
    """Scalar port of ``pulp::signal::DcBlocker`` — ``y = x - x_1 + p*y_1``."""

    def __init__(self, pole: float) -> None:
        self.pole = pole
        self.last_in = 0.0
        self.last_out = 0.0

    def process(self, x: float) -> float:
        y = x - self.last_in + self.pole * self.last_out
        if abs(y) < 1e-15:   # snap_to_zero (denormal flush); no-op at our levels.
            y = 0.0
        self.last_in = x
        self.last_out = y
        return y


# ── Bandlimited core: PhaseAccumulator + polyBLEP/BLAMP (va.hpp) ───────────────


def _blamp_residual(x: float) -> float:
    if x < -1.0 or x >= 1.0:
        return 0.0
    if x < 0.0:
        return x * x * x / 6.0 + 0.5 * x * x + 0.5 * x + 1.0 / 6.0
    return 0.5 * x * x - x * x * x / 6.0 - 0.5 * x + 1.0 / 6.0


def _poly_blep(d: float, height: float) -> tuple[float, float]:
    if not (0.0 <= d <= 1.0):
        return 0.0, 0.0
    before = 0.5 * d * d - d + 0.5
    x = 1.0 - d
    after = x - 0.5 * x * x - 0.5
    return height * before, height * after


def _poly_blamp(d: float, slope_change: float) -> tuple[float, float]:
    if not (0.0 <= d <= 1.0):
        return 0.0, 0.0
    return slope_change * _blamp_residual(-d), slope_change * _blamp_residual(1.0 - d)


def _threshold_crossings(phase_before: float, increment: float, threshold: float,
                         capacity: int = 8) -> list[float]:
    """Port of ``threshold_crossings`` — sub-sample fractions crossing a level."""
    if not math.isfinite(increment) or increment == 0.0:
        return []
    q0 = phase_before - threshold
    first = math.floor(q0)
    last = math.floor(q0 + increment)
    if not (math.isfinite(first) and math.isfinite(last)):
        return []
    forward = increment > 0.0
    magnitude = (last - first) if forward else (first - last)
    if not (magnitude >= 1.0):
        return []
    count = capacity if magnitude > capacity else int(magnitude)
    out = []
    for k in range(count):
        level = first + 1.0 + k if forward else first - k
        out.append((level - q0) / increment)
    return out


class VaCore:
    """Virtual-analog core — ``VaOscillator`` (no sync path; F1 renders never sync)."""

    _TWO_PI = 2.0 * math.pi

    def __init__(self, shape: Shape = Shape.saw, pulse_width: float = 0.5) -> None:
        self.shape = shape
        self.pulse_width = min(max(pulse_width, 0.0), 1.0)
        self._phase = 0.0
        self._carry = 0.0

    def reset(self, phase: float = 0.0) -> None:
        w = phase - math.floor(phase)
        self._phase = w if 0.0 <= w < 1.0 else 0.0
        self._carry = 0.0

    # value() — the trivial shape, matching the header's limit-at-1 invariants.
    def _value(self, p: float) -> float:
        s = self.shape
        if s == Shape.sine:
            return 0.0 if p >= 1.0 else math.sin(self._TWO_PI * p)
        if s == Shape.saw:
            return 2.0 * p - 1.0
        if s == Shape.square:
            return 1.0 if (self.pulse_width >= 1.0 or p < self.pulse_width) else -1.0
        # triangle.
        return (4.0 * p - 1.0) if p < 0.5 else (3.0 - 4.0 * p)

    def _slope_per_cycle(self, p: float) -> float:
        s = self.shape
        if s == Shape.sine:
            return self._TWO_PI * math.cos(self._TWO_PI * p)
        if s == Shape.saw:
            return 2.0
        if s == Shape.triangle:
            return 4.0 if p < 0.5 else -4.0
        return 0.0  # square.

    def _internal_threshold(self):
        """(phase, value_below, value_above, slope_below, slope_above) or None."""
        s = self.shape
        if s == Shape.square:
            if not (0.0 < self.pulse_width < 1.0):
                return None
            return (self.pulse_width, 1.0, -1.0, 0.0, 0.0)
        if s == Shape.triangle:
            return (0.5, 1.0, 1.0, 4.0, -4.0)
        return None

    def _advance_phase(self, increment: float) -> list[tuple[float, float, float]]:
        """Port of ``PhaseAccumulator::scan`` — returns (frac, before, after)."""
        if not math.isfinite(increment):
            self._phase = 0.0
            return []
        p0 = self._phase
        raw = p0 + increment
        n = math.floor(raw)
        wrapped = raw - n
        if not (0.0 <= wrapped < 1.0):
            wrapped = 0.0
        magnitude = abs(n)
        if magnitude > 8:
            events_n = 8
        elif magnitude >= 1.0:
            events_n = int(magnitude)
        else:
            events_n = 0
        forward = n > 0.0
        events = []
        for k in range(events_n):
            level = (k + 1) if forward else -k
            t = (level - p0) / increment
            events.append((t, 1.0 if forward else 0.0, 0.0 if forward else 1.0))
        self._phase = wrapped
        return events

    def _add_break(self, frac: float, value_step: float, slope_step: float,
                   out: float) -> float:
        if value_step != 0.0:
            before, after = _poly_blep(frac, value_step)
            out += before
            self._carry += after
        if slope_step != 0.0:
            before, after = _poly_blamp(frac, slope_step)
            out += before
            self._carry += after
        return out

    def _scan_threshold(self, start_phase: float, delta: float, rate: float,
                        t0: float, span: float, out: float) -> float:
        t = self._internal_threshold()
        if t is None:
            return out
        phase_t, value_below, value_above, slope_below, slope_above = t
        fracs = _threshold_crossings(start_phase, delta, phase_t)
        if not fracs:
            return out
        forward = delta > 0.0
        value_step = (value_above - value_below) if forward else (value_below - value_above)
        slope_step = ((slope_above - slope_below) if forward
                      else (slope_below - slope_above)) * rate
        for f in fracs:
            out = self._add_break(t0 + f * span, value_step, slope_step, out)
        return out

    def next(self, increment: float) -> float:
        entry_phase = self._phase
        out = self._value(entry_phase) + self._carry
        self._carry = 0.0
        events = self._advance_phase(increment)
        for frac, before, after in events:
            value_step = self._value(after) - self._value(before)
            slope_step = (self._slope_per_cycle(after) - self._slope_per_cycle(before)) * increment
            out = self._add_break(frac, value_step, slope_step, out)
        out = self._scan_threshold(entry_phase, increment, increment, 0.0, 1.0, out)
        return out


# ── Full VcoOscillator render ─────────────────────────────────────────────────

DEFAULT_START_PHASE = 0.13   # matches VcoSourceProcessor::kStartPhase.


def render(profile: VcoProfile, shape: Shape, frequency_hz: float,
           sample_rate: float, num_samples: int,
           start_phase: float = DEFAULT_START_PHASE) -> np.ndarray:
    """Render ``num_samples`` of the OSC-VCO deterministic path, faithfully.

    Drift/jitter are off (F1), so the per-sample increment is constant and
    ``pitch_noise_factor()`` is exactly 1.0.
    """
    core = VaCore(shape, profile.pulse_width)
    core.reset(start_phase)
    increment = frequency_hz / sample_rate
    shaper = profile.shapers[shape]
    reset = profile.core_reset_seconds
    tilt = profile.level_tilt_db_per_octave
    ac_on = profile.ac_corner_hz > 0.0
    blocker = DcBlocker(ac_pole(profile.ac_corner_hz, sample_rate)) if ac_on else None
    # level_for depends only on |increment|*sr, constant here — compute once.
    gain = level_for(profile, abs(increment) * sample_rate, sample_rate)

    out = np.empty(num_samples, dtype=np.float64)
    for i in range(num_samples):
        v = core.next(increment)
        v = apply_bow(profile.bow, v, shape)
        v = apply_waveshaper(shaper, v)
        v *= gain
        out[i] = blocker.process(v) if ac_on else v
    return out


def render_sine(profile: VcoProfile, frequency_hz: float, sample_rate: float,
                num_samples: int) -> np.ndarray:
    """Fast vectorized sine render (pitch/level/AC only) — the sine core is a pure
    ``sin`` and needs no per-sample BLEP loop. Used for the pitch + level kits."""
    n = np.arange(num_samples)
    phase = (DEFAULT_START_PHASE + frequency_hz / sample_rate * n)
    v = np.sin(2.0 * math.pi * phase)
    v = apply_waveshaper_array(profile.shapers[Shape.sine], v)
    v *= level_for(profile, frequency_hz, sample_rate)
    if profile.ac_corner_hz > 0.0:
        blocker = DcBlocker(ac_pole(profile.ac_corner_hz, sample_rate))
        out = np.empty(num_samples, dtype=np.float64)
        for i in range(num_samples):
            out[i] = blocker.process(float(v[i]))
        return out
    return v
