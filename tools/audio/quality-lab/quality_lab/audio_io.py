"""WAV I/O + level-matching — the shared front of every pipeline (§5 P0, §6).

Pure numpy + soundfile (both permissive). No FFT/analysis here; this is just load,
save, and the RMS level-match that the skill calls rule #1 ("normalize before you
listen / measure, or the A/B lies"). Every detector runs on level-matched audio.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf


def load_wav(path: str) -> tuple[np.ndarray, int]:
    """Load a WAV as mono float64 + sample rate (channels are mean-downmixed)."""
    y, sr = sf.read(path, always_2d=False)
    y = np.asarray(y, dtype=np.float64)
    if y.ndim > 1:
        y = y.mean(axis=1)
    return y, int(sr)


def save_wav(path: str, y: np.ndarray, sr: int) -> None:
    """Write a float32 WAV (no int16 quantization floor — what doctor/compare want)."""
    sf.write(path, np.asarray(y, dtype=np.float32), int(sr), subtype="FLOAT")


def rms(y: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.asarray(y, dtype=np.float64) ** 2) + 1e-20))


def level_match(candidate: np.ndarray, reference: np.ndarray) -> np.ndarray:
    """Scale `candidate` so its RMS equals `reference`'s. Returns a new array.

    This is the cheap, always-on level-match (skill rule #1). ITU-R BS.1770 loudness
    matching is a later option (§5 P0b); RMS is the floor that makes A/B honest.
    """
    cand = np.asarray(candidate, dtype=np.float64)
    ref_rms = rms(reference)
    cand_rms = rms(cand)
    if cand_rms <= 1e-12:
        return cand.copy()
    return cand * (ref_rms / cand_rms)
