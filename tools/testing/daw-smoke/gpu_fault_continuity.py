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
AUDIO_FAULTS = ("force_loss_before", "force_loss_between", "force_loss_after",
                "delay_completion", "hang")
RENDER_FAULTS = ("render_force_loss",)
FAULT_KINDS = AUDIO_FAULTS + RENDER_FAULTS
LOSS_FAULTS = ("force_loss_before", "force_loss_between", "force_loss_after")

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

CAPTURE_MAGIC = b"PGFC"
CAPTURE_VERSION = 1
CAPTURE_HEADER = struct.Struct("<4sIIII")  # magic, version, rate, channels, reserved
CAPTURE_RECORD = struct.Struct("<QII")     # project position, frames, flags
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

    @property
    def frames(self) -> int:
        return len(self.positions)


def write_capture(path: Path, sample_rate: int, blocks: Sequence[dict]) -> None:
    """Write the plug-in capture format. Each block is
    {"position": int|None, "flags": int, "inputs": [[...]...], "outputs": [[...]...]}.
    Used by the dry run and the self-tests; the plug-in writes the same bytes."""
    channels = len(blocks[0]["inputs"]) if blocks else CHANNELS
    with Path(path).open("wb") as out:
        out.write(CAPTURE_HEADER.pack(CAPTURE_MAGIC, CAPTURE_VERSION, sample_rate, channels, 0))
        for block in blocks:
            frames = len(block["inputs"][0])
            position = POSITION_NONE if block.get("position") is None else int(block["position"])
            out.write(CAPTURE_RECORD.pack(position, frames, int(block.get("flags", 0))))
            for plane in list(block["inputs"]) + list(block["outputs"]):
                out.write(struct.pack(f"<{frames}f", *plane))


def read_capture(path: Path) -> Capture:
    blob = Path(path).read_bytes()
    if len(blob) < CAPTURE_HEADER.size:
        raise ValueError(f"{path}: truncated capture header")
    magic, version, rate, channels, _ = CAPTURE_HEADER.unpack_from(blob, 0)
    if magic != CAPTURE_MAGIC or version != CAPTURE_VERSION or not 1 <= channels <= 8:
        raise ValueError(f"{path}: not a v{CAPTURE_VERSION} GPU fault capture")
    inputs = [[] for _ in range(channels)]
    outputs = [[] for _ in range(channels)]
    positions: list[int] = []
    segments: list[int] = []
    segment_prepares: list[int] = []
    faults: list[int] = []
    offline = 0
    pos = CAPTURE_HEADER.size
    truncated = 0
    while pos < len(blob):
        if pos + CAPTURE_RECORD.size > len(blob):
            truncated = len(blob) - pos
            break
        project, frames, flags = CAPTURE_RECORD.unpack_from(blob, pos)
        need = frames * channels * 2 * 4
        if frames > 1 << 20:
            raise ValueError(f"{path}: implausible record of {frames} frames at byte {pos}")
        if pos + CAPTURE_RECORD.size + need > len(blob):
            truncated = len(blob) - pos
            break
        pos += CAPTURE_RECORD.size
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
    return Capture(rate, channels, inputs, outputs, positions, segments, offline, faults,
                   truncated, segment_prepares)


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

    @property
    def passed(self) -> bool:
        return (self.covered and self.compared_samples > 0 and self.reference_rms > 0.0
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
    ref_energy = cand_energy = 0.0
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
            if lo <= i < hi and err > window_worst:
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
                      required_frames, min(len(p) for p in candidate), clamped)


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


