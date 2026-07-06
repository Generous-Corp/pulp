"""Warp-aware spectral flux — the graininess axis under a pitch-preserving time-stretch
(Tier 3 / T3.2, spec §5 `graininess` row).

`mean_spectral_flux` measures spectral change per frame STEP. Under a pitch-preserving
stretch by `R` (candidate ≈ R× the reference duration) the same source content evolves
`R`× more slowly, so the candidate's per-step flux is deflated by construction — a clean
stretch reads as a FALSE "smoother/less grainy". The fix (spec): measure the candidate
with `hop_scale=R` so `hop_cand = round(hop_ref · R)` (n_fft unchanged); both flux series
then step through the same *source-content* interval. New tolerance class
`spectral_flux.v2-warp`.

These tests prove the core claim numerically:
  1. clean stretch: hop-scaled candidate flux ≈ reference; un-scaled is materially LOWER
     (the false-smoother artifact), and hop-scaling is the closer match.
  2. grainy stretch: hop-scaled candidate flux is materially ELEVATED vs reference
     (the real defect survives the reformulation).
  3. hop_scale=1.0 is byte-identical to the current call (no regression for the
     un-warped graininess axis / existing suite).

Stretch stand-in caveat: quality-lab has no clean product-grade PV stretch to render a
*truly clean* stretch from. `reference_pv.phase_vocoder_stretch` is a real pitch-preserving
PV, but a plain (untreated) PV adds genuine phasiness/churn whose own flux swamps the
hop-deflation effect these tests isolate — so it cannot stand in for a CLEAN stretch. We
instead synthesize the clean stretch ANALYTICALLY (`_clean_tonal`): a constant-pitch carrier
whose vibrato/tremolo modulation is indexed by normalized position, so a longer duration
spreads the identical modulation shape over more real seconds at the same pitch. That is an
exact clean pitch-preserving time-stretch of this signal class — the ideal instrument for
isolating the per-step-deflation the reformulation targets. The grainy case degrades that
clean stretch with `generate.grainy` (a real frame-to-frame-churn stand-in already used by
the graininess detector's positive control).
"""
from __future__ import annotations

import numpy as np
import pytest

from quality_lab import generate
from quality_lab.dsp import mean_spectral_flux

SR = 48000
BASE_HOP = 256  # the mean_spectral_flux default; hop_scale=R ⇒ round(BASE_HOP·R)


def _clean_tonal(dur_s: float, f0: float = 220.0, n_vib: float = 12.0, n_trem: float = 2.0) -> np.ndarray:
    """A sustained tonal signal that is EXACTLY clean-stretchable: the carrier stays at a
    constant `f0` (pitch preserved) while vibrato/tremolo are indexed by normalized position
    `p = t/dur_s`, so `_clean_tonal(D·R)` is a clean pitch-preserving time-stretch by `R` of
    `_clean_tonal(D)` — same modulation shape spread over R× the real time, same pitch.
    Low baseline flux (graininess shows), no onsets."""
    n = int(dur_s * SR)
    t = np.arange(n) / SR
    p = t / dur_s
    vib = 1.0 + 0.01 * np.sin(2 * np.pi * n_vib * p)
    phase = 2 * np.pi * f0 * np.cumsum(vib) / SR
    y = np.zeros(n)
    for k in range(1, 14):
        hz = f0 * k
        formant = np.exp(-(((hz - 700.0) / 400.0) ** 2)) + 0.7 * np.exp(-(((hz - 1800.0) / 600.0) ** 2))
        y += (1.0 / k) * (0.35 + formant) * np.sin(k * phase)
    y *= 0.85 + 0.15 * np.sin(2 * np.pi * n_trem * p)
    ar = int(0.05 * SR)
    env = np.ones(n)
    env[:ar] = np.linspace(0, 1, ar)
    env[-ar:] = np.linspace(1, 0, ar)
    y *= env
    return y / (np.max(np.abs(y)) + 1e-12) * 0.5


def _rel(flux_cand: float, flux_ref: float) -> float:
    """The graininess axis' consumed metric (compare._graininess_kernel): relative flux
    increase. Positive = grainier (bad); negative = smoother."""
    return (flux_cand - flux_ref) / flux_ref


# Stretch (R>1) deflates un-scaled flux (false "smoother"); compression (R<1) inflates it
# (false "grainier"). Both are the by-construction artifact hop-scaling must undo. Excludes
# mild ratios (~1.25) where the fixed-n_fft window-span residual competes with a small
# deflation — an honest documented limitation, not a case to over-claim on.
_RATIOS = [0.75, 1.5, 2.0]


