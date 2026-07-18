"""WP-4 F1 — offline OSC-VCO profile fitting tool (Quality-Lab lane).

Recovers the deterministic parameters of the in-tree ``VcoOscillator`` from a
rendered measurement kit. This is the tool bound by
``planning/2026-07-16-wp4-fitting-tool-scope-bound.md``:

* It is NOT a global optimizer. It is a fixed sequence of small least-squares
  fits, each run against the ONE measurement that identifies its parameters
  (§3). The drift/jitter ridge and shaper/clip degeneracy never enter a cost
  surface because the regularization is structural — a parameter the signal
  does not determine is simply not in any fit's search space.

* F1 ships stages 1, 3, 4, 5 — the DETERMINISTIC parameters (§5). Stage 2
  (frequency-noise: drift/jitter) is DEFERRED: those rows are hand-set
  placeholders, labeled ``pinned``, with no stochastic estimation here.

* The identifiability table (§2) is encoded in ``IDENTIFIABILITY`` and is the
  single bound that collapses "unbounded research project" into "estimation
  over the identifiable subspace". The tool REFUSES to fit any pinned or cut
  parameter (Gate 3).

Only ONE new estimator is authorized for F1: the single-cycle parametric fitter
for the core/shaper stages (``_lm`` + the stage fitters below). The f0(t)/Allan
demodulator is stage 2 and is intentionally absent.

Stage → engine mapping (the fit follows the header's actual math; where the
generic §5 stage model and vco.hpp's math place an effect differently, vco.hpp
wins — code is the source of truth):

1. Pitch      → tune_offset_cents, scale_error, hf_compression, hf_knee_octaves
                (pitch-vs-note: FFT fundamental across control voltages).
2. Freq-noise → DEFERRED (drift/jitter hand-set, pinned).
3. Core       → bow (saw single cycle, time domain). In this engine the "finite
                reset time" is a frequency-proportional LEVEL sag (``level_for``),
                not a literal edge, so it is fit from the level-vs-pitch curve in
                stage 5, cross-referenced here.
4. Shaper     → per-shape lumped nonlinearity from a phase-aligned single cycle,
                using the fixed core as the known input. Fit on the triangle (a
                full-range input); the square is 2-valued so its shaper is
                unidentifiable, the saw's folds into the bow — both reported, not
                falsely fit.
5. Output     → ac_corner_hz + level_tilt_db_per_octave + core_reset_seconds
                (one joint fit over the sine level-vs-pitch curve — the three own
                distinct frequency regimes), and pulse_width (square duty).
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

from . import osc_forward as fw
from .osc_forward import Shape, VcoProfile


# ── Identifiability table (§2) — drives the report and the Gate-3 refusal ──────


@dataclass(frozen=True)
class Disposition:
    """One parameter's classification. ``klass`` is (a)/(b)/(c) shorthand:
    ``fit`` = identifiable, ``pin`` = not identifiable from audio (data input),
    ``cut`` = permanently out of scope (the signal cannot determine it)."""

    klass: str          # "fit" | "pin" | "cut"
    measurement: str
    note: str = ""


IDENTIFIABILITY: dict[str, Disposition] = {
    # Stage 1 — pitch (all directly identifiable from pitch-vs-note).
    "tune_offset_cents": Disposition("fit", "pitch-vs-note curve"),
    "scale_error": Disposition("fit", "pitch-vs-note curve"),
    "hf_compression": Disposition("fit", "pitch-vs-note curve (high end)"),
    "hf_knee_octaves": Disposition("fit", "pitch-vs-note curve (high end)"),
    # Stage 3/5 — core + output level.
    "bow": Disposition("fit", "saw single-cycle time-domain fit"),
    "ac_corner_hz": Disposition("fit", "sine level-vs-pitch (low-frequency roll-off)"),
    "level_tilt_db_per_octave": Disposition("fit", "sine level-vs-pitch (log-linear tilt)"),
    "core_reset_seconds": Disposition(
        "fit", "sine level-vs-pitch (high-frequency sag)",
        "modeled as a frequency-proportional level sag in vco.hpp, not an edge"),
    # Stage 4 — per-shape shaper.
    "shaper": Disposition(
        "fit", "phase-aligned single-cycle time-domain fit (full-range shape)",
        "one lumped nonlinearity per shape; square is 2-valued (insensitive), "
        "saw folds into bow"),
    "pulse_width": Disposition("fit", "square duty cycle"),
    # Stage 2 — DEFERRED in F1 (stochastic amplitudes). Pinned/hand-set.
    "drift_depth": Disposition(
        "pin", "(stage 2 — f0(t) Allan long-tau)",
        "F1 defers stochastic estimation; hand-set placeholder"),
    "drift_rate_hz": Disposition(
        "pin", "(stage 2 — f0(t) Allan long-tau)",
        "F1 defers stochastic estimation; hand-set placeholder"),
    "jitter_depth": Disposition(
        "pin", "(stage 2 — f0(t) Allan short-tau)",
        "F1 defers stochastic estimation; hand-set placeholder; upper bound (AM leakage)"),
    # Pinned by construction — not identifiable from audio (§2, class (c)).
    "drift_family": Disposition(
        "pin", "circuit class", "single-realization exponent is statistically hopeless"),
    "jitter_color": Disposition("pin", "-", "not resolvable above the measurement floor"),
    "comparator_hysteresis": Disposition(
        "pin", "datasheet", "static effect absorbed by tuning; dynamic folds into jitter"),
    "temperature_injection_point": Disposition("pin", "circuit topology"),
    "thermal_tempco": Disposition("pin", "datasheet", "kit has no temperature-controlled captures"),
    "noise_slew_split": Disposition(
        "pin", "-", "only the quotient (= jitter RMS) is observable; the split is degenerate"),
    "reference_hz": Disposition("pin", "nominal converter constant", "not a departure-from-ideal"),
    "volts_per_octave": Disposition("pin", "nominal converter constant", "not a departure-from-ideal"),
    # Cut permanently (§5) — the signal does not determine these.
    "injection_locking": Disposition("cut", "-", "needs controlled multi-oscillator experiments"),
    "psu_sag": Disposition("cut", "-", "not in the measurement kit"),
    "drift_exponent": Disposition("cut", "-", "single capture cannot fit the process color"),
    "interaction_effects": Disposition("cut", "-"),
}


class NotIdentifiableError(ValueError):
    """Raised when the tool is asked to fit a pinned or cut parameter (Gate 3).

    The message points at the identifiability table — refusing is the whole point
    of §2: it is the bound that keeps the tool from walking a flat cost ridge.
    """


def assert_fittable(name: str) -> None:
    """Gate 3: refuse to optimize anything the identifiability table does not
    classify as ``fit``. Unknown names are refused too (nothing invents a
    parameter the header/table does not define)."""
    disp = IDENTIFIABILITY.get(name)
    if disp is None:
        raise NotIdentifiableError(
            f"'{name}' is not an OSC-VCO profile parameter — see the identifiability "
            f"table in planning/2026-07-16-wp4-fitting-tool-scope-bound.md §2")
    if disp.klass != "fit":
        kind = {"pin": "PINNED (not identifiable from audio; a data input)",
                "cut": "CUT (permanently out of scope; the signal cannot determine it)"}[disp.klass]
        raise NotIdentifiableError(
            f"refusing to fit '{name}': it is {kind}. Measurement/basis: "
            f"{disp.measurement}. {disp.note} "
            f"(identifiability table §2; F1 fits only the identifiable subspace).")


# ── Fit result containers ─────────────────────────────────────────────────────


@dataclass
class FitParam:
    """One fitted parameter: estimate, 95% CI, and which measurement produced it.
    ``disposition`` is 'measured' or 'pinned' — a pinned row is NEVER presented as
    measured (§3)."""

    name: str
    estimate: float
    ci_low: float
    ci_high: float
    measurement: str
    disposition: str = "measured"   # "measured" | "pinned"
    note: str = ""
    stage: str = ""

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "estimate": self.estimate,
            "ci": [self.ci_low, self.ci_high],
            "measurement": self.measurement,
            "disposition": self.disposition,
            "stage": self.stage,
            "note": self.note,
        }


@dataclass
class FittedProfile:
    """The tool's output — a fitted profile as an ordered list of parameter rows."""

    params: list[FitParam] = field(default_factory=list)

    def add(self, p: FitParam) -> None:
        self.params.append(p)

    def get(self, name: str) -> FitParam:
        for p in self.params:
            if p.name == name:
                return p
        raise KeyError(name)

    def to_dict(self) -> dict:
        return {"parameters": [p.to_dict() for p in self.params]}

    def to_profile(self) -> VcoProfile:
        """Reconstruct a ``VcoProfile`` from the fitted + pinned rows, so the fit
        can be re-rendered (Gate 1's recovery check, Gate 2's re-render)."""
        prof = VcoProfile()

        def val(name, default):
            try:
                return self.get(name).estimate
            except KeyError:
                return default

        prof.tuning.tune_offset_cents = val("tune_offset_cents", 0.0)
        prof.tuning.scale_error = val("scale_error", 1.0)
        prof.tuning.hf_compression = val("hf_compression", 0.0)
        prof.tuning.hf_knee_octaves = val("hf_knee_octaves", 3.0)
        prof.bow = val("bow", 0.0)
        prof.level_tilt_db_per_octave = val("level_tilt_db_per_octave", 0.0)
        prof.core_reset_seconds = val("core_reset_seconds", 0.0)
        prof.ac_corner_hz = val("ac_corner_hz", 0.0)
        prof.pulse_width = val("pulse_width", 0.5)
        prof.drift_depth = val("drift_depth", 0.0)
        prof.drift_rate_hz = val("drift_rate_hz", 0.4)
        prof.jitter_depth = val("jitter_depth", 0.0)
        for s in Shape:
            for field_name in ("amount", "drive", "asymmetry"):
                key = f"shaper_{s.name}_{field_name}"
                try:
                    setattr(prof.shapers[s], field_name, self.get(key).estimate)
                except KeyError:
                    pass
        return prof


