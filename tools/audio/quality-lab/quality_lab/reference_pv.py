"""A textbook phase vocoder — an INDEPENDENT reference algorithm for credibility
testing, NOT the product engine and NOT the detector's matched degradation.

The point (per the code review): the synthetic `smear_transients` is authored to match
what the detector measures, so passing on it is partly circular. A real phase vocoder
smears transient attacks through genuine STFT analysis/resynthesis phase mechanics —
the actual documented artifact — with no knowledge of our detector. If the
transient-sharpness detector fires on THIS output, that is non-circular evidence it
catches real PV smear.

Standard Laroche-style phase vocoder: STFT (Hann), per-bin phase advance accumulated
at the synthesis hop, overlap-add. No transient handling (that's the whole point — an
untreated PV smears attacks).
"""
from __future__ import annotations

import numpy as np


def phase_vocoder_stretch(y: np.ndarray, ratio: float, n_fft: int = 2048, hop: int = 512) -> np.ndarray:
    """Time-stretch `y` by `ratio` (out_len ~= ratio*len) with a plain phase vocoder.

    Deterministic. Attacks WILL smear — there is no transient preservation here.
    """
    y = np.asarray(y, dtype=np.float64)
    win = np.hanning(n_fft).astype(np.float64)
    hop_a = hop
    hop_s = int(round(hop * ratio))

    # Analysis STFT
    n_frames = 1 + max(0, (len(y) - n_fft) // hop_a)
    if n_frames < 2:
        return y.copy()
    stft = np.empty((n_frames, n_fft // 2 + 1), dtype=np.complex128)
    for i in range(n_frames):
        s = i * hop_a
        stft[i] = np.fft.rfft(y[s : s + n_fft] * win)

    mag = np.abs(stft)
    phase = np.angle(stft)
    bins = np.arange(n_fft // 2 + 1)
    omega = 2 * np.pi * bins * hop_a / n_fft  # expected phase advance per analysis hop

    # Phase accumulation: unwrap analysis phase increments, re-accumulate at synthesis hop.
    out_phase = np.empty_like(phase)
    out_phase[0] = phase[0]
    for i in range(1, n_frames):
        dphi = phase[i] - phase[i - 1] - omega
        dphi = dphi - 2 * np.pi * np.round(dphi / (2 * np.pi))  # principal value
        inst_omega = omega + dphi
        out_phase[i] = out_phase[i - 1] + inst_omega * (hop_s / hop_a)

    # Synthesis overlap-add
    out_len = hop_s * (n_frames - 1) + n_fft
    out = np.zeros(out_len, dtype=np.float64)
    wsum = np.zeros(out_len, dtype=np.float64)
    for i in range(n_frames):
        spec = mag[i] * np.exp(1j * out_phase[i])
        frame = np.fft.irfft(spec, n_fft) * win
        s = i * hop_s
        out[s : s + n_fft] += frame
        wsum[s : s + n_fft] += win * win
    out /= np.maximum(wsum, 1e-9)
    return out
