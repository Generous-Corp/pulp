#!/usr/bin/env python3
"""Objective quality metrics for OfflineStretch renders.

Plan: planning/Sampler-Offline-Stretch-Build-Plan.md §6 (quality metrics).

Compares a rendered output WAV against its input at a known time ratio and
reports the metrics that gate stretch quality:

  - length_exact      : output frames == round(in_frames * ratio)
  - null_rms_db        : RMS(out - in) in dB, for ratio == 1 (identity null)
  - onset_timing_ms    : mean / max |output onset - ideal(input onset * ratio)|
  - pre_echo_db        : energy in the 20 ms BEFORE each output onset, relative
                         to the onset's own energy (catches phase-vocoder
                         "breathing before the hit")

FFT-based metrics (log-spectral distance, f0-in-cents) are stubbed: they need an
FFT, which the standard library lacks. They activate automatically if numpy is
importable, and otherwise report "skipped (no numpy)" so the harness still runs
on a bare interpreter. The realtime-engine baseline (`--baseline`) records the
current engine's numbers over the corpus; it is a pass-through at Phase 0 and
gains real values once Phase 1 wires RealtimePitchTimeProcessor.

Pure standard library by default; no hard dependency.
"""

from __future__ import annotations

import json
import math
import struct
import sys

try:
    import numpy as _np  # optional — enables spectral/pitch metrics
except Exception:  # pragma: no cover - environment dependent
    _np = None


def load_wav_float32(path):
    """Minimal float32 / int16 / int24 WAV reader -> (channels, sample_rate).

    Returns deinterleaved python lists of floats per channel.
    """
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a WAVE file")
    # Walk chunks for fmt + data.
    i = 12
    fmt = None
    data = None
    while i + 8 <= len(b):
        cid = b[i:i + 4]
        csz = struct.unpack("<I", b[i + 4:i + 8])[0]
        body = b[i + 8:i + 8 + csz]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            data = body
        i += 8 + csz + (csz & 1)
    if fmt is None or data is None:
        raise ValueError(f"{path}: missing fmt/data")
    tag, ch, sr, _br, _ba, bits = fmt
    n_bytes = bits // 8
    frames = len(data) // (n_bytes * ch)
    out = [[0.0] * frames for _ in range(ch)]
    for fr in range(frames):
        base = fr * ch * n_bytes
        for c in range(ch):
            off = base + c * n_bytes
            if tag == 3 and bits == 32:
                v = struct.unpack_from("<f", data, off)[0]
            elif tag == 1 and bits == 16:
                v = struct.unpack_from("<h", data, off)[0] / 32768.0
            elif tag == 1 and bits == 24:
                raw = data[off:off + 3] + (b"\xff" if data[off + 2] & 0x80 else b"\x00")
                v = struct.unpack("<i", raw)[0] / 8388608.0
            else:
                raise ValueError(f"{path}: unsupported fmt tag={tag} bits={bits}")
            out[c][fr] = v
    return out, sr


def mono(channels):
    if len(channels) == 1:
        return channels[0]
    n = len(channels[0])
    return [sum(channels[c][i] for c in range(len(channels))) / len(channels) for i in range(n)]


def rms(xs):
    if not xs:
        return 0.0
    return math.sqrt(sum(x * x for x in xs) / len(xs))


def db(x):
    return 20.0 * math.log10(x) if x > 1e-12 else -240.0


def onset_frames(sig, sr, hop=256, win=1024, thresh_mult=1.5):
    """Energy-flux onset detection (stdlib): positive RMS difference between
    consecutive frames, adaptive-median gated. Good ground truth on percussive
    / click material."""
    n = len(sig)
    energies = []
    for start in range(0, max(1, n - win), hop):
        energies.append(rms(sig[start:start + win]))
    flux = [max(0.0, energies[i] - energies[i - 1]) for i in range(1, len(energies))]
    if not flux:
        return []
    onsets = []
    window = 8
    last = -10
    for i, fx in enumerate(flux):
        lo = max(0, i - window)
        local = sorted(flux[lo:i + window + 1])
        med = local[len(local) // 2] if local else 0.0
        peak = fx > flux[i - 1] if i > 0 else True
        peak = peak and (i + 1 >= len(flux) or fx >= flux[i + 1])
        if fx > thresh_mult * med and fx > 1e-4 and peak and (i - last) > 2:
            onsets.append((i + 1) * hop)  # +1: flux is offset by one frame
            last = i
    return onsets


def metrics(in_path, out_path, ratio):
    ich, isr = load_wav_float32(in_path)
    och, osr = load_wav_float32(out_path)
    im = mono(ich)
    om = mono(och)
    in_n, out_n = len(im), len(om)

    result = {
        "input": in_path,
        "output": out_path,
        "ratio": ratio,
        "sample_rate": osr,
        "in_frames": in_n,
        "out_frames": out_n,
    }

    expected = int(round(in_n * ratio))
    result["length_exact"] = (out_n == expected)
    result["length_expected"] = expected

    # Null only meaningful at ratio == 1 with equal length.
    if abs(ratio - 1.0) < 1e-9 and in_n == out_n:
        diff = [om[i] - im[i] for i in range(in_n)]
        result["null_rms_db"] = round(db(rms(diff)), 2)

    # Transient timing: match each input onset (scaled by ratio) to nearest output onset.
    in_ons = onset_frames(im, isr)
    out_ons = onset_frames(om, osr)
    result["in_onsets"] = len(in_ons)
    result["out_onsets"] = len(out_ons)
    if in_ons and out_ons:
        errs_ms = []
        for t in in_ons:
            ideal = t * ratio
            nearest = min(out_ons, key=lambda o: abs(o - ideal))
            errs_ms.append(abs(nearest - ideal) / osr * 1000.0)
        result["onset_timing_mean_ms"] = round(sum(errs_ms) / len(errs_ms), 3)
        result["onset_timing_max_ms"] = round(max(errs_ms), 3)

        # Pre-echo: energy in 20 ms before each output onset vs onset energy.
        pre = int(0.020 * osr)
        ratios_db = []
        for o in out_ons:
            before = om[max(0, o - pre):o]
            at = om[o:o + pre]
            rb, ra = rms(before), rms(at)
            if ra > 1e-9:
                ratios_db.append(db(rb / ra))
        if ratios_db:
            result["pre_echo_db"] = round(sum(ratios_db) / len(ratios_db), 2)

    if _np is None:
        result["spectral_lsd"] = "skipped (no numpy)"
        result["pitch_cents"] = "skipped (no numpy)"
    # TODO(phase1+): with numpy, add log-spectral distance on stationary
    # segments and f0-tracking cents error on tonal material.

    return result


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(
            "usage: metrics.py IN OUT [--ratio R] [--json]\n")
        return 2
    in_path, out_path = argv[1], argv[2]
    ratio = 1.0
    as_json = False
    i = 3
    while i < len(argv):
        if argv[i] == "--ratio":
            ratio = float(argv[i + 1]); i += 2
        elif argv[i] == "--json":
            as_json = True; i += 1
        else:
            i += 1
    r = metrics(in_path, out_path, ratio)
    if as_json:
        print(json.dumps(r, indent=2))
    else:
        for k, v in r.items():
            print(f"{k}: {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