# ── The one authorized new estimator: Levenberg–Marquardt least squares ───────


def _lm(residual, p0, lower, upper, max_iter: int = 300):
    """Bounded Levenberg–Marquardt over a small parameter vector, numeric Jacobian.

    Returns ``(p, cov, ssr)``. ``cov`` is the parameter covariance
    ``s^2 (J^T J)^-1`` at the solution (``s^2 = SSR/dof``), from which the report's
    confidence intervals come. Dependency-free (numpy only) by design — the venv
    carries numpy/soundfile/pytest and nothing else.
    """
    p = np.array(p0, dtype=np.float64)
    lower = np.array(lower, dtype=np.float64)
    upper = np.array(upper, dtype=np.float64)

    def clip(x):
        return np.minimum(np.maximum(x, lower), upper)

    def jac(p, r):
        m, n = len(r), len(p)
        J = np.zeros((m, n))
        for k in range(n):
            step = 1e-6 * max(abs(p[k]), 1e-6)
            pk = p[k]
            # Step inward when pinned against a bound.
            if pk + step > upper[k]:
                step = -step
            pp = p.copy()
            pp[k] = pk + step
            J[:, k] = (residual(pp) - r) / step
        return J

    p = clip(p)
    r = residual(p)
    ssr = float(r @ r)
    lam = 1e-3
    for _ in range(max_iter):
        J = jac(p, r)
        JtJ = J.T @ J
        Jtr = J.T @ r
        improved = False
        for _ in range(40):
            A = JtJ + lam * np.diag(np.diag(JtJ) + 1e-15)
            try:
                dp = np.linalg.solve(A, -Jtr)
            except np.linalg.LinAlgError:
                lam *= 10.0
                continue
            pn = clip(p + dp)
            rn = residual(pn)
            cn = float(rn @ rn)
            if cn < ssr:
                improved = True
                rel = (ssr - cn) / max(ssr, 1e-300)
                p, r, ssr = pn, rn, cn
                lam = max(lam * 0.4, 1e-12)
                break
            lam *= 4.0
        if not improved or rel < 1e-12:
            break

    # Covariance at the solution.
    J = jac(p, r)
    m, n = len(r), len(p)
    dof = max(m - n, 1)
    s2 = ssr / dof
    try:
        cov = s2 * np.linalg.inv(J.T @ J)
    except np.linalg.LinAlgError:
        cov = np.full((n, n), np.nan)
    return p, cov, ssr


