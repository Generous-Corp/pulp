#!/usr/bin/env python3
"""Reusable audio-quality metrics for time-stretch / pitch A/B comparison.

These are the measurements that proved RELIABLE while tuning Pulp's OfflineStretch
against reference renders. They are necessary-but-not-sufficient — the ear is
ground truth — but they catch the things ears catch (brightness, pitch fidelity,
pitch stability, attack sharpness) and let an agent iterate without a human in the
loop for every change.

Pure numpy + soundfile (both permissively licensed). No GPL deps. See NOTICE.md.
"""
import numpy as np
import soundfile as sf


def load(path):
    """Load a wav as mono float64 + sample rate."""
    y, sr = sf.read(path)
    y = y if y.ndim == 1 else y.mean(axis=1)
    return y.astype(np.float64), sr


def spectral_centroid(y, sr, N=2048, H=512):
    """Brightness. The single most reliable 'muddy vs clear' metric here."""
    S = np.zeros(N // 2 + 1)
    win = np.hanning(N)
    for i in range(0, len(y) - N, H):
        S += np.abs(np.fft.rfft(y[i:i + N] * win))
    f = np.fft.rfftfreq(N, 1 / sr)
    return float(np.sum(f * S) / (np.sum(S) + 1e-9))


def onset_sharpness(y, sr, W=256, H=128):
    """Attack punch: peak/mean of half-wave-rectified energy flux."""
    e = np.array([np.sqrt(np.mean(y[i:i + W] ** 2) + 1e-12)
                  for i in range(0, len(y) - W, H)])
    flux = np.maximum(0.0, np.diff(e))
    return float(flux.max() / (flux.mean() + 1e-9)) if flux.size else 0.0


def dominant_peak_hz(y, sr, fmax=600.0, N=8192, H=2048):
    """Pitch-fidelity probe: the strongest spectral peak below `fmax`. Compare to
    the SOURCE — a faithful time-stretch must keep this identical at every ratio."""
    S = np.zeros(N // 2 + 1)
    win = np.hanning(N)
    for i in range(0, len(y) - N, H):
        S += np.abs(np.fft.rfft(y[i:i + N] * win)) ** 2
    f = np.fft.rfftfreq(N, 1 / sr)
    band = f < fmax
    return float(f[band][np.argmax(S[band])]) if band.any() else 0.0


def lowfreq_wobble(y, sr, fmax=300.0, N=2048, H=256):
    """Pitch INstability (std Hz of the dominant low frequency over time). High =
    the 'wobbly when hit' artifact. Lower is steadier."""
    f = np.fft.rfftfreq(N, 1 / sr)
    band = f < fmax
    fb = f[band]
    win = np.hanning(N)
    track = []
    thr = 1e-4 * (np.abs(y).max() + 1e-12)
    for i in range(0, len(y) - N, H):
        S = np.abs(np.fft.rfft(y[i:i + N] * win))[band]
        if S.max() > thr:
            track.append(fb[np.argmax(S)])
    return float(np.std(track)) if len(track) > 4 else 0.0


def ltas(y, sr, N=4096, H=1024):
    """Normalized long-term average spectrum (shape, sums to 1)."""
    S = np.zeros(N // 2 + 1)
    win = np.hanning(N)
    for i in range(0, len(y) - N, H):
        S += np.abs(np.fft.rfft(y[i:i + N] * win)) ** 2
    S = np.sqrt(S)
    return S / (S.sum() + 1e-9)


def spectral_l1(a_path, b_path):
    """L1 distance between two renders' LTAS shapes (0 = identical balance). Use
    for 'does engine X match reference Y'. Fine harmonic-bin structure inflates it
    between different implementations — read it alongside the band table, not alone."""
    a, sr = load(a_path)
    b, _ = load(b_path)
    return float(np.abs(ltas(a, sr) - ltas(b, sr)).sum())


def band_balance(y, sr, edges=(0, 200, 800, 2000, 6000, 24000)):
    """Energy fraction per band — the trustworthy 'EQ match' check (broad balance,
    immune to the fine-structure noise that inflates spectral_l1)."""
    S = ltas(y, sr)
    f = np.fft.rfftfreq(2 * (len(S) - 1), 1 / sr)
    out = {}
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (f >= lo) & (f < hi)
        out[f"{lo}-{hi}"] = float(S[m].sum())
    return out


def summary(path):
    """All scalar metrics for one render."""
    y, sr = load(path)
    return dict(
        sr=sr, seconds=round(len(y) / sr, 3),
        centroid=round(spectral_centroid(y, sr), 1),
        onset=round(onset_sharpness(y, sr), 2),
        peak_hz=round(dominant_peak_hz(y, sr), 1),
        wobble=round(lowfreq_wobble(y, sr), 2),
    )


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        print(p, summary(p))