@pytest.mark.parametrize("R", _RATIOS)
def test_clean_stretch_hop_scaling_restores_reference_flux(R: float) -> None:
    """CLEAN pitch-preserving stretch: un-scaled candidate flux is materially off (wrong
    sign vs reference), hop-scaled flux is close to reference AND a strictly better match."""
    D = 3.0
    ref = _clean_tonal(D)
    cand = _clean_tonal(D * R)  # exact clean stretch by R

    flux_ref = mean_spectral_flux(ref, SR)
    flux_unscaled = mean_spectral_flux(cand, SR)                 # hop_scale=1.0 (today's bug)
    flux_scaled = mean_spectral_flux(cand, SR, hop_scale=R)       # v2-warp

    rel_unscaled = _rel(flux_unscaled, flux_ref)
    rel_scaled = _rel(flux_scaled, flux_ref)
    msg = (f"R={R}: ref={flux_ref:.5f} unscaled={flux_unscaled:.5f} (rel {rel_unscaled:+.3f}) "
           f"scaled={flux_scaled:.5f} (rel {rel_scaled:+.3f})")

    # 1. Un-scaled flux is materially deflated (R>1) / inflated (R<1) — the false artifact.
    #    Its sign is the WRONG one: stretch looks smoother, compression looks grainier.
    assert abs(rel_unscaled) >= 0.12, "expected a material un-scaled artifact; " + msg
    assert np.sign(rel_unscaled) == -np.sign(R - 1.0), "artifact should have the wrong sign; " + msg

    # 2. Hop-scaled flux is close to the reference (a clean stretch is NOT grainy) ...
    assert abs(rel_scaled) <= 0.22, "hop-scaled flux should ≈ reference; " + msg
    # ... and is a strictly better match than the un-scaled measure.
    assert abs(rel_scaled) < abs(rel_unscaled), "hop-scaling should improve the match; " + msg


def test_grainy_stretch_flux_elevated_after_hop_scaling() -> None:
    """GRAINY stretch: the real defect survives the reformulation — hop-scaled candidate
    flux is materially ELEVATED vs reference (well past the graininess axis' 0.5 rel
    threshold), and far above the clean stretch's hop-scaled flux (defect not masked)."""
    R = 1.5
    D = 3.0
    ref = _clean_tonal(D)
    clean = _clean_tonal(D * R)
    grainy = generate.grainy(clean, SR)  # stretch that ADDED graininess

    flux_ref = mean_spectral_flux(ref, SR)
    flux_clean_scaled = mean_spectral_flux(clean, SR, hop_scale=R)
    flux_grainy_scaled = mean_spectral_flux(grainy, SR, hop_scale=R)

    rel_clean = _rel(flux_clean_scaled, flux_ref)
    rel_grainy = _rel(flux_grainy_scaled, flux_ref)
    msg = (f"ref={flux_ref:.5f} clean_scaled={flux_clean_scaled:.5f} (rel {rel_clean:+.3f}) "
           f"grainy_scaled={flux_grainy_scaled:.5f} (rel {rel_grainy:+.3f})")

    # Materially elevated vs reference — the defect is caught, not deflated away.
    assert rel_grainy >= 1.0, "grainy stretch should read materially grainier; " + msg
    # And well separated from the clean stretch (anti-masking: the reformulation preserves
    # the discriminative gap the graininess axis needs).
    assert rel_grainy > rel_clean + 0.5, "grainy vs clean gap should survive; " + msg


@pytest.mark.parametrize("R", _RATIOS)
def test_hop_scale_one_is_byte_identical(R: float) -> None:
    """hop_scale=1.0 (default) is byte-identical to the current un-scaled call, on both
    the reference and a stretched candidate — no regression for the un-warped axis / suite."""
    ref = _clean_tonal(3.0)
    cand = _clean_tonal(3.0 * R)
    for y in (ref, cand):
        base = mean_spectral_flux(y, SR)
        assert mean_spectral_flux(y, SR, hop_scale=1.0) == base       # explicit default
        # round(BASE_HOP·1.0) == BASE_HOP, so the scaled path reproduces the hop path exactly.
        assert mean_spectral_flux(y, SR, hop=BASE_HOP, hop_scale=1.0) == base