def health_expectation(fault: str, snapshots: dict[str, dict]) -> tuple[bool, str]:
    """Did the fault take effect, as seen by the transport's own counters?
    A null that passes because the fault never landed is not evidence."""
    missing = [p for p in HEALTH_PHASES if p not in snapshots]
    if missing:
        return False, "missing health snapshots: " + ", ".join(missing)
    before, after = snapshots["before"], snapshots["after"]
    if before.get("provider_state") != "ready":
        return False, f"provider was not ready before the fault ({before.get('provider_state')})"
    if gpu_delivered(before) <= 0:
        # Every block was already CPU fallback, so the fault could only test
        # the fallback. REAPER's anticipative FX processing does exactly this.
        return False, "the GPU path delivered no blocks before the fault"
    fallback_delta = int(after.get("fallback_blocks", 0)) - int(before.get("fallback_blocks", 0))
    gpu_delta = gpu_delivered(after) - gpu_delivered(before)
    if fault in LOSS_FAULTS:
        # A lost device delivers nothing more, so the GPU count freezes once the
        # loss is observed and the fallback carries the stream. recovery_reason
        # is reported but not required: on this base the transport does not yet
        # name the loss for the convolver (WP-A makes that truthful).
        at = snapshots["at"]
        reason = after.get("recovery_reason")
        if fallback_delta <= 0:
            return False, f"{fault}: no fallback blocks after the loss"
        if gpu_delivered(after) != gpu_delivered(at) and reason not in ("provider-lost",
                                                                       "provider-failure"):
            return False, f"{fault}: the GPU kept delivering after the loss"
        return True, (f"loss observed; the GPU stopped delivering and {fallback_delta} fallback "
                      f"blocks covered it; recovery_reason={reason!r}")
    if fault in ("delay_completion", "hang"):
        if fallback_delta <= 0 and int(after.get("missed_blocks", 0)) <= int(before.get("missed_blocks", 0)):
            return False, f"{fault}: neither missed nor fallback blocks moved"
        return True, f"stall observed; {fallback_delta} fallback blocks covered it"
    # render_force_loss: the audio device is separate, so audio health must not move.
    if after.get("provider_state") != "ready" or after.get("recovery_reason", "none") != "none":
        return False, (f"render loss disturbed the audio provider: state="
                       f"{after.get('provider_state')!r} reason={after.get('recovery_reason')!r}")
    if gpu_delta <= 0:
        return False, "render loss: the audio GPU stopped delivering blocks"
    return True, (f"audio provider unaffected by the render-device loss; the GPU delivered "
                  f"{gpu_delta} more blocks after it")


# ── Receipt ──────────────────────────────────────────────────────────────