def _ci(estimate: float, cov_diag: float) -> tuple[float, float]:
    """95% CI from a covariance diagonal (±1.96 sigma). NaN/negative → open CI."""
    if not (cov_diag >= 0.0) or not math.isfinite(cov_diag):
        return (float("nan"), float("nan"))
    half = 1.96 * math.sqrt(cov_diag)
    return (estimate - half, estimate + half)


# ── Shared measurement helper ─────────────────────────────────────────────────


def estimate_fundamental(y: np.ndarray, sample_rate: float) -> float:
    """Fundamental via QIFFT — Hann-windowed rFFT peak, parabolically interpolated
    on log-magnitude. Sub-cent accurate on a clean single tone at these durations
    (the pitch stage's ±1-cent tolerance depends on it)."""
    y = np.asarray(y, dtype=np.float64)
    y = y - np.mean(y)
    win = np.hanning(len(y))
    mag = np.abs(np.fft.rfft(y * win))
    k = int(np.argmax(mag[1:]) + 1)
    if 1 <= k < len(mag) - 1:
        a = math.log(mag[k - 1] + 1e-300)
        b = math.log(mag[k] + 1e-300)
        c = math.log(mag[k + 1] + 1e-300)
        offset = 0.5 * (a - c) / (a - 2 * b + c + 1e-300)
    else:
        offset = 0.0
    bin_hz = sample_rate / len(y)
    return (k + offset) * bin_hz


