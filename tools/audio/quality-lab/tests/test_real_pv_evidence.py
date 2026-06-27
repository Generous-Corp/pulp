"""Non-circular credibility test (the code-review's #1 ask).

The synthetic `smear_transients` is authored by the same hand as the detector, so
passing on it is partly self-fulfilling. Here we run a REAL, independent textbook
phase vocoder (`reference_pv`) — which smears attacks through genuine STFT
resynthesis, with no knowledge of the detector — and require the transient detector
to fire on its output. Plus a render-vs-itself control proving the detector measures
the *difference* from a faithful reference, not merely "is this a PV render".
"""
from __future__ import annotations

from quality_lab import align, audio_io, generate, reference_pv
from quality_lab.detectors import transient_sharpness


def _run(reference, candidate, sr):
    candidate = audio_io.level_match(candidate, reference)
    ro = align.detect_onsets(reference, sr)
    co = align.detect_onsets(candidate, sr)
    pairs = align.map_onsets(ro, co, len(reference) / sr, len(candidate) / sr)
    return transient_sharpness.detect(reference, candidate, sr, pairs)


def test_detector_fires_on_real_phase_vocoder_smear():
    """A REAL phase vocoder smears attacks — the detector must catch it (non-circular)."""
    sr, ratio = 48000, 1.5
    source, _ = generate.render_drum_break(sr, 120.0, 1.0, 0)
    reference, _ = generate.render_drum_break(sr, 120.0, ratio, 0)  # transient-preserving
    pv = reference_pv.phase_vocoder_stretch(source, ratio)  # independent algorithm

    det = _run(reference, pv, sr)
    assert det.fired, f"detector missed real PV smear: scalar={det.scalar}"
    assert det.scalar >= 0.3
    assert det.coverage >= 0.5, f"low coverage on real PV: {det.coverage}"


def test_pv_render_vs_itself_is_clean():
    """Control: a PV render compared to ITSELF must be clean — proving the detector
    measures the smear *relative to a faithful reference*, not 'PV-ness' per se."""
    sr, ratio = 48000, 1.5
    source, _ = generate.render_drum_break(sr, 120.0, 1.0, 0)
    pv = reference_pv.phase_vocoder_stretch(source, ratio)

    det = _run(pv, pv.copy(), sr)
    assert not det.fired, f"detector false-positive on identical PV renders: {det.scalar}"
    assert det.scalar <= 0.05
