"""Shared pieces of the audio-decoder seed corpus: the deterministic test
signal, PCM packing, encoder discovery and the Corpus collector.
"""

from __future__ import annotations

import math
import os
import random
import shutil
import struct
import subprocess
import sys


PRNG_SEED = 0x5EED_C0DE
MAX_SEED_BYTES = 32 * 1024

# Candidate tool locations, first match wins. Absolute paths pin the encoders
# this corpus was generated with; the bare name is a fallback for other hosts.
TOOL_CANDIDATES = {
    "flac": ["/opt/homebrew/bin/flac", "flac"],
    "lame": ["/opt/homebrew/bin/lame", "lame"],
    "ffmpeg": ["/usr/local/bin/ffmpeg", "ffmpeg"],
    "afconvert": ["/usr/bin/afconvert", "afconvert"],
}


def find_tool(name: str) -> str | None:
    for candidate in TOOL_CANDIDATES[name]:
        if os.path.isabs(candidate):
            if os.access(candidate, os.X_OK):
                return candidate
        else:
            found = shutil.which(candidate)
            if found:
                return found
    return None


def run(cmd: list[str], cwd: str | None = None) -> None:
    subprocess.run(cmd, cwd=cwd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


# --------------------------------------------------------------------------
# Deterministic test signal
# --------------------------------------------------------------------------


def make_signal(rate: int, seconds: float, channels: int, amplitude: float = 0.6) -> list[list[float]]:
    """Per-channel float samples in (-1, 1): two sines plus faint seeded noise.

    Channel k gets its own fundamental so stereo seeds are not accidentally
    mono. The noise keeps lossless encoders from collapsing to trivial
    subframes while staying fully reproducible.
    """
    prng = random.Random(PRNG_SEED + rate * 7 + channels)
    frames = int(round(rate * seconds))
    out: list[list[float]] = []
    for ch in range(channels):
        f0 = 220.0 * (1.5 ** ch)
        f1 = f0 * 2.5
        samples = []
        for n in range(frames):
            t = n / rate
            env = min(1.0, n / 64.0, (frames - n) / 64.0)  # short fades, no clicks
            v = amplitude * env * (0.7 * math.sin(2 * math.pi * f0 * t) + 0.3 * math.sin(2 * math.pi * f1 * t))
            v += (prng.random() - 0.5) * 0.002
            samples.append(max(-0.999, min(0.999, v)))
        out.append(samples)
    return out


def quantize(signal: list[list[float]], bits: int) -> list[list[int]]:
    full = (1 << (bits - 1)) - 1
    return [[int(round(v * full)) for v in ch] for ch in signal]


def interleave(channels: list[list[int]]) -> list[int]:
    return [channels[c][i] for i in range(len(channels[0])) for c in range(len(channels))]


def pack_pcm_le(samples: list[int], bits: int, container_bits: int | None = None) -> bytes:
    container_bits = container_bits or bits
    shift = container_bits - bits
    out = bytearray()
    for s in samples:
        if container_bits == 8:
            out.append((s + 128) & 0xFF)
        else:
            v = (s << shift) & ((1 << container_bits) - 1)
            out += v.to_bytes(container_bits // 8, "little")
    return bytes(out)


def pack_pcm_be(samples: list[int], bits: int) -> bytes:
    out = bytearray()
    for s in samples:
        out += (s & ((1 << bits) - 1)).to_bytes(bits // 8, "big")
    return bytes(out)


def pack_float(signal: list[list[float]], fmt: str) -> bytes:
    inter = [signal[c][i] for i in range(len(signal[0])) for c in range(len(signal))]
    return struct.pack("<" + fmt * len(inter), *inter)


class Corpus:
    def __init__(self) -> None:
        self.files: dict[str, bytes] = {}
        self.notes: list[str] = []
        self.tools_used: dict[str, str] = {}
        self.skipped: list[str] = []

    def add(self, name: str, data: bytes) -> bytes:
        if name in self.files:
            raise ValueError(f"duplicate seed name {name}")
        if len(data) > MAX_SEED_BYTES:
            raise ValueError(f"{name} is {len(data)} bytes, over the {MAX_SEED_BYTES}-byte seed budget")
        self.files[name] = data
        return data

    def skip(self, name: str, reason: str) -> None:
        self.skipped.append(f"{name}: {reason}")
        print(f"skip {name}: {reason}", file=sys.stderr)