def _sine_amplitude(y: np.ndarray, settle: int) -> float:
    """Amplitude of a steady sine: sqrt(2)*RMS over the post-transient tail. A pure
    sine through the linear AC blocker stays a pure sine, so RMS is exact."""
    tail = y[settle:]
    return math.sqrt(2.0) * math.sqrt(float(np.mean(tail * tail)))


def _ac_filter(x: np.ndarray, corner_hz: float, sample_rate: float) -> np.ndarray:
    """Run the ``DcBlocker`` over ``x`` from zero state, so a candidate core/shaper
    waveform in a time-domain fit passes through the SAME already-fit AC stage the
    engine applied. Bypass (identity) when the corner is off."""
    if not (corner_hz > 0.0):
        return x
    blocker = fw.DcBlocker(fw.ac_pole(corner_hz, sample_rate))
    return np.array([blocker.process(float(v)) for v in x])


# ── Measurement kit (Gate 1 renders it from a known profile) ──────────────────


@dataclass
class MeasurementKit:
    """The rendered measurement kit the tool consumes — the ONLY input to a fit
    besides the pinned-parameter set. Mirrors the proposal's kit: pitch-vs-note,
    single cycles, a level curve, a PWM/duty capture."""

    sample_rate: float
    pitch: list[tuple[float, np.ndarray]]                 # (control_volts, wav).
    saw_cycle: tuple[float, np.ndarray]                    # (freq, wav).
    shaper_cycles: dict[Shape, tuple[float, np.ndarray]]   # shape -> (freq, wav).
    level_curve: list[tuple[float, np.ndarray]]            # (freq, wav).
    level_settle: int                                      # samples to skip (transient).
    pwm: tuple[float, np.ndarray]                          # (freq, wav).


