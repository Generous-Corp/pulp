"""Stereo-width detector — image collapse and phase damage.

The mono detectors are blind to the stereo field. This detector operates on (N, 2)
arrays: it must fire when the image is narrowed toward mono or the channels are flipped
out of phase, and stay quiet on an identity render. Validated against a known,
synthetic width change.
"""
from __future__ import annotations

import numpy as np

from quality_lab import generate
from quality_lab.detectors import stereo_width
from quality_lab.dsp import interchannel_correlation, stereo_width_ratio

SR = 48000


def test_synthetic_pad_is_actually_wide():
    pad = generate.render_stereo_pad(SR, 2.0, 0)
    assert pad.shape[1] == 2
    assert stereo_width_ratio(pad) > 0.1  # non-trivial side energy
    assert interchannel_correlation(pad) < 0.999  # not already mono


def test_identity_is_clean():
    pad = generate.render_stereo_pad(SR, 2.0, 0)
    d = stereo_width.detect(pad, pad, SR)
    assert not d.fired and d.scalar == 0.0


def test_fires_on_partial_narrowing():
    pad = generate.render_stereo_pad(SR, 2.0, 0)
    narrowed = generate.narrow_stereo(pad, amount=0.8)  # 80% width collapse
    d = stereo_width.detect(pad, narrowed, SR)
    assert d.fired, f"missed stereo narrowing: {d.scalar}"
    assert d.scalar >= 0.5  # most of the width is gone


def test_full_mono_collapse_fires_hard():
    pad = generate.render_stereo_pad(SR, 2.0, 0)
    mono = generate.narrow_stereo(pad, amount=1.0)  # fully mono
    d = stereo_width.detect(pad, mono, SR)
    assert d.fired and d.scalar > 0.95


def test_phase_inversion_fires_even_without_width_loss():
    """Flipping R polarity keeps the width ratio high but wrecks mono-compatibility —
    the correlation sign-flip must fire."""
    pad = generate.render_stereo_pad(SR, 2.0, 0)
    inverted = generate.invert_phase_right(pad)
    d = stereo_width.detect(pad, inverted, SR)
    assert d.fired, "missed an out-of-phase channel inversion"
    assert "PHASE INVERTED" in d.notes


def test_widening_alone_is_not_flagged_worse():
    """Widening is a change, not a degradation — the 'made it worse' contract means a
    wider candidate does not fire (scalar measures width REDUCTION)."""
    pad = generate.render_stereo_pad(SR, 2.0, 0)
    mid = 0.5 * (pad[:, 0] + pad[:, 1])
    side = 0.5 * (pad[:, 0] - pad[:, 1]) * 1.8  # widen
    wider = np.stack([mid + side, mid - side], axis=1)
    d = stereo_width.detect(pad, wider, SR)
    assert not d.fired and d.scalar == 0.0
