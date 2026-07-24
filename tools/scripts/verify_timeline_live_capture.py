#!/usr/bin/env python3
"""Validate an explicitly supplied physical-input WAV capture."""

from __future__ import annotations

import argparse
import math
import os
import struct
import sys
import tempfile
from pathlib import Path

SKIP = 77


def chunks(payload: bytes):
    if len(payload) < 12 or payload[:4] != b"RIFF" or payload[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    offset = 12
    while offset + 8 <= len(payload):
        kind = payload[offset : offset + 4]
        size = struct.unpack_from("<I", payload, offset + 4)[0]
        start = offset + 8
        end = start + size
        if end > len(payload):
            raise ValueError("truncated WAV chunk")
        yield kind, payload[start:end]
        offset = end + (size & 1)


def inspect_capture(path: Path, minimum_frames: int, silence_threshold: float) -> str:
    payload = path.read_bytes()
    fmt = None
    audio = None
    for kind, body in chunks(payload):
        if kind == b"fmt ":
            fmt = body
        elif kind == b"data":
            audio = body
    if fmt is None or len(fmt) < 16 or audio is None:
        raise ValueError("WAV requires fmt and data chunks")
    format_tag, channels, sample_rate, _, block_align, bits = struct.unpack_from(
        "<HHIIHH", fmt
    )
    if channels == 0 or sample_rate == 0 or block_align == 0:
        raise ValueError("invalid WAV stream shape")
    frames = len(audio) // block_align
    if frames < minimum_frames:
        raise ValueError(f"capture has {frames} frames; need at least {minimum_frames}")

    peak = 0.0
    if format_tag == 3 and bits == 32:
        for (sample,) in struct.iter_unpack("<f", audio[: frames * block_align]):
            if not math.isfinite(sample):
                raise ValueError("capture contains a non-finite float sample")
            peak = max(peak, abs(sample))
    elif format_tag == 1 and bits == 16:
        for (sample,) in struct.iter_unpack("<h", audio[: frames * block_align]):
            peak = max(peak, abs(sample) / 32768.0)
    elif format_tag == 1 and bits == 24:
        for offset in range(0, frames * block_align, 3):
            raw = int.from_bytes(audio[offset : offset + 3], "little", signed=False)
            if raw & 0x800000:
                raw -= 1 << 24
            peak = max(peak, abs(raw) / 8388608.0)
    else:
        raise ValueError(f"unsupported WAV encoding tag={format_tag} bits={bits}")
    if peak <= silence_threshold:
        raise ValueError(
            f"capture peak {peak:.9f} is at/below silence threshold "
            f"{silence_threshold:.9f}"
        )
    return (
        f"PASS: physical capture {path} channels={channels} rate={sample_rate} "
        f"frames={frames} peak={peak:.6f}"
    )


def self_test() -> None:
    samples = struct.pack("<4f", 0.0, 0.0, 0.25, -0.5)
    fmt = struct.pack("<HHIIHH", 3, 1, 48_000, 192_000, 4, 32)
    payload = (
        b"RIFF"
        + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(samples))
        + b"WAVEfmt "
        + struct.pack("<I", len(fmt))
        + fmt
        + b"data"
        + struct.pack("<I", len(samples))
        + samples
    )
    with tempfile.TemporaryDirectory(prefix="pulp-live-capture-self-test-") as directory:
        path = Path(directory) / "capture.wav"
        path.write_bytes(payload)
        result = inspect_capture(path, 4, 1.0e-6)
        if not result.startswith("PASS:"):
            raise AssertionError(result)
        try:
            inspect_capture(path, 5, 1.0e-6)
        except ValueError:
            pass
        else:
            raise AssertionError("short capture negative control passed")
    print("PASS: live-capture verifier self-test")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--minimum-frames", type=int, default=4_800)
    parser.add_argument("--silence-threshold", type=float, default=1.0e-6)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    capture = args.capture
    if capture is None:
        value = os.environ.get("PULP_TIMELINE_LIVE_CAPTURE_WAV")
        capture = Path(value) if value else None
    if capture is None:
        print(
            "SKIP: physical live capture not supplied; set "
            "PULP_TIMELINE_LIVE_CAPTURE_WAV=/path/to/capture.wav"
        )
        return SKIP
    if args.minimum_frames <= 0 or args.silence_threshold < 0.0:
        print("FAIL: invalid verifier thresholds", file=sys.stderr)
        return 2
    try:
        print(inspect_capture(capture, args.minimum_frames, args.silence_threshold))
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