def build_measurement_kit(profile: VcoProfile, *, sample_rate: float = 96000.0,
                          shaper_shapes: tuple[Shape, ...] = (Shape.triangle,)
                          ) -> MeasurementKit:
    """Render the full measurement kit from ``profile`` (Gate 1's target).

    Rendered at 96 kHz: the level curve reaches 40 kHz, where the finite-reset sag
    ``(1 - reset*f)`` carries enough leverage to identify ``core_reset_seconds``,
    and the low end reaches 15 Hz where the AC roll-off is a clean corner probe.
    """
    # Pitch: clean sine tones across control voltages spanning the keyboard and
    # the HF knee. The fundamental is what the tuning stage sets, independent of
    # every other stage, so a clean sine is the faithful probe.
    control_volts = np.arange(-2.0, 4.51, 0.25)
    pitch = []
    for v in control_volts:
        f = profile.tuning.frequency_hz(float(v))
        n = int(round(sample_rate * 0.5))               # 0.5 s — sub-cent QIFFT.
        pitch.append((float(v), fw.render_sine(profile, f, sample_rate, n)))

    # Core single cycles at a mid pitch, several cycles rendered so the fit skips
    # the AC transient and reads steady state. The time-domain fits model the AC
    # stage explicitly (it distorts a saw's rich low harmonics), so the pitch need
    # not be high enough for AC to be negligible.
    cyc_freq = 220.0
    cyc_n = int(round(sample_rate / cyc_freq * 8))
    saw_cycle = (cyc_freq, fw.render(profile, Shape.saw, cyc_freq, sample_rate, cyc_n))
    shaper_cycles = {
        s: (cyc_freq, fw.render(profile, s, cyc_freq, sample_rate, cyc_n))
        for s in shaper_shapes
    }

    # Level curve: sine amplitude across log-spaced pitches (AC low end, tilt
    # across the range, reset sag at the top). Skip the AC transient.
    freqs = np.geomspace(15.0, 40000.0, 56)
    lvl_n = int(round(sample_rate * 0.2))
    settle = int(round(sample_rate * 0.09))
    level_curve = [(float(f), fw.render_sine(profile, float(f), sample_rate, lvl_n))
                   for f in freqs]

    # PWM: a square at a mid pitch; duty is read from the sign fraction.
    pwm_freq = 500.0
    pwm_n = int(round(sample_rate / pwm_freq * 16))
    pwm = (pwm_freq, fw.render(profile, Shape.square, pwm_freq, sample_rate, pwm_n))

    return MeasurementKit(sample_rate, pitch, saw_cycle, shaper_cycles,
                          level_curve, settle, pwm)


# ── Stage 1 — pitch ───────────────────────────────────────────────────────────


def fit_pitch(kit: MeasurementKit, profile_ref: VcoProfile) -> list[FitParam]:
    """Fit tune_offset, scale_error, hf_compression, hf_knee from pitch-vs-note.

    ``profile_ref`` supplies only the PINNED nominal converter constants
    (reference_hz, volts_per_octave) — those are data inputs, not fit (§2)."""
    for name in ("tune_offset_cents", "scale_error", "hf_compression", "hf_knee_octaves"):
        assert_fittable(name)

    ref_hz = profile_ref.tuning.reference_hz
    vpo = profile_ref.tuning.volts_per_octave
    v = np.array([cv for cv, _ in kit.pitch])
    meas_oct = np.array([math.log2(estimate_fundamental(w, kit.sample_rate) / ref_hz)
                         for _, w in kit.pitch])

    def model_oct(p):
        scale, off_cents, hf_comp, knee = p
        o = (v / vpo) * scale + off_cents / 1200.0
        comp = np.where((hf_comp > 0.0) & (o > knee), hf_comp * (o - knee), 0.0)
        return o - comp

    def residual(p):
        return model_oct(p) - meas_oct

    p0 = [1.0, 0.0, 0.02, 3.0]
    lower = [0.9, -200.0, 0.0, 0.5]
    upper = [1.1, 200.0, 0.5, 6.0]
    p, cov, _ = _lm(residual, p0, lower, upper)
    names = ["scale_error", "tune_offset_cents", "hf_compression", "hf_knee_octaves"]
    out = []
    for i, name in enumerate(names):
        lo, hi = _ci(p[i], cov[i, i])
        out.append(FitParam(name, float(p[i]), lo, hi,
                            IDENTIFIABILITY[name].measurement,
                            note=IDENTIFIABILITY[name].note, stage="1-pitch"))
    return out


