"""Log-frequency shift compensation — the tonal-balance axis under a declared
duration-preserving pitch shift (Tier 3 / T3.3, spec §5 `pitch:S` row).

`relative_centroid_shift` measures the candidate's LTAS centroid against the reference's.
Under a declared pitch shift by `ratio` (= 2**(S/12)) the shift ALONE scales the centroid by
`ratio`, so the raw shift reads ≈ `ratio − 1` — the intended transform drowning any real
algorithm damage. `pitch_compensated_centroid_shift` subtracts that expectation out: it
compares the candidate centroid to the EXPECTED shifted centroid `ratio · c_ref`, so a clean
shift reads ≈ 0 and only ADDED damage (dulling from formant smear / PV low-pass) shows through.
`ltas_logfreq_shift` is the vector-level primitive (the expected shifted reference LTAS) whose
centroid agrees with the closed form to within a bin-resolution floor.

Ground-truth construction: we synthesize an EXACT pitch shift by RESAMPLING a tonal signal by
`1/ratio` (play it `ratio`× faster). A pure resample scales every frequency by exactly `ratio`,
so the centroid scales by exactly `ratio` — knowable ground truth. Duration changes too, but
LTAS is a time-average, so duration is irrelevant to these spectral-only assertions (`pitch:S`
is duration-preserving in production; the resample is only our test instrument for a spectrally
exact shift). The tonal source keeps all harmonics well below Nyquist even at +12 st (f0=220,
13 harmonics -> 2.86 kHz, ×2 = 5.7 kHz << 24 kHz), so no aliasing corrupts the ground truth.
"""
from __future__ import annotations

import numpy as np
import pytest

from quality_lab import generate
from quality_lab.dsp import (
    ltas,
    ltas_logfreq_shift,
    pitch_compensated_centroid_shift,
    pitch_ratio_from_semitones,
    relative_centroid_shift,
    spectral_centroid_hz,
)

SR = 48000
DUR = 2.5


def _resample_pitch(y: np.ndarray, ratio: float) -> np.ndarray:
    """Exact resample-based pitch shift UP by `ratio` (play `ratio`× faster): output[k] =
    y[k·ratio] via linear interpolation, new length ≈ len(y)/ratio. Scales every partial's
    frequency by exactly `ratio`; duration shrinks by `ratio` (irrelevant to LTAS). This is a
    varispeed-style shift — the whole spectrum (formants included) scales, which is precisely
    what `ratio · c_ref` predicts, giving a knowable centroid ground truth."""
    y = np.asarray(y, dtype=np.float64)
    n = len(y)
    new_n = int(round(n / ratio))
    idx = np.arange(new_n) * ratio
    return np.interp(idx, np.arange(n), y)


def _tone() -> np.ndarray:
    return generate.render_tonal(SR, DUR, seed=0)[0]


# +3, +7, +12 semitones up; -5 down. Ratios 1.19 .. 2.0 and 0.75.
_SEMITONES = [3, 7, 12, -5]


def test_ratio_from_semitones() -> None:
    """Sanity on the helper + domain guard."""
    assert pitch_ratio_from_semitones(12) == pytest.approx(2.0)
    assert pitch_ratio_from_semitones(-12) == pytest.approx(0.5)
    assert pitch_ratio_from_semitones(0) == 1.0
    for bad in (float("nan"), float("inf")):
        with pytest.raises(ValueError):
            pitch_ratio_from_semitones(bad)
    for bad_ratio in (0.0, -1.0, float("nan")):
        with pytest.raises(ValueError):
            ltas_logfreq_shift(np.ones(4), np.arange(4.0), bad_ratio)
        with pytest.raises(ValueError):
            pitch_compensated_centroid_shift(_tone(), _tone(), SR, bad_ratio)


