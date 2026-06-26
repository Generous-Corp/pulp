#!/usr/bin/env python3
"""Generate the synthetic portion of the OfflineStretch test corpus.

The corpus has two parts:
  - synthetic/  — deterministic fixtures generated here (sine, click train,
    log sweep, silence, DC, full-scale, and click-loops at 60/90/120/174 BPM
    for the analysis module). These are checked in and used by correctness +
    metrics tests; they have exactly-known onsets and pitch so metrics are
    unambiguous.
  - musical/    — real loops (acoustic drums sparse/busy, 808-tail, hi-hat,
    vocal phrase, bass, full mix). NOT generated here; user-supplied per the
    README, because licensed audio cannot be synthesized. Tests that need
    musical material skip with a clear reason when musical/ is empty.

Pure standard library: writes 32-bit IEEE-float WAV (the engine's native
format), no numpy/soundfile dependency. Deterministic: identical output every
run (no RNG), so it can seed byte-identical regression baselines.
"""

from __future__ import annotations

import math
import os
import struct
import wave
from typing import List


def _write_float32_wav(path: str, channels: List[List[float]], sample_rate: int) -> None:
    """Write deinterleaved float channels as a 32-bit IEEE-float WAV."""
    num_ch = len(channels)
    num_frames = len(channels[0]) if num_ch else 0
    for c in channels:
        assert len(c) == num_frames, "channels must be equal length"

    # Interleave + clamp to [-1, 1] and pack as little-endian float32.
    data = bytearray()
    for i in range(num_frames):
        for c in range(num_ch):
            v = channels[c][i]
            if v > 1.0:
                v = 1.0
            elif v < -1.0:
                v = -1.0
            data += struct.pack("<f", v)

    # wave can't emit WAVE_FORMAT_IEEE_FLOAT directly, so write the RIFF
    # header by hand (fmt tag 3 = IEEE float, 32-bit).
    byte_rate = sample_rate * num_ch * 4
    block_align = num_ch * 4
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data)))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<I", 16))           # PCM-style fmt chunk size
        f.write(struct.pack("<H", 3))            # 3 = IEEE float
        f.write(struct.pack("<H", num_ch))
        f.write(struct.pack("<I", sample_rate))
        f.write(struct.pack("<I", byte_rate))
        f.write(struct.pack("<H", block_align))
        f.write(struct.pack("<H", 32))           # bits per sample
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(data)


def sine(freq: float, dur_s: float, sr: int, amp: float = 0.5) -> List[float]:
    n = int(round(dur_s * sr))
    w = 2.0 * math.pi * freq / sr
    return [amp * math.sin(w * i) for i in range(n)]


def log_sweep(f0: float, f1: float, dur_s: float, sr: int, amp: float = 0.5) -> List[float]:
    n = int(round(dur_s * sr))
    k = math.log(f1 / f0)
    out = []
    for i in range(n):
        t = i / sr
        # instantaneous-phase integral of an exponential sweep
        phase = 2.0 * math.pi * f0 * (dur_s / k) * (math.exp(k * t / dur_s) - 1.0)
        out.append(amp * math.sin(phase))
    return out


def click_train(bpm: float, bars: int, sr: int, beats_per_bar: int = 4,
                amp: float = 0.9, click_ms: float = 2.0) -> List[float]:
    """Impulse-ish clicks on every beat — exact, known onset positions so the
    analysis module and transient-timing metrics have ground truth."""
    beat_s = 60.0 / bpm
    total_beats = bars * beats_per_bar
    n = int(round(total_beats * beat_s * sr))
    out = [0.0] * n
    click_n = max(1, int(round(click_ms * 1e-3 * sr)))
    for b in range(total_beats):
        start = int(round(b * beat_s * sr))
        for j in range(click_n):
            idx = start + j
            if idx < n:
                # short decaying cosine burst (band-limited-ish click)
                env = 1.0 - (j / click_n)
                out[idx] = amp * env * math.cos(2.0 * math.pi * 2000.0 * j / sr)
    return out


def const(value: float, dur_s: float, sr: int) -> List[float]:
    return [value] * int(round(dur_s * sr))


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.normpath(os.path.join(here, "..", "corpus", "synthetic"))
    os.makedirs(out_dir, exist_ok=True)

    sr = 48000
    manifest = []

    def emit(name: str, channels: List[List[float]], note: str) -> None:
        path = os.path.join(out_dir, name)
        _write_float32_wav(path, channels, sr)
        frames = len(channels[0])
        manifest.append(f"{name}\t{len(channels)}ch\t{frames}fr\t{frames/sr:.3f}s\t{note}")
        print(f"  wrote {name} ({len(channels)}ch, {frames} fr)")

    print(f"Generating synthetic corpus -> {out_dir}")
    # Tonal: pitch ground truth for cents accuracy.
    emit("sine_440_mono.wav", [sine(440.0, 2.0, sr)], "A4 pitch-accuracy reference")
    emit("sine_440_stereo.wav",
         [sine(440.0, 2.0, sr), sine(440.0, 2.0, sr, amp=0.4)],
         "stereo image / coherence reference")
    # Transients: onset-timing + pre-echo ground truth.
    emit("clicks_120bpm.wav", [click_train(120.0, 2, sr)], "exact beat onsets @120")
    # Sweep: broadband spectral reference.
    emit("logsweep_20_20k.wav", [log_sweep(20.0, 20000.0, 3.0, sr)], "20Hz-20kHz log sweep")
    # Degenerate / safety inputs.
    emit("silence.wav", [const(0.0, 0.5, sr)], "silence-in -> silence-out")
    emit("dc.wav", [const(0.5, 0.5, sr)], "DC stability")
    emit("fullscale.wav", [const(1.0, 0.25, sr)], "full-scale no-blowup")
    # Analysis-module BPM targets.
    for bpm in (60.0, 90.0, 120.0, 174.0):
        emit(f"clickloop_{int(bpm)}bpm.wav", [click_train(bpm, 4, sr)],
             f"BPM-detect target {bpm:g}")

    with open(os.path.join(out_dir, "MANIFEST.tsv"), "w") as f:
        f.write("# name\tchannels\tframes\tduration\tnote (sr=48000, float32)\n")
        f.write("\n".join(manifest) + "\n")
    print(f"Done: {len(manifest)} files + MANIFEST.tsv")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
