"""WP-4 F1 acceptance suite for the OSC-VCO fitting tool
(``planning/2026-07-16-wp4-fitting-tool-scope-bound.md`` §4).

* **Gate 1 — synthetic round-trip** (the estimator-works gate; deterministic,
  public-CI-able per D6). Render a measurement kit from a KNOWN profile through
  the OSC-VCO forward model, feed the tool only the kit plus the pinned set, and
  recover every DETERMINISTIC Fit-column parameter within the doc's tolerance:
  pitch-curve points ±1 cent; reset time ±10%; bow time-constant ±10%; shaper
  harmonic signature ±1 dB up to the 10th harmonic; AC corner ±10%.

* **Gate 3 — refusal.** The tool errors, with a pointer to the identifiability
  table, when asked to fit any pinned (drift/jitter, nominal converter constants)
  or cut (injection locking, PSU sag, …) parameter. F1 fits only the identifiable
  subspace.

* **Forward-model fidelity.** The Gate-1 renders come from a Python
  re-implementation of ``vco.hpp`` (the render bridge cannot reach the
  deterministic character parameters — §4). This test cross-checks that model
  against the REAL C++ engine (``pulp-osc-render-wav``) for the neutral shapes,
  so Gate 1 is a round-trip through a model proven to match the header. Skips
  (never silently passes) when the tool is not built.
"""
from __future__ import annotations

import math
import os
import subprocess
from pathlib import Path

import numpy as np
import pytest

from quality_lab import fit
from quality_lab import osc_forward as fw
from quality_lab.osc_forward import Shape, VcoProfile, VcoTuning, WaveshaperParams

REPO_ROOT = Path(__file__).resolve().parents[4]


# ── Known profiles for Gate 1 ─────────────────────────────────────────────────


def _profile_a() -> VcoProfile:
    p = VcoProfile()
    p.tuning = VcoTuning(tune_offset_cents=7.0, scale_error=1.0025,
                         hf_compression=0.03, hf_knee_octaves=3.0)
    p.bow = 2.5
    p.shapers[Shape.triangle] = WaveshaperParams(amount=0.7, drive=2.2, asymmetry=0.35)
    p.level_tilt_db_per_octave = 2.5
    p.core_reset_seconds = 3.0e-6
    p.ac_corner_hz = 8.0
    p.pulse_width = 0.42
    return p


def _profile_b() -> VcoProfile:
    # A second point in parameter space, incl. hf_compression = 0 (the HF knee is
    # then unidentifiable — the fit must still reproduce the pitch curve and not
    # blow up), negative tilt, a gentler bow, a stronger/asymmetric shaper.
    p = VcoProfile()
    p.tuning = VcoTuning(tune_offset_cents=-11.0, scale_error=0.9985,
                         hf_compression=0.0, hf_knee_octaves=3.0)
    p.bow = 1.2
    p.shapers[Shape.triangle] = WaveshaperParams(amount=0.9, drive=3.0, asymmetry=-0.2)
    p.level_tilt_db_per_octave = -1.5
    p.core_reset_seconds = 8.0e-6
    p.ac_corner_hz = 20.0
    p.pulse_width = 0.6
    return p


def _shaper_harmonic_db_error(true_shaper: WaveshaperParams,
                              fit_shaper: WaveshaperParams) -> float:
    """Max |ΔdB| over harmonics 1..10 of the two shapers' output on the SAME
    triangle core input — the doc's shaper-signature tolerance. An integer number
    of cycles fills the buffer so harmonics land exactly on FFT bins (no window)."""
    phase = np.linspace(0.0, 1.0, 8192, endpoint=False)
    core = np.where(phase < 0.5, 4.0 * phase - 1.0, 3.0 - 4.0 * phase)
    yt = fw.apply_waveshaper_array(true_shaper, core)
    yf = fw.apply_waveshaper_array(fit_shaper, core)
    mt = np.abs(np.fft.rfft(yt))
    mf = np.abs(np.fft.rfft(yf))
    floor = 1e-4 * mt.max()
    worst = 0.0
    for h in range(1, 11):
        if mt[h] < floor:            # skip harmonics the true shaper barely excites.
            continue
        worst = max(worst, abs(20.0 * math.log10(mt[h]) - 20.0 * math.log10(mf[h] + 1e-300)))
    return worst


@pytest.mark.parametrize("make_profile", [_profile_a, _profile_b])
def test_gate1_synthetic_round_trip(make_profile):
    true = make_profile()
    kit = fit.build_measurement_kit(true)
    fitted = fit.fit_profile(kit, VcoProfile(),
                             pinned={"drift_depth": 4.0, "jitter_depth": 0.5})
    rp = fitted.to_profile()

    # Pitch curve — recovered tuning must reproduce every note within ±1 cent.
    max_cents = max(
        abs(1200.0 * math.log2(rp.tuning.frequency_hz(v) / true.tuning.frequency_hz(v)))
        for v in np.arange(-2.0, 4.51, 0.25))
    assert max_cents <= 1.0, f"pitch curve off by {max_cents:.3f} cents"

    # Bow time-constant ±10%.
    assert fitted.get("bow").estimate == pytest.approx(true.bow, rel=0.10)

    # AC corner ±10%.
    assert fitted.get("ac_corner_hz").estimate == pytest.approx(true.ac_corner_hz, rel=0.10)

    # Reset time ±10%.
    assert fitted.get("core_reset_seconds").estimate == pytest.approx(
        true.core_reset_seconds, rel=0.10)

    # Level tilt — not given an explicit tolerance in the doc; hold it tight since
    # the curve fit is clean (a loose tilt would mean the level curve is mis-fit).
    assert fitted.get("level_tilt_db_per_octave").estimate == pytest.approx(
        true.level_tilt_db_per_octave, abs=0.15)

    # Shaper harmonic signature ±1 dB up to the 10th harmonic.
    fit_shaper = rp.shapers[Shape.triangle]
    assert _shaper_harmonic_db_error(true.shapers[Shape.triangle], fit_shaper) <= 1.0

    # Pulse width (duty) — the engine has no CV→duty map, so pulse_width is fit
    # directly; a duty read to ~1 sample of the period.
    assert fitted.get("pulse_width").estimate == pytest.approx(true.pulse_width, abs=0.01)


