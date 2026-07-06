"""Do-no-harm + anti-masking self-test for the windowed-sinc resampler (spec §3b / §4.1).

The varispeed alignment class (`--align varispeed:R`) is a resample-type speed change where pitch
follows: a candidate is *exactly* a resample of the reference by `R`, so resampling it back by `1/R`
returns it to the reference time base and the whole existing compare pipeline — including the
phase-sensitive sample-domain null-residual — validates it unchanged. That only holds if the
resampler is itself transparent to the compare axes. This proves it:

- **Do no harm** — resampling a reference render by `R` and back reads `no_material_change` on
  tonal-balance / added-hf / noise-roughness. The resample round trip adds no fizz, roughness, or
  imaging that an axis would flag.
- **Anti-masking** — a varispeed candidate that ALSO has real added HF fizz still flags `added-hf`
  after the resample-back. A transparent resampler must not smear a genuine defect away.

The one physical limit (not a resampler defect): a speed-up varispeed (`R < 1`) is a down-sample
that bandlimits to the new, lower Nyquist, so source energy above it is unrecoverable. On a
transient render whose (synthetic differentiator) hi-hats pile ~20% of their energy near Nyquist,
that reads as a tonal-balance dulling — but it matches an ideal brickwall bandlimit to ~1%, so the
resampler adds no harm of its own. The test isolates that case with a brickwall oracle.
"""
from __future__ import annotations

import numpy as np
import pytest

from quality_lab import compare, generate
from quality_lab.dsp import resample_by_ratio, resample_to_length

SR = 48000
RATIOS = (0.8, 1.25, 1.5)
AXES = ("tonal-balance", "added-hf", "noise-roughness")


def _drum() -> np.ndarray:
    y, _ = generate.render_drum_break(sr=SR)
    return y


def _tonal() -> np.ndarray:
    y, _ = generate.render_tonal(sr=SR, dur_s=2.0)
    return y


def _materiality(reference: np.ndarray, candidate: np.ndarray, profile: str) -> dict:
    """Run one compare axis against a golden reference and return its materiality block."""
    report = compare.compare_arrays(reference, candidate, SR, profile=profile,
                                    reference_role="golden")
    env = report["measurements"][0]
    assert env["status"] == "measured", f"{profile}: axis not measured ({env.get('reason')})"
    return {"verdict": report["verdict"], **env["materiality"]}


def _roundtrip(y: np.ndarray, ratio: float) -> np.ndarray:
    """Synthesize a varispeed candidate at `ratio`, then resample it back to the source length."""
    candidate = resample_by_ratio(y, ratio)
    return resample_to_length(candidate, y.size)


def _brickwall(y: np.ndarray, cutoff_frac: float) -> np.ndarray:
    """Ideal FFT brickwall lowpass at `cutoff_frac` of Nyquist — the physical oracle for the
    unavoidable bandlimit of a speed-up (down-sample) varispeed round trip."""
    spec = np.fft.rfft(y)
    freqs = np.fft.rfftfreq(y.size, 1.0 / SR)
    spec[freqs >= cutoff_frac * SR / 2.0] = 0.0
    return np.fft.irfft(spec, y.size)


# ── Do no harm ────────────────────────────────────────────────────────────

@pytest.mark.parametrize("name,signal", [("drum", _drum), ("tonal", _tonal)])
@pytest.mark.parametrize("ratio", [1.25, 1.5])
@pytest.mark.parametrize("profile", AXES)
def test_lossless_roundtrip_is_transparent(name, signal, ratio, profile):
    """An up-sample-first round trip (ratio >= 1) is losslessly reversible, so EVERY axis must read
    no_material_change against the raw reference — the core resampler-transparency proof."""
    ref = signal()
    mat = _materiality(ref, _roundtrip(ref, ratio), profile)
    assert not mat["exceeds"], (
        f"{name} R={ratio} {profile}: round trip flagged a change "
        f"(delta={mat['delta']}, threshold={mat['threshold']})")
    assert mat["verdict"] == "no_material_change_detected"


@pytest.mark.parametrize("profile", AXES)
def test_tonal_speedup_roundtrip_is_transparent(profile):
    """The sustained tonal render has no near-Nyquist energy, so even a speed-up (R=0.8, down-sample)
    round trip is transparent on every axis against the raw reference."""
    ref = _tonal()
    mat = _materiality(ref, _roundtrip(ref, 0.8), profile)
    assert not mat["exceeds"], f"tonal R=0.8 {profile}: delta={mat['delta']}"