def decide(*, fault: str, plugin_null: dict | None, bounce_null: dict | None,
           health_ok: bool, fault_fired: bool, host_alive: bool,
           realtime_at_fault: bool, preconditions: Sequence[str] = (),
           mismatches: Sequence[str] = ()) -> tuple[str, list[str]]:
    """Order matters. A host crash, evidence that does not belong to this run
    (`mismatches`), and a computed null that failed are all FAIL, even when a
    precondition also failed, so a broken run is never softened to
    INCONCLUSIVE. `preconditions` (not on the shared-I/O path, a dropped
    capture record, a different IR, a refused fault) make an otherwise clean
    run INCONCLUSIVE."""
    reasons: list[str] = []
    if not host_alive:
        # A crash on render loss is the h2 finding the plan asks to record, not hide.
        return "fail", ["host process died during the run"]
    if mismatches:
        return "fail", list(mismatches)
    for name, result in (("plugin", plugin_null), ("bounce", bounce_null)):
        if result is None:
            reasons.append(f"{name} null not computed")
        elif not result["passed"]:
            why = ("did not cover the fault window" if not result.get("covered", True)
                   else f"max_abs_error={result['max_abs_error']}")
            return "fail", [f"{name} null failed: {why} "
                            f"first={result['first_deviating_sample']} "
                            f"last={result['last_deviating_sample']}"]
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
                   pid: int = 0, tail: int = 0) -> dict[str, Any]:
    """Write the artifacts a real run would produce, from the reference
    itself, so the analysis and the verifier can be exercised without REAPER.
    `break_fallback` is the planted negative: the blocks after the fault
    are silent, as a transport with no CPU fallback would emit."""
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
    wet = delayed(convolve(stream_in, ir), pdc)
    blocks, fault_stream = [], lead_in + fault_sample
    for start in range(0, len(stream_in), block):
        end = min(start + block, len(stream_in))
        out = wet[start:end]
        if break_fallback and end > fault_stream:
            out = [0.0 if start + i >= fault_stream else v for i, v in enumerate(out)]
        playing = start >= lead_in
        flags = (FLAG_PLAYING if playing else 0) | (
            (FLAG_SEGMENT_START | (1 << PREPARE_INDEX_SHIFT)) if start == 0 else 0)
        if start <= fault_stream < end:
            flags |= FLAG_FAULT_BLOCK
        blocks.append({"position": start - lead_in if playing else None, "flags": flags,
                       "inputs": [stream_in[start:end]] * CHANNELS,
                       "outputs": [out] * CHANNELS})
    write_capture(out_dir / "capture.bin", SAMPLE_RATE, blocks)

    bounce = convolve(stim + [0.0] * tail, ir)
    if break_fallback:
        bounce = [0.0 if i >= fault_sample else v for i, v in enumerate(bounce)]
    write_wav_f32(out_dir / "bounce.wav", [bounce] * CHANNELS)

    def status(produced: int, fallback: int, state: str, reason: str) -> dict[str, Any]:
        return {"schema_version": 3, "provider_state": state, "recovery_reason": reason,
                "produced_blocks": produced, "missed_blocks": 0,
                "fallback_blocks": fallback, "recovery_sequence": None,
                "provider": "dawn", "latency_samples": pdc,
                "delivery": {"gpu_blocks": produced, "cpu_fallback_blocks": fallback,
                             "priming_blocks": 2, "silence_blocks": 0},
                "source": "plugin_status_snapshot"}

    lost = fault in LOSS_FAULTS
    stalled = fault in ("delay_completion", "hang")
    # Shaped like this base's real snapshots: a loss freezes GPU delivery but
    # recovery_reason is not named for the convolver.
    health = {
        "before": status(100, 0, "ready", "none"),
        "at": status(150, 1 if (lost or stalled) else 0, "ready", "none"),
        "after": status(150 if lost else 400, 250 if lost else (20 if stalled else 0),
                        "ready", "none"),
    }
    events = [
        {"event": "prepared", "ok": True, "pdc_samples": pdc, "quantum": quantum,
         "lead_blocks": lead_blocks, "ir_taps": len(ir),
         "provider": "dawn", "path": "shared_memory", "nonce": nonce, "pid": pid,
         "prepare_index": 1},
        {"event": "fault_armed", "fault": fault, "at_project_sample": fault_sample},
        {"event": "fault_fired", "fault": fault, "at_project_sample": fault_sample,
         "issued_project_sample": fault_sample + block, "offline": False},
    ] + ([{"event": "render_loss", "performed": True, "detail": "dry run"}]
         if fault in RENDER_FAULTS else
         [{"event": "provider_fault_applied", "point": "dry_run",
           "project_sample": fault_sample + block}]) + [{"event": "health", "phase": p, "status": s} for p, s in health.items()]
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
                 listed: Sequence[str] | None = None) -> dict[str, Any]:
    """Analyse a finished run directory and return the receipt's evidence
    fields (everything except identity and preflight).

    `listed` restricts the analysis to digest-bound artifacts: a file that is
    present but not listed is treated as absent. `render_frames` is how long
    the bounce must be. `nonce` and `reaper_pid` tie the plug-in's events to
    this run and this REAPER process."""
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
    if nonce is not None and any(e.get("nonce") != nonce for e in prepares):
        mismatches.append("the plug-in's events carry a different run nonce")
    if reaper_pid is not None and any(e.get("pid") != reaper_pid for e in prepares):
        mismatches.append("the plug-in's events come from a different process than the REAPER "
                          "this run started")

    capture = None
    plugin_null, plugin_note = None, "no capture"
    owning: list[dict] = []
    if usable("capture.bin") and prepares:
        capture = read_capture(run_dir / "capture.bin")
        # Pair each capture segment with the prepare it follows. A host may
        # prepare several times before processing again; only the last of
        # those owns audio.
        by_index = {e.get("prepare_index"): e for e in prepares}
        owning = [by_index.get(i) for i in capture.segment_prepares]
        if any(e is None for e in owning):
            preconditions.append("a capture segment has no matching prepared event")
            owning = [e for e in owning if e is not None]
        else:
            result, plugin_note = analyze_plugin_capture(
                capture, ir, [int(e.get("pdc_samples", -1)) for e in owning], fault_sample,
                **kw)
            plugin_null = result.as_dict() if result else None

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
    realtime = bool(fired) and not bool(fired.get("offline", True))
    render_loss = first_event(events, "render_loss")
    applied = first_event(events, "provider_fault_applied")
    # Armed is not fired, and fired is not applied: an audio fault counts only
    # once the interposer reports it faulted the provider, a render fault only
    # once the editor reports ForceLoss was issued.
    took_effect = fired is not None and (
        bool(render_loss and render_loss.get("performed")) if fault in RENDER_FAULTS
        else applied is not None)
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
                              preconditions=preconditions, mismatches=mismatches)
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
        "nulls": {"plugin": plugin_null, "bounce": bounce_null,
                  "bounce_lag_estimate": bounce_lag},
        "health": {"snapshots": snaps, "expectation_met": health_ok, "note": health_note},
        "host_alive": host_alive,
        "verdict": verdict,
        "verdict_reasons": reasons,
    }


def artifact_digests(run_dir: Path, names: Sequence[str]) -> dict[str, str]:
    return {name: sha256_file(run_dir / name) for name in names if (run_dir / name).exists()}
