"""LTAS log-spectral distance (T3.2): a warp-compatible raw comparator.

`ltas_log_spectral_distance_db` is the corroboration measure the phase-sensitive sample
residual cannot provide across a time warp. These tests pin its four load-bearing properties:
identity -> 0, monotonic growth on a spectral-envelope change (dulling), phase-BLINDNESS
(same magnitude spectrum, randomized phase -> ~0), and warp-compatibility (a different-LENGTH
candidate still returns a finite, sensible distance with no shape crash).
"""
from __future__ import annotations

import numpy as np

from quality_lab import generate
from quality_lab.dsp import ltas_log_spectral_distance_db as lsd

SR = 48000


def _tone() -> np.ndarray:
    y, _ = generate.render_tonal(SR)
    return y


def _phase_randomized(y: np.ndarray, seed: int = 3) -> np.ndarray:
    """Same magnitude spectrum, randomized phase: keep |X|, replace the angle, invert.

    DC (and Nyquist for an even length) must stay real, or irfft would not reproduce |X|."""
    X = np.fft.rfft(y)
    mag = np.abs(X)
    rng = np.random.default_rng(seed)
    ang = rng.uniform(-np.pi, np.pi, size=mag.shape)
    ang[0] = 0.0
    if y.size % 2 == 0:
        ang[-1] = 0.0
    out = np.fft.irfft(mag * np.exp(1j * ang), n=y.size)
    # Sanity: the construction actually preserves the full-length magnitude spectrum.
    assert np.max(np.abs(np.abs(np.fft.rfft(out)) - mag)) < 1e-6
    return out


def test_identity_is_zero():
    assert lsd(_tone(), _tone(), SR) == 0.0


def test_level_robust():
    """A broadband gain is not a spectral change: the RMS-normalizing guard cancels it."""
    y = _tone()
    assert lsd(y, y * 8.0, SR) < 1e-9


def test_dulling_grows_monotonically():
    """A one-pole low-pass (dulling) grows the distance monotonically as the cutoff drops."""
    y = _tone()
    cutoffs = [6000.0, 3500.0, 2000.0, 1000.0]
    dists = [lsd(y, generate.dull(y, SR, cutoff_hz=c), SR) for c in cutoffs]
    assert dists[0] > 0.02, f"mild dulling should register: {dists}"
    assert all(b > a for a, b in zip(dists, dists[1:])), f"not monotonic: {dists}"
    # And a clear dulling is a large, unambiguous distance.
    assert dists[-1] > 0.4, f"heavy dulling should be large: {dists[-1]}"


def test_phase_blind():
    """Identical magnitude spectra with randomized phase -> distance ~ 0, and far below any
    clear spectral-envelope change (a strong dulling)."""
    y = _tone()
    d_phase = lsd(y, _phase_randomized(y), SR)
    d_dull = lsd(y, generate.dull(y, SR, cutoff_hz=1000.0), SR)  # a clear, heavy dulling
    assert d_phase < 0.15, f"phase-only difference should be ~0, got {d_phase}"
    assert d_phase < 0.4 * d_dull, f"phase {d_phase} not << clear dulling {d_dull}"


def test_warp_different_length_truncation():
    """A truncated (different-LENGTH) candidate with the SAME spectral content compares with no
    shape crash and reads near-identity."""
    y = _tone()
    trunc = y[: y.size // 2]
    d = lsd(y, trunc, SR)
    assert np.isfinite(d)
    assert d < 0.05, f"same-spectrum truncation should read near-identity: {d}"


def test_warp_resampled_is_finite_and_material():
    """A naive time-stretch (resample) is a genuine spectral change AND a length change; the
    distance must stay finite and register the change (the warp-compatibility contract)."""
    y = _tone()
    idx = np.linspace(0, y.size - 1, int(y.size * 1.5))
    stretched = np.interp(idx, np.arange(y.size), y)
    assert stretched.size != y.size  # different length, must not crash
    d = lsd(y, stretched, SR)
    assert np.isfinite(d) and d > 0.4, f"resample should be finite + material: {d}"