def test_gate1_pinned_rows_labelled_not_measured():
    """Stage-2 drift/jitter are hand-set placeholders in F1: they must appear in the
    profile labeled ``pinned`` with their hand-set value, never as ``measured``."""
    true = _profile_a()
    kit = fit.build_measurement_kit(true)
    fitted = fit.fit_profile(kit, VcoProfile(),
                             pinned={"drift_depth": 4.0, "drift_rate_hz": 0.4,
                                     "jitter_depth": 0.5})
    for name in ("drift_depth", "drift_rate_hz", "jitter_depth"):
        row = fitted.get(name)
        assert row.disposition == "pinned"
        assert row.stage == "2-deferred"
    assert fitted.get("drift_depth").estimate == 4.0
    assert fitted.get("jitter_depth").estimate == 0.5
    # And every deterministic row IS measured — the two dispositions never blur.
    for name in ("bow", "ac_corner_hz", "core_reset_seconds", "pulse_width"):
        assert fitted.get(name).disposition == "measured"


# ── Gate 3 — refusal ──────────────────────────────────────────────────────────


@pytest.mark.parametrize("name", [
    "drift_depth", "drift_rate_hz", "jitter_depth",     # stage-2 deferred / pinned.
    "drift_family", "jitter_color", "comparator_hysteresis",
    "temperature_injection_point", "thermal_tempco", "noise_slew_split",
    "reference_hz", "volts_per_octave",                 # nominal converter constants.
])
def test_gate3_refuses_pinned(name):
    with pytest.raises(fit.NotIdentifiableError) as exc:
        fit.assert_fittable(name)
    # The error points at the identifiability table (§2) — a test, not documentation.
    assert "identifiability table" in str(exc.value)


@pytest.mark.parametrize("name", [
    "injection_locking", "psu_sag", "drift_exponent", "interaction_effects",
])
def test_gate3_refuses_cut(name):
    with pytest.raises(fit.NotIdentifiableError) as exc:
        fit.assert_fittable(name)
    assert "CUT" in str(exc.value)


def test_gate3_refuses_unknown_parameter():
    # An unfamiliar name is refused too — the tool never invents a parameter the
    # header/table does not define.
    with pytest.raises(fit.NotIdentifiableError):
        fit.assert_fittable("mystery_knob")


def test_gate3_allows_fit_parameters():
    for name in ("tune_offset_cents", "scale_error", "hf_compression",
                 "hf_knee_octaves", "bow", "ac_corner_hz",
                 "level_tilt_db_per_octave", "core_reset_seconds",
                 "shaper", "pulse_width"):
        fit.assert_fittable(name)   # must not raise.


def test_gate3_pipeline_stages_call_the_gate():
    """The refusal is not just a standalone check — the stage fitters invoke it, so
    a pipeline can never fit a non-identifiable parameter by another path."""
    saved = fit.IDENTIFIABILITY["bow"]
    fit.IDENTIFIABILITY["bow"] = fit.Disposition("pin", "-", "temporarily pinned for the test")
    try:
        kit = fit.build_measurement_kit(_profile_a())
        with pytest.raises(fit.NotIdentifiableError):
            fit.fit_bow(kit, ac_corner_hz=8.0)
    finally:
        fit.IDENTIFIABILITY["bow"] = saved


# ── Forward-model fidelity vs the real C++ engine ─────────────────────────────


def _find_render_tool() -> Path | None:
    override = os.environ.get("PULP_OSC_RENDER_WAV")
    if override:
        p = Path(override)
        return p if p.exists() else None
    for build in sorted(REPO_ROOT.glob("build*")):
        cand = build / "test" / "pulp-osc-render-wav"
        if cand.exists():
            return cand
    return None


TOOL = _find_render_tool()


@pytest.mark.skipif(TOOL is None,
                    reason="pulp-osc-render-wav not built (set PULP_OSC_RENDER_WAV or build it)")
@pytest.mark.parametrize("shape_name,shape", [
    ("sine", Shape.sine), ("saw", Shape.saw),
    ("square", Shape.square), ("triangle", Shape.triangle),
])
def test_forward_model_matches_engine(tmp_path, shape_name, shape):
    """The Python OSC-VCO core must match the real engine on the neutral shapes —
    the Gate-1 forward model is only as trustworthy as this cross-check."""
    import soundfile as sf

    out = tmp_path / f"{shape_name}.wav"
    proc = subprocess.run(
        [str(TOOL), "--out", str(out), "--shape", shape_name,
         "--freq", "220", "--sr", "48000", "--dur-ms", "100", "--bits", "float"],
        capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, f"render tool failed: {proc.stderr}\n{proc.stdout}"

    cxx, _ = sf.read(str(out))
    cxx = np.asarray(cxx, dtype=np.float64)
    py = fw.render(VcoProfile(), shape, 220.0, 48000.0, len(cxx))
    # float32 WAV quantization is ~1e-7; the models agree well inside that.
    assert np.max(np.abs(py - cxx)) < 1e-6