# ── Stage 3 — core: bow ───────────────────────────────────────────────────────


def _cycle_phase(freq: float, sample_rate: float, n: int) -> np.ndarray:
    return (fw.DEFAULT_START_PHASE + freq / sample_rate * np.arange(n)) % 1.0


def _steady_interior_mask(phase: np.ndarray, samples_per_cycle: float,
                          excludes: tuple[float, ...], guard: float = 0.03) -> np.ndarray:
    """Samples in a shape's smooth interior: past the first cycle (AC transient)
    and clear of the wrap (phase 0/1) and any internal discontinuity in
    ``excludes`` (e.g. the triangle apex at 0.5). The polyBLEP/BLAMP ripple lives
    exactly at those points and the ideal-core model does not — so the fit reads
    the smooth curve, not the correction."""
    mask = (phase > guard) & (phase < 1.0 - guard)
    for e in excludes:
        mask &= np.abs(phase - e) > guard
    mask[: int(round(samples_per_cycle))] = False
    return mask


def fit_bow(kit: MeasurementKit, ac_corner_hz: float) -> FitParam:
    """Fit the saw integrator-leak bow from a single-cycle time-domain capture.

    The saw core value at phase p is 2p-1, so the bow output is a pure function of
    phase. The candidate is passed through the already-fit AC stage (the DC blocker
    materially tilts a saw's rich low harmonics), then a scalar gain g and DC term
    dc absorb the level stage and any residual offset."""
    assert_fittable("bow")
    freq, y = kit.saw_cycle
    n = len(y)
    p = _cycle_phase(freq, kit.sample_rate, n)
    y = np.asarray(y, dtype=np.float64)
    mask = _steady_interior_mask(p, kit.sample_rate / freq, excludes=())

    def bow_shape(k):
        # 2*(1-e^{-k p})/(1-e^{-k}) - 1, guarding k -> 0.
        if abs(k) < 1e-9:
            return 2.0 * p - 1.0
        return 2.0 * (1.0 - np.exp(-k * p)) / (1.0 - math.exp(-k)) - 1.0

    def residual(params):
        k, g, dc = params
        model = g * _ac_filter(bow_shape(k), ac_corner_hz, kit.sample_rate) + dc
        return (model - y)[mask]

    amp = 0.5 * (y.max() - y.min())
    pfit, cov, _ = _lm(residual, [1.0, amp, 0.0], [1e-3, 1e-6, -2.0], [20.0, 10.0, 2.0])
    lo, hi = _ci(pfit[0], cov[0, 0])
    return FitParam("bow", float(pfit[0]), lo, hi, IDENTIFIABILITY["bow"].measurement,
                    stage="3-core")


# ── Stage 4 — per-shape shaper ────────────────────────────────────────────────


def _core_values(shape: Shape, phase: np.ndarray, pulse_width: float) -> np.ndarray:
    """Ideal (pre-BLEP) core value at each phase — the known shaper input."""
    if shape == Shape.triangle:
        return np.where(phase < 0.5, 4.0 * phase - 1.0, 3.0 - 4.0 * phase)
    if shape == Shape.saw:
        return 2.0 * phase - 1.0
    if shape == Shape.sine:
        return np.sin(2.0 * math.pi * phase)
    return np.where(phase < pulse_width, 1.0, -1.0)   # square (2-valued).


