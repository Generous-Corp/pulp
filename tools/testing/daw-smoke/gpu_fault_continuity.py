#!/usr/bin/env python3
"""Audio-continuity evidence for a GPU-audio fault injected inside REAPER (WP-h1).

The `gpu-fault` mode of reaper_smoke.py loads the GPU fault-continuity test
plug-in, injects one GPU fault at a recorded sample, and keeps audio running.
This module owns everything that does not need REAPER: the deterministic
stimulus and impulse response, the plug-in's capture-file format, the
independent CPU reference, the null analysis around the fault sample, the
receipt builder, and a dry run that exercises all of it with synthetic
captures.

Two captures are nulled:

* the plug-in side: every callback's input and output, written by the plug-in
  from a host thread. The reference is the recorded input convolved with the
  impulse response and delayed by the PDC each prepare reported. This is the
  continuity evidence: the fault fires during this realtime playback.
* the REAPER bounce: an offline render made after playback. REAPER compensates
  PDC, so the reference is the stimulus convolved with the impulse response,
  undelayed. The fault fired once, on the realtime pass, and an offline render
  runs the CPU path, so this only cross-checks the offline path and PDC.

A shared-I/O GPU convolver delivers `conv(input(t - L))` whether a block came
from the GPU or from the continuously primed CPU fallback, so a continuous
fallback nulls exactly and a dropout, a dry block or a stale block does not.
"""
from __future__ import annotations

import hashlib
import json
import math
import struct
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Sequence

SCHEMA = "pulp.gpu-audio.daw-continuity.v1"

# Audio-device faults are applied to the running provider by the plug-in's
# test-only Dawn proc-table interposer; render_force_loss calls ForceLoss on the
# editor's render device through the surface's device accessor, which is a
# separate Dawn device from the audio one.
# worker_stall sleeps the provider's worker (completions arrive late; the GPU is
# fine); missing_completion swallows one completion callback (the hung-device
# route). "positive_control" fires at the sample and faults nothing.
AUDIO_FAULTS = ("force_loss_before", "force_loss_between", "force_loss_after",
                "delay_completion", "worker_stall", "missing_completion")
RENDER_FAULTS = ("render_force_loss",)
CONTROL_FAULTS = ("positive_control",)
FAULT_KINDS = AUDIO_FAULTS + RENDER_FAULTS + CONTROL_FAULTS
LOSS_FAULTS = ("force_loss_before", "force_loss_between", "force_loss_after")
STALL_FAULTS = ("delay_completion", "worker_stall", "missing_completion")
# Planted negatives (audio must FAIL) and the path negative (must be
# INCONCLUSIVE: not on the shared-I/O path).
PLANTED = ("no_fallback", "fallback_misaligned", "fallback_stale")
PATH_NEGATIVES = ("gpu_off",)
# M2 delivery requirements.
PRE_FAULT_QUANTA = 96
GPU_SHARE_PASS = 0.9
GPU_SHARE_VOID = 0.5
WINDOW_RMS_FLOOR = 1e-3

# How the fault request reached the plug-in. Only `control_op` is evidence for
# the control surface itself; the in-process file handshake proves the same
# audio claim but says nothing about the control operation.
TRIGGER_TRANSPORTS = ("control_op", "in_process_file", "dry_run")
HEALTH_PHASES = ("before", "at", "after")
VERDICTS = ("pass", "fail", "inconclusive")

SAMPLE_RATE = 48000
CHANNELS = 2
IR_TAPS = 384
DEFAULT_TOLERANCE = 1e-4
DEFAULT_WINDOW_PRE = 4800    # 100 ms before the fault sample
DEFAULT_WINDOW_POST = 48000  # 1 s after it
# The shortest post-fault window a receipt may use: long enough that a lost
# device's silence or a stalled worker's late blocks are inside it. A dry run is
# synthetic and never DAW evidence, so its self-tests may use a shorter one.
MIN_WINDOW_POST = SAMPLE_RATE  # 1 s
MIN_WINDOW_POST_DRY_RUN = 4800  # 100 ms

# Where the plug-in's interposer applies each audio fault.
FAULT_POINTS = {
    "force_loss_before": "before_submit",
    "force_loss_between": "between_submit_and_completion_registration",
    "force_loss_after": "after_completion_registration",
    "delay_completion": "process_events_stall",
    "worker_stall": "process_events_stall",
    "missing_completion": "completion_callback_swallowed",
}

CAPTURE_MAGIC = b"PGFC"
CAPTURE_VERSION = 2
CAPTURE_HEADER = struct.Struct("<4sIIII")  # magic, version, rate, channels, reserved
CAPTURE_RECORD_V1 = struct.Struct("<QII")  # project position, frames, flags
# v2 appends the host time and the transport's cumulative delivery counters
# (gpu, cpu fallback, silence, priming) at the end of the record.
CAPTURE_RECORD = struct.Struct("<QIIQQQQQ")
POSITION_NONE = 0xFFFFFFFFFFFFFFFF
FLAG_PLAYING = 1 << 0
FLAG_OFFLINE = 1 << 1
FLAG_FAULT_BLOCK = 1 << 2
FLAG_SEGMENT_START = 1 << 3
PREPARE_INDEX_SHIFT = 16  # a segment-start record names its prepare (low 16 bits)


# ── Deterministic signals ────────────────────────────────────────────────

def _lcg(seed: int):
    state = seed & 0xFFFFFFFF
    while True:
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        yield (state / 4294967296.0) * 2.0 - 1.0