@pytest.mark.parametrize("profile", ["added-hf", "noise-roughness"])
def test_drum_speedup_adds_no_artifacts(profile):
    """A speed-up (R=0.8) round trip of the transient render only REMOVES the top octave (Nyquist
    limit); it must not ADD HF fizz or roughness — added-hf/noise-roughness stay sub-threshold."""
    ref = _drum()
    mat = _materiality(ref, _roundtrip(ref, 0.8), profile)
    assert not mat["exceeds"], f"drum R=0.8 {profile}: round trip added an artifact (delta={mat['delta']})"


def test_drum_speedup_dulling_is_the_nyquist_limit_not_resampler_harm():
    """R=0.8 down-samples the transient render to a 0.8*Nyquist band, dulling its (near-Nyquist-heavy)
    spectrum. That dulling is physical: against an IDEAL brickwall at the same band edge the restored
    signal's tonal-balance delta is tiny — the resampler matches ideal bandlimiting, adding no harm."""
    ref = _drum()
    restored = _roundtrip(ref, 0.8)
    # Against the raw reference the round trip reads a large (physical) dulling …
    raw = _materiality(ref, restored, "tonal-balance")
    assert raw["exceeds"] and raw["delta"] < 0.0
    # … but against the ideal brickwall oracle the resampler-attributable delta is sub-threshold.
    oracle = _materiality(_brickwall(ref, 0.8), restored, "tonal-balance")
    assert not oracle["exceeds"], f"resampler differs from ideal bandlimit: delta={oracle['delta']}"


# ── Anti-masking ───────────────────────────────────────────────────────────

@pytest.mark.parametrize("ratio", RATIOS)
def test_varispeed_plus_fizz_still_flags_added_hf(ratio):
    """A varispeed candidate that ALSO has real added HF fizz must still flag added-hf after the
    resample-back — the resampler must not smear a genuine defect away (anti-masking)."""
    ref = _tonal()
    candidate = generate.add_fizz(resample_by_ratio(ref, ratio), SR, amount=0.10)
    restored = resample_to_length(candidate, ref.size)
    mat = _materiality(ref, restored, "added-hf")
    assert mat["exceeds"], f"added fizz masked by the resample-back at R={ratio} (delta={mat['delta']})"
    assert mat["verdict"] == "regression_suspected"
    assert mat["delta"] > 3.0  # clearly above the default added-hf threshold, not a marginal call


# ── Numerical + contract properties ────────────────────────────────────────

def test_lossless_roundtrip_null_residual_is_deep():
    """A tonal up-sample-first round trip returns to within numerical noise — the sample-domain
    null-residual (which the varispeed lane relies on) comes back below -100 dB, and compare's
    corroboration reads 'corroborated'."""
    from quality_lab.dsp import null_residual_db
    ref = _tonal()
    restored = _roundtrip(ref, 1.5)
    nr = null_residual_db(ref, restored)
    assert nr.residual_db < -100.0, f"round-trip residual too high: {nr.residual_db} dB"
    report = compare.compare_arrays(ref, restored, SR, profile="tonal-balance", reference_role="golden")
    assert report["advisory"]["corroboration"]["status"] == "corroborated"


def test_resample_roundtrip_recovers_length():
    for ratio in RATIOS:
        ref = _tonal()
        candidate = resample_by_ratio(ref, ratio)
        assert candidate.size == round(ref.size * ratio)
        assert resample_to_length(candidate, ref.size).size == ref.size


def test_resample_edge_cases():
    assert resample_to_length(np.zeros(0), 10).shape == (10,)
    assert resample_to_length(np.array([0.5]), 4).tolist() == [0.5, 0.5, 0.5, 0.5]
    ident = np.linspace(-1.0, 1.0, 100)
    assert np.array_equal(resample_to_length(ident, 100), ident)  # identity ratio = exact passthrough
    assert resample_to_length(ident, 0).size == 0
    with pytest.raises(ValueError):
        resample_by_ratio(ident, 0.0)
    with pytest.raises(ValueError):
        resample_to_length(np.zeros((4, 2)), 8)  # non-1-D