def fit_shaper(kit: MeasurementKit, shape: Shape, pulse_width: float,
               ac_corner_hz: float) -> list[FitParam]:
    """Fit a shape's lumped nonlinearity from a phase-aligned single cycle, using
    the fixed core as the known input (§3 sequencing: core before shaper) and
    passing the candidate through the already-fit AC stage.

    Reports honestly where a shape cannot identify a shaper: the square is
    2-valued (any memoryless map is absorbed into two levels), so its shaper is
    left neutral and labeled insensitive rather than fit to noise."""
    assert_fittable("shaper")
    freq, y = kit.shaper_cycles[shape]
    n = len(y)
    phase = _cycle_phase(freq, kit.sample_rate, n)
    core = _core_values(shape, phase, pulse_width)
    y = np.asarray(y, dtype=np.float64)
    # Exclude the shape's internal discontinuities (triangle apex at 0.5) plus the
    # wrap and the AC transient.
    excludes = (0.5,) if shape == Shape.triangle else ()
    mask = _steady_interior_mask(phase, kit.sample_rate / freq, excludes)

    distinct = np.unique(np.round(core, 6))
    if len(distinct) < 8:
        # Not enough distinct input values to identify a memoryless curve.
        return [FitParam(f"shaper_{shape.name}_{f}", 0.0 if f != "drive" else 1.0,
                         float("nan"), float("nan"),
                         IDENTIFIABILITY["shaper"].measurement, disposition="pinned",
                         note="insensitive — input is not full-range; pin recommended",
                         stage="4-shaper")
                for f in ("amount", "drive", "asymmetry")]

    def model(params):
        amount, drive, asym, g, dc = params
        w = fw.WaveshaperParams(amount=amount, drive=drive, asymmetry=asym)
        shaped = fw.apply_waveshaper_array(w, core)
        return g * _ac_filter(shaped, ac_corner_hz, kit.sample_rate) + dc

    def residual(params):
        return (model(params) - y)[mask]

    amp = 0.5 * (y.max() - y.min())
    p0 = [0.5, 1.5, 0.0, amp, 0.0]
    lower = [0.0, 0.05, -3.0, 1e-6, -2.0]
    upper = [1.0, 8.0, 3.0, 10.0, 2.0]
    pfit, cov, _ = _lm(residual, p0, lower, upper)
    out = []
    for i, fname in enumerate(("amount", "drive", "asymmetry")):
        lo, hi = _ci(pfit[i], cov[i, i])
        out.append(FitParam(f"shaper_{shape.name}_{fname}", float(pfit[i]), lo, hi,
                            IDENTIFIABILITY["shaper"].measurement, stage="4-shaper"))
    return out


# ── Stage 5 — output: level curve (AC + tilt + reset) and PWM duty ─────────────


def fit_level_curve(kit: MeasurementKit) -> list[FitParam]:
    """Joint fit of ac_corner, level_tilt, core_reset over the sine level curve.

    The three own distinct frequency regimes — AC rolls off the low end (+6 dB/oct
    below its corner), the tilt is log-linear across the range, the finite-reset
    sag ``(1 - reset*f)`` bites at the top — so a single joint least-squares over
    dB-vs-log-f separates them without a global optimizer (§3)."""
    for name in ("ac_corner_hz", "level_tilt_db_per_octave", "core_reset_seconds"):
        assert_fittable(name)
    sr = kit.sample_rate
    freqs = np.array([f for f, _ in kit.level_curve])
    amps = np.array([_sine_amplitude(w, kit.level_settle) for _, w in kit.level_curve])
    meas_db = 20.0 * np.log10(np.maximum(amps, 1e-12))

    def model_db(p):
        corner, tilt, reset = p
        ac = np.array([fw.ac_magnitude(corner, sr, float(f)) for f in freqs])
        ac_db = 20.0 * np.log10(np.maximum(ac, 1e-12))
        tilt_db = -tilt * np.log2(freqs / fw.LEVEL_REFERENCE_HZ)
        sag = np.maximum(1.0 - reset * freqs, 1e-6)
        return ac_db + tilt_db + 20.0 * np.log10(sag)

    def residual(p):
        return model_db(p) - meas_db

    p0 = [10.0, 0.0, 0.0]
    lower = [0.5, -12.0, 0.0]
    upper = [200.0, 12.0, 5e-5]
    pfit, cov, _ = _lm(residual, p0, lower, upper)
    names = ["ac_corner_hz", "level_tilt_db_per_octave", "core_reset_seconds"]
    # ac_corner is the core stage's AC row (§2/§3); tilt + reset are the output
    # stage. All three come from this one joint fit — they own distinct regimes.
    stages = {"ac_corner_hz": "3-core"}
    out = []
    for i, name in enumerate(names):
        lo, hi = _ci(pfit[i], cov[i, i])
        out.append(FitParam(name, float(pfit[i]), lo, hi,
                            IDENTIFIABILITY[name].measurement,
                            note=IDENTIFIABILITY[name].note,
                            stage=stages.get(name, "5-output")))
    return out