def make_stimulus(frames: int, sample_rate: int = SAMPLE_RATE, seed: int = 0x5EED) -> list[float]:
    """Noise bursts over a slow sine, so every block has energy and the
    convolution tail never decays to an all-zero window that would null
    trivially. Peaks stay near 0.55 so the convolved output stays well below
    full scale: REAPER's default fixed-point bounce clips at 1.0."""
    noise = _lcg(seed)
    out = []
    for n in range(frames):
        t = n / sample_rate
        burst = 0.125 * next(noise) if (n // 2400) % 2 == 0 else 0.025 * next(noise)
        out.append(0.15 * math.sin(2.0 * math.pi * 220.0 * t) + burst)
    return out


_MASK64 = (1 << 64) - 1


def _splitmix64(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & _MASK64
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & _MASK64
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & _MASK64
    return x ^ (x >> 31)


def internal_stimulus(n: int, rate: int = SAMPLE_RATE) -> float:
    """Sample n (a project position) of the plug-in's internal stimulus,
    rounded to float32 as the plug-in stores it. Mirrors
    examples/gpu-fault-continuity/gpu_fault_stimulus.hpp bit for bit."""
    n &= _MASK64
    u = (_splitmix64(n) >> 11) * (1.0 / 9007199254740992.0)
    noise = 0.125 if (n // 2400) % 2 == 0 else 0.025
    phase = ((220 * n) & _MASK64) % rate / rate
    value = 0.15 * math.sin(2.0 * math.pi * phase) + noise * (2.0 * u - 1.0)
    return struct.unpack("<f", struct.pack("<f", value))[0]


def internal_stimulus_span(start: int, frames: int, rate: int = SAMPLE_RATE) -> list[float]:
    return [internal_stimulus(start + i, rate) for i in range(frames)]


def make_impulse_response(taps: int = IR_TAPS, seed: int = 0x1B5) -> list[float]:
    noise = _lcg(seed)
    ir = [next(noise) * math.exp(-6.0 * k / taps) for k in range(taps)]
    ir[0] = 1.0
    norm = math.sqrt(sum(v * v for v in ir))
    return [v / norm for v in ir]


# ── Files ────────────────────────────────────────────────────────────────

def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_f32(path: Path, samples: Sequence[float]) -> None:
    Path(path).write_bytes(struct.pack(f"<{len(samples)}f", *samples))


def read_f32(path: Path) -> list[float]:
    raw = Path(path).read_bytes()
    if len(raw) % 4:
        raise ValueError(f"{path}: not a float32 stream")
    return list(struct.unpack(f"<{len(raw) // 4}f", raw))


def write_wav_f32(path: Path, channels: Sequence[Sequence[float]],
                  sample_rate: int = SAMPLE_RATE) -> None:
    """IEEE-float WAV (format tag 3); `wave` only writes PCM, so the header is
    written by hand."""
    count = len(channels)
    frames = len(channels[0]) if channels else 0
    interleaved = [channels[c][n] for n in range(frames) for c in range(count)]
    data = struct.pack(f"<{len(interleaved)}f", *interleaved)
    fmt = struct.pack("<HHIIHH", 3, count, sample_rate, sample_rate * count * 4, count * 4, 32)
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
            + b"data" + struct.pack("<I", len(data)) + data)
    Path(path).write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def read_wav(path: Path) -> tuple[int, list[list[float]]]:
    """Read PCM16/24/32 or IEEE-float WAV, including WAVE_FORMAT_EXTENSIBLE,
    which is what REAPER writes for float renders."""
    blob = Path(path).read_bytes()
    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(blob):
        ident = blob[pos:pos + 4]
        size = struct.unpack_from("<I", blob, pos + 4)[0]
        chunk = blob[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
        if ident == b"fmt ":
            fmt = chunk
        elif ident == b"data":
            data = chunk
    if fmt is None or data is None:
        raise ValueError(f"{path}: missing fmt or data chunk")
    tag, count, rate = struct.unpack_from("<HHI", fmt, 0)
    bits = struct.unpack_from("<H", fmt, 14)[0]
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = struct.unpack_from("<H", fmt, 24)[0]
    if tag == 3 and bits == 32:
        flat = list(struct.unpack(f"<{len(data) // 4}f", data))
    elif tag == 1 and bits == 16:
        flat = [v / 32768.0 for v in struct.unpack(f"<{len(data) // 2}h", data)]
    elif tag == 1 and bits == 24:
        flat = [int.from_bytes(data[i:i + 3], "little", signed=True) / 8388608.0
                for i in range(0, len(data) - 2, 3)]
    elif tag == 1 and bits == 32:
        flat = [v / 2147483648.0 for v in struct.unpack(f"<{len(data) // 4}i", data)]
    else:
        raise ValueError(f"{path}: unsupported WAV encoding tag={tag} bits={bits}")
    return rate, [flat[c::count] for c in range(count)]


@dataclass
class Capture:
    """The plug-in's callback stream, flattened in callback order."""
    sample_rate: int
    channels: int
    inputs: list[list[float]]
    outputs: list[list[float]]
    positions: list[int]          # project sample per stream sample, -1 when stopped
    segment_starts: list[int]     # stream indices where the plug-in was (re)prepared
    offline_samples: int = 0
    fault_block_starts: list[int] = field(default_factory=list)
    # REAPER is stopped with a signal once the bounce is written, which can cut
    # the plug-in's last capture write short. That final partial record is
    # dropped and counted here; a short record anywhere else is corruption.
    truncated_tail_bytes: int = 0
    # The prepare index each segment follows (see PREPARE_INDEX_SHIFT).
    segment_prepares: list[int] = field(default_factory=list)
    version: int = CAPTURE_VERSION
    # One entry per record (v2): its first project sample (-1 when stopped),
    # offline flag, host time, and the cumulative delivery counters after it.
    records: list[dict] = field(default_factory=list)

    @property
    def frames(self) -> int:
        return len(self.positions)


def write_capture(path: Path, sample_rate: int, blocks: Sequence[dict],
                  version: int = CAPTURE_VERSION) -> None:
    """Write the plug-in capture format. Each block is
    {"position": int|None, "flags": int, "inputs": [[...]...], "outputs": [[...]...],
     "counters": (gpu, fallback, silence, priming), "host_time_ns": int}.
    Used by the dry run and the self-tests; the plug-in writes the same bytes."""
    channels = len(blocks[0]["inputs"]) if blocks else CHANNELS
    with Path(path).open("wb") as out:
        out.write(CAPTURE_HEADER.pack(CAPTURE_MAGIC, version, sample_rate, channels, 0))
        for block in blocks:
            frames = len(block["inputs"][0])
            position = POSITION_NONE if block.get("position") is None else int(block["position"])
            if version == 1:
                out.write(CAPTURE_RECORD_V1.pack(position, frames, int(block.get("flags", 0))))
            else:
                out.write(CAPTURE_RECORD.pack(position, frames, int(block.get("flags", 0)),
                                              int(block.get("host_time_ns", 0)),
                                              *(int(c) for c in block.get("counters",
                                                                          (0, 0, 0, 0)))))
            for plane in list(block["inputs"]) + list(block["outputs"]):
                out.write(struct.pack(f"<{frames}f", *plane))


def read_capture(path: Path) -> Capture:
    blob = Path(path).read_bytes()
    if len(blob) < CAPTURE_HEADER.size:
        raise ValueError(f"{path}: truncated capture header")
    magic, version, rate, channels, _ = CAPTURE_HEADER.unpack_from(blob, 0)
    if magic != CAPTURE_MAGIC or version not in (1, 2) or not 1 <= channels <= 8:
        raise ValueError(f"{path}: not a v1/v2 GPU fault capture")
    header = CAPTURE_RECORD_V1 if version == 1 else CAPTURE_RECORD
    inputs = [[] for _ in range(channels)]
    outputs = [[] for _ in range(channels)]
    positions: list[int] = []
    segments: list[int] = []
    segment_prepares: list[int] = []
    faults: list[int] = []
    records: list[dict] = []
    offline = 0
    pos = CAPTURE_HEADER.size
    truncated = 0
    while pos < len(blob):
        if pos + header.size > len(blob):
            truncated = len(blob) - pos
            break
        fields = header.unpack_from(blob, pos)
        project, frames, flags = fields[:3]
        need = frames * channels * 2 * 4
        if frames > 1 << 20:
            raise ValueError(f"{path}: implausible record of {frames} frames at byte {pos}")
        if pos + header.size + need > len(blob):
            truncated = len(blob) - pos
            break
        pos += header.size
        planes = struct.unpack_from(f"<{frames * channels * 2}f", blob, pos)
        pos += need
        start = len(positions)
        if flags & FLAG_SEGMENT_START or not segments:
            segments.append(start)
            segment_prepares.append(flags >> PREPARE_INDEX_SHIFT)
        if flags & FLAG_FAULT_BLOCK:
            faults.append(start)
        if flags & FLAG_OFFLINE:
            offline += frames
        for c in range(channels):
            inputs[c].extend(planes[c * frames:(c + 1) * frames])
            outputs[c].extend(planes[(channels + c) * frames:(channels + c + 1) * frames])
        playing = bool(flags & FLAG_PLAYING) and project != POSITION_NONE
        positions.extend((project + i) if playing else -1 for i in range(frames))
        if version >= 2:
            records.append({"stream": start, "position": project if playing else -1,
                            "frames": frames, "offline": bool(flags & FLAG_OFFLINE),
                            "segment_start": bool(flags & FLAG_SEGMENT_START),
                            "host_time_ns": fields[3], "gpu": fields[4], "fallback": fields[5],
                            "silence": fields[6], "priming": fields[7]})
    return Capture(rate, channels, inputs, outputs, positions, segments, offline, faults,
                   truncated, segment_prepares, version, records)


# ── Reference and null ───────────────────────────────────────────────────

def convolve(signal: Sequence[float], ir: Sequence[float]) -> list[float]:
    """Linear convolution truncated to len(signal). Uses numpy when present;
    the pure-Python path keeps the self-tests runnable on a bare interpreter."""
    try:
        import numpy as np
    except ImportError:
        np = None
    n = len(signal)
    if n == 0:
        return []
    if np is not None:
        size = 1 << (n + len(ir) - 1).bit_length()
        spectrum = np.fft.rfft(np.asarray(signal, dtype=np.float64), size) * \
            np.fft.rfft(np.asarray(ir, dtype=np.float64), size)
        return np.fft.irfft(spectrum, size)[:n].tolist()
    out = [0.0] * n
    for k, h in enumerate(ir):
        if h == 0.0:
            continue
        for i in range(k, n):
            out[i] += h * signal[i - k]
    return out


def delayed(signal: Sequence[float], delay: int) -> list[float]:
    if delay < 0:
        raise ValueError("a reference is never advanced, only delayed")
    return [0.0] * min(delay, len(signal)) + list(signal[:max(0, len(signal) - delay)])


def plugin_reference(capture: Capture, ir: Sequence[float],
                     pdcs: Sequence[int]) -> list[list[float]]:
    """conv(input) delayed by PDC, restarted at every (re)prepare, because a
    prepare resets the convolver history and the transport's priming. Segment
    i uses the PDC the plug-in reported in its i-th `prepared` event: a host
    can re-prepare with a different block size, and the quantum follows it."""
    if len(pdcs) != len(capture.segment_starts):
        raise ValueError(f"{len(pdcs)} prepared events for {len(capture.segment_starts)} "
                         "capture segments")
    refs = []
    bounds = capture.segment_starts + [capture.frames]
    for c in range(capture.channels):
        plane: list[float] = []
        for (start, end), pdc in zip(zip(bounds, bounds[1:]), pdcs):
            plane.extend(delayed(convolve(capture.inputs[c][start:end], ir), pdc))
        refs.append(plane)
    return refs


def _finite_or_none(value: float) -> float | None:
    return value if math.isfinite(value) else None


@dataclass
class NullResult:
    max_abs_error: float
    window_max_abs_error: float
    first_deviation: int | None
    last_deviation: int | None
    deviating_samples: int
    window: tuple[int, int]
    compared_samples: int
    reference_rms: float
    candidate_rms: float
    tolerance: float
    non_finite_samples: int = 0
    # The fault window must lie entirely inside the compared frames, and the
    # candidate must be at least `required_frames` long: a short capture or
    # bounce that stops before (or just after) the fault would otherwise null
    # only the audio that was never at risk.
    required_frames: int = 0
    candidate_frames: int = 0

    @property
    def covered(self) -> bool:
        lo, hi = self.window
        return (hi - lo) > 0 and self.candidate_frames >= self.required_frames \
            and not self.window_clamped

    window_clamped: bool = False
    # Over the fault window only: a near-silent stimulus there would let a
    # silence substitution null, so it must carry energy.
    window_reference_rms: float = 0.0

    @property
    def passed(self) -> bool:
        # A near-silent reference over the window is not an audio failure; the
        # run is made INCONCLUSIVE by evaluate_run instead (see S2 there).
        return (self.covered and self.compared_samples > 0
                and self.non_finite_samples == 0 and self.deviating_samples == 0
                and self.max_abs_error <= self.tolerance
                and self.window_max_abs_error <= self.tolerance)

    def as_dict(self) -> dict[str, Any]:
        return {
            "passed": self.passed,
            "tolerance": self.tolerance,
            # JSON has no infinity; a non-finite sample is counted instead.
            "max_abs_error": _finite_or_none(self.max_abs_error),
            "window_max_abs_error": _finite_or_none(self.window_max_abs_error),
            "non_finite_samples": self.non_finite_samples,
            "first_deviating_sample": self.first_deviation,
            "last_deviating_sample": self.last_deviation,
            "deviating_samples": self.deviating_samples,
            "window": list(self.window),
            "compared_samples": self.compared_samples,
            "reference_rms": self.reference_rms,
            "candidate_rms": self.candidate_rms,
            "covered": self.covered,
            "window_reference_rms": self.window_reference_rms,
            "required_frames": self.required_frames,
            "candidate_frames": self.candidate_frames,
        }


def null_test(candidate: Sequence[Sequence[float]], reference: Sequence[Sequence[float]],
              fault_index: int, *, tolerance: float = DEFAULT_TOLERANCE,
              window_pre: int = DEFAULT_WINDOW_PRE, window_post: int = DEFAULT_WINDOW_POST,
              index_labels: Sequence[int] | None = None,
              required_frames: int = 0) -> NullResult:
    """Null `candidate` against `reference` over every sample both cover, and
    separately over [fault - pre, fault + post]. Deviating sample indices are
    reported through `index_labels` (project samples) when given."""
    frames = min(min(len(p) for p in candidate), min(len(p) for p in reference))
    lo = max(0, fault_index - window_pre)
    hi = min(frames, fault_index + window_post)
    clamped = lo != fault_index - window_pre or hi != fault_index + window_post
    worst = window_worst = 0.0
    first = last = None
    deviating = non_finite = 0
    ref_energy = cand_energy = window_energy = 0.0
    for c in range(min(len(candidate), len(reference))):
        cand, ref = candidate[c], reference[c]
        for i in range(frames):
            x, r = cand[i], ref[i]
            err = abs(x - r) if math.isfinite(x) else math.inf
            non_finite += 0 if math.isfinite(x) else 1
            ref_energy += r * r
            cand_energy += x * x if math.isfinite(x) else 0.0
            if err > worst:
                worst = err
            if lo <= i < hi:
                window_energy += r * r
                if err > window_worst:
                    window_worst = err
            if err > tolerance:
                deviating += 1
                label = index_labels[i] if index_labels is not None else i
                first = label if first is None else min(first, label)
                last = label if last is None else max(last, label)
    count = frames * min(len(candidate), len(reference))
    rms = lambda energy: math.sqrt(energy / count) if count else 0.0  # noqa: E731
    return NullResult(worst, window_worst, first, last, deviating, (lo, hi), count,
                      rms(ref_energy), rms(cand_energy), tolerance, non_finite,
                      required_frames, min(len(p) for p in candidate), clamped,
                      math.sqrt(window_energy / ((hi - lo) * min(len(candidate), len(reference))))
                      if hi > lo else 0.0)


def fault_stream_index(capture: Capture, fault_sample: int) -> int | None:
    """Stream index whose project position is the fault sample."""
    for i, p in enumerate(capture.positions):
        if p == fault_sample:
            return i
    return None


def analyze_plugin_capture(capture: Capture, ir: Sequence[float], pdcs: Sequence[int],
                           fault_sample: int, **kw) -> tuple[NullResult | None, str]:
    index = fault_stream_index(capture, fault_sample)
    if index is None:
        return None, f"the capture never played project sample {fault_sample}"
    reference = plugin_reference(capture, ir, pdcs)
    labels = [p if p >= 0 else -1 for p in capture.positions]
    return null_test(capture.outputs, reference, index, index_labels=labels, **kw), ""


def bounce_reference(stimulus: Sequence[Sequence[float]], ir: Sequence[float],
                     frames: int) -> list[list[float]]:
    padded = [list(p) + [0.0] * max(0, frames - len(p)) for p in stimulus]
    return [convolve(p[:frames], ir) for p in padded]


def estimate_lag(candidate: Sequence[float], reference: Sequence[float], max_lag: int) -> int:
    """Diagnostic only: the lag that minimises the null residual. The verdict
    never searches for an alignment; a wrong PDC is reported, not absorbed."""
    best, best_err = 0, math.inf
    span = min(len(candidate), len(reference)) - max_lag
    if span <= 0:
        return 0
    step = max(1, span // 4096)
    for lag in range(-max_lag, max_lag + 1):
        err = 0.0
        for i in range(max_lag, max_lag + span, step):
            j = i - lag
            if 0 <= j < len(reference):
                err += (candidate[i] - reference[j]) ** 2
        if err < best_err:
            best, best_err = lag, err
    return best


# ── Health expectations ──────────────────────────────────────────────────

def gpu_delivered(snapshot: dict) -> int:
    """Quanta the GPU path delivered. The plug-in's snapshot carries the
    transport's delivery counters; a snapshot without them falls back to
    produced_blocks, which on the shared-I/O path may lag delivery."""
    delivery = snapshot.get("delivery")
    if isinstance(delivery, dict) and "gpu_blocks" in delivery:
        return int(delivery["gpu_blocks"])
    return int(snapshot.get("produced_blocks", 0))


def captured_input_matches_stimulus(capture: "Capture", stimulus: Sequence[float] | None,
                                    tolerance: float = 1e-6) -> int | None:
    """M6: over the WHOLE run, on every channel, the plug-in's captured INPUT
    must be exactly the expected stimulus where the transport played, and
    silence everywhere else (stopped blocks, past the item). `stimulus` is the
    bound stimulus.wav samples; None means the plug-in's internal stimulus.
    Returns the first offending stream index, or None. Anything else in the
    input (a live input, a microphone) is a mismatch."""
    for c in range(capture.channels):
        plane = capture.inputs[c]
        for i, p in enumerate(capture.positions):
            if p < 0:
                want = 0.0
            elif stimulus is None:
                want = internal_stimulus(p, capture.sample_rate or SAMPLE_RATE)
            else:
                want = stimulus[p] if p < len(stimulus) else 0.0
            if abs(plane[i] - want) > tolerance:
                return i
    return None


def applied_project_sample(capture: "Capture", applied: dict | None) -> int | None:
    """Where the fault actually hit: the first played record whose callback
    started at or after the interposer's `host_time_ns` (the same steady clock
    the capture records). Falls back to the service thread's `project_sample`,
    which is observed later and so is a bound, when the time is absent."""
    if not applied:
        return None
    t = applied.get("host_time_ns")
    if isinstance(t, int) and t > 0 and capture.version >= 2:
        hit = next((r["position"] for r in capture.records
                    if r["position"] >= 0 and r["host_time_ns"] >= t), None)
        if hit is not None:
            return hit
    return applied.get("project_sample")


def delivery_disposition(capture: "Capture", fault_sample: int,
                         applied_project_sample: int | None, lead_blocks: int, *,
                         fault: str = "force_loss_after", quantum: int | None = None,
                         window_post: int = DEFAULT_WINDOW_POST) -> dict[str, Any]:
    """WP-K's view of delivery_analysis, under the names its v2 verifier reads.
    The quantum defaults to the capture's own record length."""
    if quantum is None:
        quantum = next((r["frames"] for r in capture.records if r["position"] >= 0), 1)
    d = delivery_analysis(capture, fault=fault, fault_sample=fault_sample,
                          applied_sample=applied_project_sample, quantum=quantum,
                          lead_blocks=lead_blocks, window_post=window_post)
    first = d.get("first_fallback_project_sample")
    applied = applied_project_sample if applied_project_sample is not None else fault_sample
    return {**d, "pre_window_gpu_share": d.get("pre_fault_gpu_share"),
            "pre_window_quanta": d.get("pre_fault_quanta"),
            "gpu_frozen_after_applied": d.get("available", False)
            and d.get("gpu_blocks_after_takeover") == 0 and first is not None,
            "all_fallback_after": d.get("available", False) and first is not None
            and d.get("gpu_blocks_after_takeover") == 0 and d.get("silence_blocks") == 0,
            "silence_blocks_delta": d.get("silence_blocks"),
            "first_fallback_quanta_after_applied":
                None if first is None else (first - applied) // max(1, quantum),
            "lead_blocks": lead_blocks}


def health_expectation(fault: str, snapshots: dict[str, dict]) -> tuple[bool, str]:
    """Did the fault take effect, as seen by the transport's own snapshots?
    A null that passes because the fault never landed is not evidence. The
    per-block delivery stream (delivery_analysis) is the stronger check; this
    one stays as the before/at/after cross-check."""
    missing = [p for p in HEALTH_PHASES if p not in snapshots]
    if missing:
        return False, "missing health snapshots: " + ", ".join(missing)
    before, at, after = snapshots["before"], snapshots["at"], snapshots["after"]
    if before.get("provider_state") != "ready":
        return False, f"provider was not ready before the fault ({before.get('provider_state')})"
    if gpu_delivered(before) <= 0:
        # Every block was already CPU fallback, so the fault could only test
        # the fallback. REAPER's anticipative FX processing does exactly this.
        return False, "the GPU path delivered no blocks before the fault"
    fallback_delta = int(after.get("fallback_blocks", 0)) - int(before.get("fallback_blocks", 0))
    gpu_delta = gpu_delivered(after) - gpu_delivered(before)
    if fault in LOSS_FAULTS:
        # A lost device delivers nothing more, whatever reason the transport
        # names: the GPU count must freeze between "at" and "after".
        if fallback_delta <= 0:
            return False, f"{fault}: no fallback blocks after the loss"
        if gpu_delivered(after) != gpu_delivered(at):
            return False, f"{fault}: the GPU kept delivering after the loss"
        return True, (f"loss observed; the GPU stopped delivering and {fallback_delta} fallback "
                      f"blocks covered it; recovery_reason={after.get('recovery_reason')!r}")
    if fault in STALL_FAULTS:
        # Tied to this transport: its fallback must already have taken over by
        # the "at" snapshot, taken just after the stall was applied.
        at_delta = int(at.get("fallback_blocks", 0)) - int(before.get("fallback_blocks", 0))
        if at_delta <= 0 or fallback_delta <= 0:
            return False, f"{fault}: this transport's fallback did not take over at the stall"
        return True, f"stall observed; {fallback_delta} fallback blocks covered it"
    # render_force_loss and the positive control: audio must not move.
    what = "render loss" if fault in RENDER_FAULTS else "positive control"
    if after.get("provider_state") != "ready" or after.get("recovery_reason", "none") != "none":
        return False, (f"{what} disturbed the audio provider: state="
                       f"{after.get('provider_state')!r} reason={after.get('recovery_reason')!r}")
    if gpu_delta <= 0:
        return False, f"{what}: the audio GPU stopped delivering blocks"
    return True, (f"audio provider unaffected by the {what}; the GPU delivered "
                  f"{gpu_delta} more blocks after it")


def delivery_analysis(capture: "Capture", *, fault: str, fault_sample: int,
                      applied_sample: int | None, quantum: int, lead_blocks: int,
                      window_post: int) -> dict[str, Any]:
    """Per-block delivery from the v2 capture's cumulative counters.

    Because the CPU fallback is sample-exact against the GPU path, the null
    cannot tell GPU blocks from fallback blocks; only these counters can. For a
    PASS: the GPU carried at least GPU_SHARE_PASS of the PRE_FAULT_QUANTA quanta
    before the fault (between GPU_SHARE_VOID and that is a degraded host; below
    it the run is void); no block was ever silence; and after the fault the
    fallback took over within lead_blocks + 1 quanta of where it was applied,
    with GPU delivery frozen for a loss. A render loss or the control must leave
    the GPU delivering."""
    if capture.version < 2 or not capture.records:
        return {"available": False, "met": False,
                "note": "the capture carries no per-block delivery counters"}
    rows, prev = [], None
    for r in capture.records:
        if r["segment_start"] or prev is None:
            prev = {"gpu": 0, "fallback": 0, "silence": 0}
        rows.append({"position": r["position"], "frames": r["frames"],
                     "gpu": r["gpu"] - prev["gpu"], "fallback": r["fallback"] - prev["fallback"],
                     "silence": r["silence"] - prev["silence"]})
        prev = r
    playing = [r for r in rows if r["position"] >= 0]
    silence = sum(r["silence"] for r in playing)
    pre_lo = fault_sample - PRE_FAULT_QUANTA * quantum
    pre = [r for r in playing if pre_lo <= r["position"] < fault_sample]
    pre_gpu = sum(r["gpu"] for r in pre)
    pre_total = pre_gpu + sum(r["fallback"] + r["silence"] for r in pre)
    share = pre_gpu / pre_total if pre_total else 0.0
    post = [r for r in playing if fault_sample <= r["position"] < fault_sample + window_post]
    first_fallback = next((r["position"] for r in post if r["fallback"] > 0), None)
    applied_ref = max(fault_sample, applied_sample if applied_sample is not None else fault_sample)
    deadline = applied_ref + (lead_blocks + 1) * quantum
    after_first = [r for r in post if first_fallback is not None and r["position"] > first_fallback]
    gpu_after_takeover = sum(r["gpu"] for r in after_first)
    post_gpu = sum(r["gpu"] for r in post)
    post_total = post_gpu + sum(r["fallback"] + r["silence"] for r in post)
    out = {"available": True, "pre_fault_quanta": pre_total, "pre_fault_gpu_share": share,
           "degraded_host": GPU_SHARE_VOID <= share < GPU_SHARE_PASS,
           "void": share < GPU_SHARE_VOID, "silence_blocks": silence,
           "first_fallback_project_sample": first_fallback,
           "takeover_deadline_project_sample": deadline,
           "gpu_blocks_after_takeover": gpu_after_takeover,
           "post_fault_gpu_share": post_gpu / post_total if post_total else 0.0}
    problems = []
    if out["void"]:
        problems.append(f"the GPU carried {share:.0%} of the {pre_total} quanta before the "
                        f"fault (void below {GPU_SHARE_VOID:.0%})")
    if fault in LOSS_FAULTS or fault in STALL_FAULTS:
        if first_fallback is None:
            problems.append("no fallback block followed the fault")
        elif first_fallback > deadline:
            problems.append(f"the fallback took over at {first_fallback}, later than "
                            f"{deadline} (lead_blocks + 1 quanta after the fault was applied)")
        if fault in LOSS_FAULTS and gpu_after_takeover:
            problems.append(f"the GPU delivered {gpu_after_takeover} blocks after a loss")
    elif out["post_fault_gpu_share"] < GPU_SHARE_VOID:
        problems.append(f"the GPU carried only {out['post_fault_gpu_share']:.0%} after the "
                        f"{fault}")
    out["met"] = not problems
    out["note"] = "; ".join(problems) or (
        f"GPU share {share:.0%} before the fault; " +
        ("fallback took over at " + str(first_fallback) if first_fallback is not None
         else "GPU kept delivering") + "; no silence")
    return out


# ── Receipt ──────────────────────────────────────────────────────────────

def decide(*, fault: str, plugin_null: dict | None, bounce_null: dict | None,
           health_ok: bool, fault_fired: bool, host_alive: bool,
           realtime_at_fault: bool, preconditions: Sequence[str] = (),
           mismatches: Sequence[str] = (), realtime_null: dict | None = None,
           delivery: dict | None = None,
           invalid_input: Sequence[str] = ()) -> tuple[str, list[str]]:
    """Order matters. A host crash, evidence that does not belong to this run
    (`mismatches`), and a computed null that failed are all FAIL, even when a
    precondition also failed, so a broken run is never softened to
    INCONCLUSIVE. `preconditions` (not on the shared-I/O path, a dropped
    capture record, a different IR, a refused fault) make an otherwise clean
    run INCONCLUSIVE, and are also listed after the cause of a FAIL.

    PASS needs three nulls: the plug-in's own capture, the host's realtime
    render of the fault pass (the primary host-side evidence), and the host's
    offline bounce (an offline-path/PDC cross-check); plus the per-block
    delivery requirements in `delivery`."""
    reasons: list[str] = []
    if not host_alive:
        # A crash on render loss is the h2 finding the plan asks to record, not hide.
        return "fail", ["host process died during the run"]
    if invalid_input:
        # The plug-in's input was not the stimulus (host silence, or foreign
        # audio): nothing about continuity can be concluded, and the verifier
        # refuses the receipt outright (M6).
        return "inconclusive", list(invalid_input) + list(preconditions)
    if mismatches:
        return "fail", list(mismatches) + list(preconditions)
    for name, result in (("plugin", plugin_null), ("host realtime render", realtime_null),
                         ("bounce", bounce_null)):
        if result is None:
            reasons.append(f"{name} null not computed")
        elif not result["passed"]:
            if not result.get("covered", True):
                why = "did not cover the fault window"
            elif result.get("window_reference_rms", 1.0) < WINDOW_RMS_FLOOR:
                why = "the reference is near-silent over the fault window"
            else:
                why = f"max_abs_error={result['max_abs_error']}"
            return "fail", [f"{name} null failed: {why} "
                            f"first={result['first_deviating_sample']} "
                            f"last={result['last_deviating_sample']}"] + list(preconditions)
    if preconditions:
        return "inconclusive", list(preconditions)
    if not fault_fired:
        return "inconclusive", ["the fault never fired"]
    if reasons:
        return "inconclusive", reasons
    if not realtime_at_fault:
        return "inconclusive", ["the fault landed on an offline block; the realtime path was not exercised"]
    if not health_ok:
        return "inconclusive", ["the fault did not register in the transport's health counters"]
    if delivery is not None and not delivery.get("met"):
        return "inconclusive", [f"per-block delivery: {delivery.get('note')}"]
    if delivery is not None and delivery.get("degraded_host"):
        return "pass", [f"degraded host: the GPU carried {delivery['pre_fault_gpu_share']:.0%} "
                        "of the quanta before the fault"]
    return "pass", []


def build_receipt(**fields: Any) -> dict[str, Any]:
    receipt = {"schema": SCHEMA}
    receipt.update(fields)
    body = json.dumps({k: v for k, v in receipt.items() if k != "receipt_sha256"},
                      sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    receipt["receipt_sha256"] = hashlib.sha256(body).hexdigest()
    return receipt


def receipt_digest(receipt: dict[str, Any]) -> str:
    body = json.dumps({k: v for k, v in receipt.items() if k != "receipt_sha256"},
                      sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    return hashlib.sha256(body).hexdigest()


# ── Dry run ──────────────────────────────────────────────────────────────

def synthesize_run(out_dir: Path, *, fault: str, fault_sample: int, frames: int,
                   block: int = 128, quantum: int = 128, lead_blocks: int = 2,
                   break_fallback: bool = False, nonce: str = "dry-run",
                   pid: int = 0, tail: int = 0, window_post: int | None = None,
                   planted: str | None = None) -> dict[str, Any]:
    """Write the artifacts a real run would produce, from the reference
    itself, so the analysis and the verifier can be exercised without REAPER.

    `planted` selects a negative, shaped like the plug-in's own:
    no_fallback (silence after the fault; `break_fallback` is the old name),
    fallback_misaligned (one quantum late after the fault), fallback_stale (one
    quantum of input withheld at the fault), or gpu_off (the staged path)."""
    if break_fallback:
        planted = "no_fallback"
    out_dir.mkdir(parents=True, exist_ok=True)
    pdc = (lead_blocks + 1) * quantum
    stim = make_stimulus(frames)
    ir = make_impulse_response()
    stimulus = [stim] * CHANNELS
    write_wav_f32(out_dir / "stimulus.wav", stimulus)
    write_f32(out_dir / "ir.f32", ir)

    # Plug-in stream: a few stopped blocks of silence, then the played item and
    # `tail` samples past it, as the harness plays past the fault window.
    lead_in = 4 * block
    stream_in = [0.0] * lead_in + stim + [0.0] * tail
    fault_stream = lead_in + fault_sample
    # PDC samples past the stream, so the host's PDC-compensated render of the
    # whole played range exists.
    node_in = list(stream_in) + [0.0] * (pdc + 1)
    if planted == "fallback_stale":
        q0 = (fault_stream // quantum) * quantum
        node_in[q0:q0 + quantum] = [0.0] * quantum
    wet = delayed(convolve(node_in, ir), pdc)
    if planted == "no_fallback":
        wet = [0.0 if i >= fault_stream else v for i, v in enumerate(wet)]
    elif planted == "fallback_misaligned":
        wet = [wet[i - quantum] if i >= fault_stream + quantum else
               (0.0 if i >= fault_stream else wet[i]) for i in range(len(wet))]

    audio_fault = fault in AUDIO_FAULTS
    takeover = fault_sample + block  # the fallback carries from the next block
    gpu = fallback = 0
    blocks = []
    for start in range(0, len(stream_in), block):
        end = min(start + block, len(stream_in))
        playing = start >= lead_in
        position = start - lead_in if playing else None
        if planted == "gpu_off" or (audio_fault and playing and position >= takeover):
            fallback += 1
        else:
            gpu += 1
        flags = (FLAG_PLAYING if playing else 0) | (
            (FLAG_SEGMENT_START | (1 << PREPARE_INDEX_SHIFT)) if start == 0 else 0)
        if start <= fault_stream < end:
            flags |= FLAG_FAULT_BLOCK
        blocks.append({"position": position, "flags": flags,
                       "inputs": [stream_in[start:end]] * CHANNELS,
                       "outputs": [wet[start:end]] * CHANNELS,
                       "host_time_ns": start * 20833, "counters": (gpu, fallback, 0, 2)})
    write_capture(out_dir / "capture.bin", SAMPLE_RATE, blocks)

    # The host's realtime render of the fault pass: the plug-in's output with
    # the PDC the host compensates removed. The offline bounce is a separate,
    # fault-free render, so it is always the clean reference.
    played = len(stim) + tail
    # One sample longer than the bounce, as a separate render of its own: the
    # verifier treats a realtime render byte-identical to the bounce as a copy.
    realtime = wet[lead_in + pdc:lead_in + pdc + played + 1]
    realtime += [0.0] * (played + 1 - len(realtime))
    write_wav_f32(out_dir / "realtime.wav", [realtime] * CHANNELS)
    (out_dir / "realtime_render.json").write_text(json.dumps({
        "artifact": "realtime.wav", "mode": "realtime", "host": "dry_run",
        "sample_rate": SAMPLE_RATE, "started_unix_us": 0,
        "finished_unix_us": int(played / SAMPLE_RATE * 1e6)}) + "\n", encoding="utf-8")
    write_wav_f32(out_dir / "bounce.wav", [convolve(stim + [0.0] * tail, ir)] * CHANNELS)

    def status(produced: int, fallback_blocks: int) -> dict[str, Any]:
        return {"schema_version": 3, "provider_state": "ready", "recovery_reason": "none",
                "produced_blocks": produced, "missed_blocks": 0,
                "fallback_blocks": fallback_blocks, "recovery_sequence": None,
                "provider": "dawn", "latency_samples": pdc,
                "delivery": {"gpu_blocks": produced, "cpu_fallback_blocks": fallback_blocks,
                             "priming_blocks": 2, "silence_blocks": 0},
                "source": "plugin_status_snapshot"}

    # Shaped like this base's real snapshots: a fault freezes GPU delivery and
    # recovery_reason is not named for the convolver.
    pre = 100
    health = {
        "before": status(pre, 0),
        "at": status(pre + 1, 1) if audio_fault else status(pre + 50, 0),
        "after": status(pre + 1, 250) if audio_fault else status(pre + 400, 0),
    }
    if fault in LOSS_FAULTS:
        # Batch A's truthful status: a lost device reads unavailable/provider-lost.
        for phase in ("at", "after"):
            health[phase].update(provider_state="unavailable", recovery_reason="provider-lost")
    device = "0x1d2e3f"
    events = [
        {"event": "prepared", "ok": True, "pdc_samples": pdc, "quantum": quantum,
         "lead_blocks": lead_blocks, "ir_taps": len(ir), "provider": "dawn",
         "path": "staged" if planted == "gpu_off" else "shared_memory", "nonce": nonce,
         "pid": pid, "prepare_index": 1, "provider_device": device, "node": "convolver",
         "storage": "staged" if planted == "gpu_off" else "shared", "timestamps": False},
        {"event": "fault_armed", "fault": fault, "at_project_sample": fault_sample,
         "after_project_sample": fault_sample + (tail if window_post is None else window_post),
         "planted_negative": planted if planted in PLANTED + PATH_NEGATIVES else None,
         "input": "host",
         "gpu_off": planted == "gpu_off"},
        {"event": "fault_fired", "fault": fault, "at_project_sample": fault_sample,
         "issued_project_sample": fault_sample + block, "offline": False},
    ]
    if fault in RENDER_FAULTS:
        events.append({"event": "render_loss", "performed": True, "detail": "dry run"})
    elif audio_fault:
        events.append({"event": "provider_fault_applied", "point": FAULT_POINTS[fault],
                       "project_sample": fault_sample + block, "device": device,
                       "prepare_index": 1,
                       "host_time_ns": (lead_in + fault_sample + block) * 20833})
    events += [{"event": "health", "phase": p, "status": st} for p, st in health.items()]
    with (out_dir / "events.jsonl").open("w", encoding="utf-8") as stream:
        for event in events:
            stream.write(json.dumps(event, sort_keys=True) + "\n")
    return {"pdc": pdc, "events": events}


def load_events(path: Path) -> list[dict[str, Any]]:
    events = []
    if not Path(path).exists():
        return events
    for line in Path(path).read_text(errors="replace", encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            events.append(value)
    return events


def first_event(events: Sequence[dict], name: str) -> dict | None:
    return next((e for e in events if e.get("event") == name), None)


def health_snapshots(events: Sequence[dict]) -> dict[str, dict]:
    snaps: dict[str, dict] = {}
    for event in events:
        if event.get("event") == "health" and event.get("phase") in HEALTH_PHASES:
            snaps.setdefault(event["phase"], event.get("status") or {})
    return snaps


def summarize_preflight(doc: dict[str, Any]) -> dict[str, Any]:
    """The receipt's preflight summary, derived from preflight.json. The
    verifier re-derives it, so a receipt cannot claim a quieter host than the
    preflight it carries."""
    return {"status": doc.get("status"), "quiet_host": doc.get("quiet_host") is True,
            "reasons": [str(r) for r in doc.get("reasons", [])],
            "load1": doc.get("load1"), "ncpu": doc.get("ncpu"),
            "raw_observations_sha256": doc.get("raw_observations_sha256")}


def evaluate_run(run_dir: Path, *, fault: str, fault_sample: int, tolerance: float,
                 window_pre: int, window_post: int, host_alive: bool,
                 bounce_path: Path | None, render_frames: int = 0,
                 nonce: str | None = None, reaper_pid: int | None = None,
                 listed: Sequence[str] | None = None,
                 realtime_path: Path | None = None,
                 host_quiet: bool | None = None) -> dict[str, Any]:
    """Analyse a finished run directory and return the receipt's evidence
    fields (everything except identity and preflight).

    `listed` restricts the analysis to digest-bound artifacts: a file that is
    present but not listed is treated as absent. `render_frames` is how long
    the bounce must be. `nonce` and `reaper_pid` tie the plug-in's events to
    this run and this REAPER process. `realtime_path` is the host's realtime
    render of the fault pass, nulled like the bounce."""
    def usable(name: str) -> bool:
        return (run_dir / name).is_file() and (listed is None or name in listed)

    events = load_events(run_dir / "events.jsonl") if usable("events.jsonl") else []
    prepares = [e for e in events if e.get("event") == "prepared"]
    prepared = prepares[0] if prepares else {}
    armed = first_event(events, "fault_armed")
    fired = first_event(events, "fault_fired")
    pdc = int(prepared.get("pdc_samples", -1))
    ir = read_f32(run_dir / "ir.f32") if usable("ir.f32") else []
    _, stimulus = read_wav(run_dir / "stimulus.wav") if usable("stimulus.wav") else (0, [[]])
    kw = {"tolerance": tolerance, "window_pre": window_pre, "window_post": window_post}

    preconditions: list[str] = []
    mismatches: list[str] = []
    invalid_input: list[str] = []
    # Every event the verdict rests on must be about this fault and sample.
    for name, event in (("fault_armed", armed), ("fault_fired", fired)):
        if event is None:
            continue
        if event.get("fault") != fault:
            mismatches.append(f"{name} names fault {event.get('fault')!r}, the run asked for "
                              f"{fault!r}")
        if event.get("at_project_sample") != fault_sample:
            mismatches.append(f"{name} is at project sample {event.get('at_project_sample')}, "
                              f"the run asked for {fault_sample}")
    # The window the receipt analyses must be the one the plug-in was asked to
    # observe, so a receipt cannot shrink it after the fact.
    if armed is not None and armed.get("after_project_sample") != fault_sample + window_post:
        mismatches.append(f"fault_armed observes until {armed.get('after_project_sample')}, the "
                          f"analysis window ends at {fault_sample + window_post}")
    applied_event = first_event(events, "provider_fault_applied")
    if applied_event is not None and applied_event.get("point") != FAULT_POINTS.get(fault):
        mismatches.append(f"the fault was applied at {applied_event.get('point')!r}, not where "
                          f"{fault} is applied ({FAULT_POINTS.get(fault)!r})")
    if nonce is not None and any(e.get("nonce") != nonce for e in prepares):
        mismatches.append("the plug-in's events carry a different run nonce")
    # The run directory's claim names the process that owns it; it must be the
    # one every prepared event came from.
    if usable("claim"):
        # "<pid>\n" or "<pid>\n<claimed_unix_us>\n"
        claim_pid = (run_dir / "claim").read_text(encoding="utf-8").splitlines()[0].strip()
        if any(str(e.get("pid")) != claim_pid for e in prepares):
            # Typically a plug-in scanner won the claim: the test did not run in
            # the intended process, which says nothing about the audio (M5).
            preconditions.append("unbound: the run directory was claimed by a different "
                                 "process than the one that prepared")
    if reaper_pid is not None and any(e.get("pid") != reaper_pid for e in prepares):
        mismatches.append("the plug-in's events come from a different process than the REAPER "
                          "this run started")

    capture = None
    plugin_null, plugin_note = None, "no capture"
    owning: list[dict] = []
    overflow = first_event(events, "capture_overflow") is not None
    if overflow:
        # A dropped record splices the stream, so a null over it would blame
        # the audio for a harness-side drop. No plug-in null is computed.
        plugin_note = "capture records were dropped; no plug-in null"
    if usable("capture.bin") and prepares and not overflow:
        try:
            capture = read_capture(run_dir / "capture.bin")
        except ValueError as exc:
            # A corrupt record is a harness-side failure, never an audio FAIL.
            capture = None
            plugin_note = f"capture unreadable ({exc}); no plug-in null"
            preconditions.append("the plug-in capture is corrupt")
    if capture is not None:
        # Pair each capture segment with the prepare it follows. A host may
        # prepare several times before processing again; only the last of
        # those owns audio.
        by_index = {e.get("prepare_index"): e for e in prepares}
        owning = [by_index.get(i) for i in capture.segment_prepares]
        complete = all(e is not None for e in owning)
        owning = [e for e in owning if e is not None]
        if owning:
            # Identity, PDC and quantum come from the prepare that owns the
            # audio, not from a prepare the host discarded unused.
            prepared = owning[-1]
            pdc = int(prepared.get("pdc_samples", -1))
        if not complete:
            preconditions.append("a capture segment has no matching prepared event")
        else:
            result, plugin_note = analyze_plugin_capture(
                capture, ir, [int(e.get("pdc_samples", -1)) for e in owning], fault_sample,
                **kw)
            plugin_null = result.as_dict() if result else None

    if (armed or {}).get("input") == "internal" and render_frames:
        # The plug-in ignored the track item and rendered its own stimulus.
        stimulus = [internal_stimulus_span(0, render_frames)] * CHANNELS
    realtime_null = None
    if realtime_path is not None and usable(realtime_path.name):
        _, realtime_audio = read_wav(realtime_path)
        result = null_test(realtime_audio, bounce_reference(stimulus, ir, len(realtime_audio[0])),
                           fault_sample, required_frames=render_frames, **kw)
        realtime_null = result.as_dict()

    bounce_null, bounce_lag = None, None
    if bounce_path is not None and usable(bounce_path.name):
        _, bounce = read_wav(bounce_path)
        reference = bounce_reference(stimulus, ir, len(bounce[0]))
        result = null_test(bounce, reference, fault_sample, required_frames=render_frames, **kw)
        bounce_null = result.as_dict()
        if not result.passed and result.covered:
            bounce_lag = estimate_lag(bounce[0], reference[0], max(64, pdc + 64))

    snaps = health_snapshots(events)
    health_ok, health_note = health_expectation(fault, snaps)
    # S2: a stimulus that is near-silent over the fault window would let a
    # silence substitution null, and a host that fed the plug-in silence
    # instead of the stimulus tested nothing. Either makes the run INCONCLUSIVE.
    for name, result in (("plug-in", plugin_null), ("host realtime render", realtime_null)):
        if result is not None and result["window_reference_rms"] < WINDOW_RMS_FLOOR:
            preconditions.append(f"the {name} reference is near-silent over the fault window "
                                 "(the stimulus did not reach the plug-in)")
    if capture is not None and stimulus and stimulus[0]:
        internal = (armed or {}).get("input") == "internal"
        bad = captured_input_matches_stimulus(capture, None if internal else stimulus[0])
        if bad is not None:
            # The harness reports it as INCONCLUSIVE; the verifier refuses the
            # receipt outright (see verify()), because it may hold foreign audio.
            invalid_input.append(f"invalid input: the plug-in's input was not the stimulus "
                                 f"(first difference at stream sample {bad})")
    # The quiet-host rule: a live run on a host whose preflight was not quiet is
    # INCONCLUSIVE and never upgraded (None: not a live run, rule not applied).
    if host_quiet is False:
        preconditions.append("host_not_quiet: the host preflight did not admit a quiet host")
    # M1 provenance: the realtime render must have taken realtime and differ
    # from the bounce, or it is not evidence of a realtime pass.
    if usable("realtime_render.json") and realtime_null is not None:
        try:
            prov = json.loads((run_dir / "realtime_render.json").read_text(encoding="utf-8"))
        except ValueError:
            prov = {}
        took = (int(prov.get("finished_unix_us", 0)) - int(prov.get("started_unix_us", 0))) / 1e6
        audio_s = render_frames / SAMPLE_RATE if render_frames else 0.0
        if prov.get("mode") != "realtime" or prov.get("artifact") != realtime_path.name:
            preconditions.append("realtime_render.json does not describe the realtime render")
        elif audio_s and took < 0.9 * audio_s:
            preconditions.append(f"the realtime render took {took:.1f} s for {audio_s:.1f} s of "
                                 "audio; it was not a realtime pass")
        if bounce_path is not None and usable(bounce_path.name) and \
                sha256_file(realtime_path) == sha256_file(bounce_path):
            preconditions.append("the realtime render is byte-identical to the bounce")
    elif realtime_null is not None and host_quiet is not None:
        preconditions.append("the realtime render has no provenance (realtime_render.json)")
    applied_event = first_event(events, "provider_fault_applied")
    delivery = None
    if capture is not None:
        delivery = delivery_analysis(
            capture, fault=fault, fault_sample=fault_sample,
            applied_sample=applied_project_sample(capture, applied_event),
            quantum=int(prepared.get("quantum", 0) or 0) or 1,
            lead_blocks=int(prepared.get("lead_blocks", 0) or 0), window_post=window_post)
        if delivery.get("silence_blocks"):
            mismatches.append(f"the transport delivered {delivery['silence_blocks']} silence "
                              "blocks")
        if delivery.get("void"):
            preconditions.append(delivery["note"])
    # The binding of the applied fault to this instance's provider device.
    if applied_event is not None and prepares and \
            applied_event.get("device") not in {e.get("provider_device") for e in prepares}:
        mismatches.append("the fault was applied to a device that is not this plug-in's provider")
    if armed is not None and armed.get("gpu_off"):
        preconditions.append("path negative: the plug-in ran with the GPU path off (gpu_off)")
    realtime = bool(fired) and not bool(fired.get("offline", True))
    render_loss = first_event(events, "render_loss")
    applied = first_event(events, "provider_fault_applied")
    # Armed is not fired, and fired is not applied: an audio fault counts only
    # once the interposer reports it faulted the provider, a render fault only
    # once the editor reports ForceLoss was issued.
    took_effect = fired is not None and (
        bool(render_loss and render_loss.get("performed")) if fault in RENDER_FAULTS
        else True if fault in CONTROL_FAULTS else applied is not None)
    audio_prepares = owning or prepares
    if any(e.get("ok") is not True for e in audio_prepares) or not prepares:
        preconditions.append("the plug-in did not prepare its GPU engine every time")
    if any(e.get("path") != "shared_memory" for e in audio_prepares):
        preconditions.append(f"the plug-in is not on the shared-I/O GPU path "
                             f"(paths={sorted({str(e.get('path')) for e in audio_prepares})})")
    if any(e.get("ir_taps") != len(ir) for e in audio_prepares):
        preconditions.append(f"the plug-in loaded {prepared.get('ir_taps')} IR taps, "
                             f"the harness wrote {len(ir)}")
    if first_event(events, "capture_overflow") is not None:
        preconditions.append("the plug-in dropped capture records")
    refused = first_event(events, "fault_refused")
    if refused is not None:
        preconditions.append(f"the plug-in refused the fault: "
                             f"{refused.get('reason') or refused.get('detail')}")
    verdict, reasons = decide(fault=fault, plugin_null=plugin_null, bounce_null=bounce_null,
                              health_ok=health_ok, fault_fired=took_effect,
                              host_alive=host_alive, realtime_at_fault=realtime,
                              preconditions=preconditions, mismatches=mismatches,
                              realtime_null=realtime_null, delivery=delivery,
                              invalid_input=invalid_input)
    if plugin_note:
        reasons.append(plugin_note)
    return {
        "fault": {
            "kind": fault,
            "requested_project_sample": fault_sample,
            "fired": fired is not None,
            "fired_project_sample": fired.get("at_project_sample") if fired else None,
            "issued_project_sample": fired.get("issued_project_sample") if fired else None,
            "realtime_block": realtime,
            "render_loss": render_loss,
            "provider_fault_applied": applied,
        },
        "pdc_samples": pdc,
        "plugin_identity": {k: prepared.get(k) for k in ("provider", "path", "quantum",
                                                          "lead_blocks", "ir_taps")},
        "capture": {
            "frames": capture.frames if capture else 0,
            "segments": len(capture.segment_starts) if capture else 0,
            "offline_samples": capture.offline_samples if capture else 0,
            "truncated_tail_bytes": capture.truncated_tail_bytes if capture else 0,
        },
        "delivery": delivery,
        "controls": {"planted_negative": armed.get("planted_negative") if armed else None,
                     "input": armed.get("input") if armed else None,
                     "gpu_off": bool(armed.get("gpu_off")) if armed else False},
        # The bounce comes from a fresh, unclaimed instance that records nothing,
        # so whether REAPER flagged its blocks offline is not observable here.
        "offline_records_in_bounce": None,
        "offline_path_checked": False,
        "nulls": {"plugin": plugin_null, "host_realtime": realtime_null, "bounce": bounce_null,
                  "bounce_lag_estimate": bounce_lag},
        "health": {"snapshots": snaps, "expectation_met": health_ok, "note": health_note},
        "host_alive": host_alive,
        "verdict": verdict,
        "verdict_reasons": reasons,
    }


def artifact_digests(run_dir: Path, names: Sequence[str]) -> dict[str, str]:
    return {name: sha256_file(run_dir / name) for name in names if (run_dir / name).exists()}