@pytest.mark.parametrize("semitones", _SEMITONES)
def test_clean_shift_compensated_near_zero_uncompensated_large(semitones: int) -> None:
    """A CLEAN exact shift reads compensated delta ≈ 0, while the UNCOMPENSATED relative
    centroid shift reads the raw pitch move (≈ ratio − 1). Both numbers printed."""
    ratio = pitch_ratio_from_semitones(semitones)
    ref = _tone()
    cand = _resample_pitch(ref, ratio)  # exact clean shift by `ratio`

    comp, c_ref, c_cand = pitch_compensated_centroid_shift(ref, cand, SR, ratio)
    raw, _, _ = relative_centroid_shift(ref, cand, SR)

    print(
        f"\nS={semitones:+d} ratio={ratio:.4f}: "
        f"c_ref={c_ref:7.1f}Hz c_cand={c_cand:7.1f}Hz "
        f"UNcompensated={raw:+.3f}  compensated={comp:+.3f}"
    )

    # Clean shift: compensated delta is near zero (only resampler/bin error remains).
    assert abs(comp) <= 0.05, f"clean shift should compensate to ~0, got {comp:+.3f}"
    # Uncompensated reads the raw pitch move — materially larger, and tracks ratio − 1.
    assert abs(raw) >= 0.5 * abs(ratio - 1.0), "uncompensated should read the pitch move"
    assert abs(raw) > abs(comp) + 0.05, "compensation must materially shrink the delta"


@pytest.mark.parametrize("semitones", [7, -5])
def test_shift_plus_dull_reads_negative(semitones: int) -> None:
    """A clean shift FOLLOWED by dulling reads a materially NEGATIVE compensated delta —
    the added damage shows through the compensation (the shift itself is subtracted out)."""
    ratio = pitch_ratio_from_semitones(semitones)
    ref = _tone()
    shifted = _resample_pitch(ref, ratio)
    dulled = generate.dull(shifted, SR, cutoff_hz=1000.0)

    comp_clean, _, _ = pitch_compensated_centroid_shift(ref, shifted, SR, ratio)
    comp_dull, _, _ = pitch_compensated_centroid_shift(ref, dulled, SR, ratio)

    print(
        f"\nS={semitones:+d}: compensated clean={comp_clean:+.3f}  "
        f"shift+dull={comp_dull:+.3f}"
    )

    assert comp_dull <= -0.12, f"shift+dull should read materially duller, got {comp_dull:+.3f}"
    assert comp_dull < comp_clean - 0.10, "dulling must push the compensated delta down"


def test_logfreq_shift_identity() -> None:
    """`ltas_logfreq_shift` by ratio 1.0 is an exact identity."""
    f, m = ltas(_tone(), SR)
    out = ltas_logfreq_shift(m, f, 1.0)
    assert np.array_equal(out, m), "ratio=1.0 must be byte-identical"


def test_logfreq_shift_interpolation_floor() -> None:
    """Interpolation error of `ltas_logfreq_shift` is small at the 2048-point LTAS grid for
    shifts up to ±12 st: the centroid of the shifted reference LTAS matches the closed-form
    `ratio · c_ref` to a tight floor. Reports the measured worst-case relative error."""
    f, m = ltas(_tone(), SR)
    c_ref = spectral_centroid_hz(f, m)

    worst = 0.0
    for semitones in range(-12, 13):
        ratio = pitch_ratio_from_semitones(semitones)
        shifted = ltas_logfreq_shift(m, f, ratio)
        c_shift = spectral_centroid_hz(f, shifted)
        expected = ratio * c_ref
        err = abs(c_shift - expected) / expected
        worst = max(worst, err)

    print(f"\nltas_logfreq_shift centroid vs ratio·c_ref: worst rel error = {worst:.4%}")
    # ~2.4% worst-case at the 2048-point grid — well below the tonal-balance axis threshold
    # (0.05) and, crucially, NOT injected into pitch_compensated_centroid_shift, which uses the
    # closed-form ratio·c_ref. This bounds only the vector-level (log-spectral-distance) primitive.
    assert worst <= 0.03, f"bin-interpolation floor exceeded: {worst:.4%}"


def test_logfreq_shift_nyquist_zeroed_on_downshift() -> None:
    """A down-shift (ratio < 1) leaves the top output bins empty (clamped to 0), not smeared
    copies of the last source bin: those bins query source frequencies above Nyquist."""
    f, m = ltas(_tone(), SR)
    ratio = 0.5  # -12 st: output bin at Nyquist queries source at 2×Nyquist -> no support
    shifted = ltas_logfreq_shift(m, f, ratio)
    # Bins whose pre-shift query exceeds the top source bin must be exactly zero.
    beyond = (f / ratio) > f[-1]
    assert np.any(beyond), "test premise: some output bins should exceed source Nyquist"
    assert np.all(shifted[beyond] == 0.0), "beyond-Nyquist output bins must be zeroed"