def fit_pulse_width(kit: MeasurementKit) -> FitParam:
    """Duty cycle → pulse_width. After AC removes the square's DC, the high plateau
    sits above zero and the low below, so the sign fraction is the duty directly.

    (The generic §5 "PWM CV→duty mapping" and "comparator slew" are NOT modeled by
    this engine — vco.hpp exposes only ``pulse_width`` — so only it is fit; the CV
    map is not invented.)"""
    assert_fittable("pulse_width")
    _, y = kit.pwm
    y = np.asarray(y, dtype=np.float64)
    y = y - np.mean(y)                       # belt-and-suspenders DC removal.
    duty = float(np.mean(y > 0.0))
    return FitParam("pulse_width", duty, float("nan"), float("nan"),
                    IDENTIFIABILITY["pulse_width"].measurement, stage="5-output")


# ── Orchestrator — the fixed sequence, plus the pinned/deferred rows ──────────


def fit_profile(kit: MeasurementKit, profile_ref: VcoProfile, *,
                pinned: dict[str, float] | None = None,
                shaper_shapes: tuple[Shape, ...] = (Shape.triangle,)) -> FittedProfile:
    """Run the F1 fixed sequence (stages 1, 3, 4, 5) and assemble the fitted
    profile. ``pinned`` supplies the hand-set drift/jitter (stage-2) values and any
    other pinned data inputs; they are recorded as ``disposition='pinned'`` — never
    presented as measured (§3)."""
    pinned = dict(pinned or {})
    result = FittedProfile()

    # Stage 1 — pitch.
    for fp in fit_pitch(kit, profile_ref):
        result.add(fp)

    # Stage 3/5 — the level curve is fit first because it identifies the AC corner
    # cleanly (the sine roll-off is independent of bow/shaper), and the corner is
    # then an INPUT to the time-domain core/shaper fits, which model the AC stage.
    # This is the §3 "each fit against the measurement that identifies it" order;
    # the AC row is labeled core, tilt/reset output.
    level_params = fit_level_curve(kit)
    ac_corner = next(p.estimate for p in level_params if p.name == "ac_corner_hz")

    # Stage 3 — core (bow), through the now-known AC. The finite reset is a level
    # term in this engine, fit above in the level curve.
    result.add(fit_bow(kit, ac_corner))

    # Stage 4 — shaper (per requested full-range shape), through core + AC.
    for s in shaper_shapes:
        for fp in fit_shaper(kit, s, profile_ref.pulse_width, ac_corner):
            result.add(fp)

    # Stage 5 — output.
    for fp in level_params:
        result.add(fp)
    result.add(fit_pulse_width(kit))

    # Stage 2 — DEFERRED: drift/jitter are hand-set, labeled pinned.
    for name, default in (("drift_depth", 0.0), ("drift_rate_hz", 0.4),
                          ("jitter_depth", 0.0)):
        disp = IDENTIFIABILITY[name]
        result.add(FitParam(name, float(pinned.get(name, default)),
                            float("nan"), float("nan"), disp.measurement,
                            disposition="pinned", note=disp.note, stage="2-deferred"))
    return result
