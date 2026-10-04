#!/usr/bin/env python3
"""Find the glitches in a render and the trace blocks that made them.

One command for the loop an audio glitch hunt otherwise runs by hand: render a
scenario with tracing on, find the click or dropout in the WAV, then find the
host block and the work inside it in the Perfetto trace.

    glitch_trace.py --wav out.wav --trace out.pftrace [--json]

THE TIMELINE JOIN. The processor emits one ``process`` slice per host block
(category ``dsp``) carrying ``stream_pos`` (the stream sample of the block's
first frame) and ``frames``. A glitch at sample N of the WAV is the block whose
[stream_pos, stream_pos + frames) holds N, so every event is reported with
that block's wall time, its deadline (frames / rate) and the slices nested in
it. Without those args there is no sample <-> time mapping, and the tool says
so instead of guessing one.

THE DETECTORS (pure Python, no numpy):

- dropout: 5 ms RMS windows more than ``--dropout-db`` below the median level
  of the second before them. A gap reads as a run of such windows.
- click: a sample the signal's own recent past does not predict. An order-32
  linear predictor is fitted to each second of the render and run over it;
  each residual sample is scored against the residual RMS in the 4 ms around
  it (excluding its own half millisecond). The score is in dB over that
  neighbourhood; ``--click-db`` (default 30) flags.

Both are reference-free. A transient the material itself carries (a drum hit)
also scores; compare against an unedited render of the same material, or read
the score next to the block's cost rather than alone.

BLOCK TIME. Per-block wall time is reported against the deadline: p50, p99,
max, and the count of blocks that ran past their deadline. An offline render
has no deadline to miss, so a miss here is a block that WOULD drop a buffer in
a host at that buffer size -- often the real cause of "it glitches live but
the offline render is clean".
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# ── WAV ─────────────────────────────────────────────────────────────────────


def read_wav(path: Path) -> tuple[list[float], float]:
    """First channel of a PCM16/24/32 or float32 WAV, and its rate."""
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")
    pos, fmt, rate, channels, bits, samples = 12, None, 0.0, 0, 0, None
    while pos + 8 <= len(data):
        chunk, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if chunk == b"fmt ":
            fmt, channels, rate_i, _, _, bits = struct.unpack_from("<HHIIHH", body, 0)
            if fmt == 0xFFFE and len(body) >= 26:
                fmt = struct.unpack_from("<H", body, 24)[0]
            rate = float(rate_i)
        elif chunk == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    if fmt is None or samples is None or channels < 1:
        raise ValueError(f"{path}: missing fmt or data chunk")
    width = bits // 8
    frame = width * channels
    n = len(samples) // frame
    out: list[float] = []
    if fmt == 3 and bits == 32:
        out = [struct.unpack_from("<f", samples, i * frame)[0] for i in range(n)]
    elif fmt == 1 and bits == 16:
        out = [struct.unpack_from("<h", samples, i * frame)[0] / 32768.0 for i in range(n)]
    elif fmt == 1 and bits == 24:
        for i in range(n):
            b = samples[i * frame:i * frame + 3]
            v = int.from_bytes(b, "little", signed=True)
            out.append(v / 8388608.0)
    elif fmt == 1 and bits == 32:
        out = [struct.unpack_from("<i", samples, i * frame)[0] / 2147483648.0 for i in range(n)]
    else:
        raise ValueError(f"{path}: unsupported WAV format {fmt}/{bits}-bit")
    return out, rate


# ── Detectors ───────────────────────────────────────────────────────────────

ORDER = 32


@dataclass
class Event:
    kind: str          # "click" | "dropout"
    sample: int        # first sample of the event
    score_db: float    # click: dB over neighbourhood; dropout: dB under the level
    length: int = 1    # samples (a dropout's span)
    block: dict | None = None


def _whitener(x: list[float], start: int, end: int) -> list[float]:
    n = end - start
    if n <= ORDER + 1:
        return [1.0] + [0.0] * ORDER
    w = [x[start + i] * (0.5 - 0.5 * math.cos(2.0 * math.pi * i / (n - 1))) for i in range(n)]
    r = [sum(w[i] * w[i - k] for i in range(k, n)) for k in range(ORDER + 1)]
    r[0] = r[0] * (1.0 + 1e-6) + 1e-12
    a = [1.0] + [0.0] * ORDER
    err = r[0]
    for i in range(1, ORDER + 1):
        acc = r[i] + sum(a[j] * r[i - j] for j in range(1, i))
        k = -acc / err
        prev = a[:]
        for j in range(1, i):
            a[j] = prev[j] + k * prev[i - j]
        a[i] = k
        err *= 1.0 - k * k
        if err <= 0.0:
            break
    return a


def detect_clicks(x: list[float], rate: float, threshold_db: float) -> list[Event]:
    """Whitened-residual spikes, refitting the predictor every second."""
    n = len(x)
    seg = max(int(rate), ORDER * 8)
    e = [0.0] * n
    for start in range(0, n, seg):
        end = min(n, start + seg)
        # Fit on the segment before when there is one: a fit that includes the
        # click learns it.
        fit_from, fit_to = (start - seg, start) if start >= seg else (start, end)
        a = _whitener(x, fit_from, fit_to)
        for i in range(start, end):
            acc = 0.0
            for k in range(min(ORDER, i) + 1):
                acc += a[k] * x[i - k]
            e[i] = acc
    outer, inner = max(2, int(0.004 * rate)), max(1, int(0.0005 * rate))
    prefix = [0.0]
    for v in e:
        prefix.append(prefix[-1] + v * v)
    events: list[Event] = []
    last = -10**9
    merge = int(0.005 * rate)
    for i in range(outer, n - outer - 1):
        around = (prefix[i - inner] - prefix[i - outer]) + (prefix[i + outer + 1] - prefix[i + inner + 1])
        rms = math.sqrt(around / (2 * (outer - inner)) + 1e-24)
        db = 20.0 * math.log10(abs(e[i]) / rms + 1e-12)
        if db >= threshold_db:
            if i - last <= merge and events:
                if db > events[-1].score_db:
                    events[-1].score_db = db
                    events[-1].sample = i
            else:
                events.append(Event("click", i, db))
            last = i
    return events


def detect_dropouts(x: list[float], rate: float, threshold_db: float) -> list[Event]:
    """5 ms windows far below the median level of the second before them."""
    w = max(1, int(0.005 * rate))
    levels = []
    for start in range(0, len(x) - w + 1, w):
        acc = sum(v * v for v in x[start:start + w])
        levels.append(10.0 * math.log10(acc / w + 1e-20))
    history = max(1, int(1.0 / 0.005))
    events: list[Event] = []
    run_start = None
    for idx, level in enumerate(levels):
        before = sorted(levels[max(0, idx - history):idx])
        median = before[len(before) // 2] if before else level
        low = len(before) >= history // 2 and median > -90.0 and level < median - threshold_db
        if low and run_start is None:
            run_start = idx
        if (not low or idx == len(levels) - 1) and run_start is not None:
            end = idx if not low else idx + 1
            depth = min(levels[run_start:end]) - (median if before else 0.0)
            events.append(Event("dropout", run_start * w, depth, (end - run_start) * w))
            run_start = None
    return events


# ── Trace ───────────────────────────────────────────────────────────────────

BLOCK_QUERY = """
SELECT s.id, s.ts, s.dur, s.depth,
  COALESCE(EXTRACT_ARG(s.arg_set_id, 'debug.stream_pos'),
           EXTRACT_ARG(s.arg_set_id, 'args.stream_pos')) AS stream_pos,
  COALESCE(EXTRACT_ARG(s.arg_set_id, 'debug.frames'),
           EXTRACT_ARG(s.arg_set_id, 'args.frames')) AS frames
FROM slice s
WHERE s.name = 'process' AND s.dur >= 0
ORDER BY s.ts;
"""

CHILD_QUERY = """
SELECT s.ts, s.dur, s.name, s.depth FROM slice s
WHERE s.name != 'process'
ORDER BY s.ts;
"""


def resolve_processor(explicit: str = "") -> str | None:
    if explicit:
        return explicit
    if os.environ.get("PULP_TRACE_PROCESSOR"):
        return os.environ["PULP_TRACE_PROCESSOR"]
    found = shutil.which("trace_processor_shell") or shutil.which("trace_processor")
    if found:
        return found
    cache = Path.home() / ".pulp" / "tools" / "trace-processor"
    if cache.is_dir():
        for candidate in sorted(cache.glob("*/*/trace_processor_shell"), reverse=True):
            return str(candidate)
    return None


def run_query(processor: str, trace: Path, query: str) -> list[dict]:
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as handle:
        handle.write(query)
        query_path = handle.name
    try:
        result = subprocess.run([processor, "-q", query_path, str(trace)],
                                check=False, capture_output=True, text=True)
    finally:
        os.unlink(query_path)
    if result.returncode != 0:
        raise RuntimeError(f"trace_processor exited {result.returncode}: {result.stderr.strip()}")
    lines = [ln for ln in result.stdout.splitlines() if ln.strip()]
    # trace_processor prints a CSV-like table with a quoted header row.
    header_at = next((i for i, ln in enumerate(lines) if ln.startswith('"')), None)
    if header_at is None:
        return []
    header = [h.strip('"') for h in lines[header_at].split(",")]
    rows = []
    for ln in lines[header_at + 1:]:
        cells = [c.strip('"') for c in ln.split(",")]
        if len(cells) != len(header):
            continue
        rows.append(dict(zip(header, cells)))
    return rows


@dataclass
class Block:
    ts: int
    dur: int
    stream_pos: int
    frames: int
    children: list[tuple[str, int]] = field(default_factory=list)


def blocks_from_rows(rows: list[dict]) -> list[Block]:
    blocks = []
    for r in rows:
        try:
            pos, frames = r.get("stream_pos"), r.get("frames")
            if pos in (None, "", "[NULL]") or frames in (None, "", "[NULL]"):
                continue
            blocks.append(Block(int(r["ts"]), int(r["dur"]), int(float(pos)), int(float(frames))))
        except (KeyError, ValueError):
            continue
    return blocks


def attach_children(blocks: list[Block], rows: list[dict]) -> None:
    j = 0
    for r in rows:
        ts, dur = int(r["ts"]), int(r["dur"]) if r.get("dur") not in (None, "") else 0
        while j < len(blocks) and blocks[j].ts + blocks[j].dur < ts:
            j += 1
        if j < len(blocks) and blocks[j].ts <= ts <= blocks[j].ts + blocks[j].dur:
            blocks[j].children.append((r["name"], max(0, dur)))


def locate(sample: int, blocks: list[Block]) -> Block | None:
    lo, hi = 0, len(blocks) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        b = blocks[mid]
        if sample < b.stream_pos:
            hi = mid - 1
        elif sample >= b.stream_pos + b.frames:
            lo = mid + 1
        else:
            return b
    return None


def percentile(values: list[float], q: float) -> float:
    if not values:
        return float("nan")
    s = sorted(values)
    return s[min(len(s) - 1, int(round(q * (len(s) - 1))))]


def block_stats(blocks: list[Block], rate: float) -> dict:
    ms = [b.dur / 1e6 for b in blocks]
    misses = [b for b in blocks if b.dur / 1e9 > b.frames / rate]
    worst = max(blocks, key=lambda b: b.dur / max(1, b.frames), default=None)
    return {
        "blocks": len(blocks),
        "p50_ms": percentile(ms, 0.50),
        "p99_ms": percentile(ms, 0.99),
        "max_ms": max(ms) if ms else float("nan"),
        "deadline_misses": len(misses),
        "worst_load": (worst.dur / 1e9) / (worst.frames / rate) if worst else float("nan"),
        "worst_block_pos": worst.stream_pos if worst else None,
    }


# ── CLI ─────────────────────────────────────────────────────────────────────


def analyse(wav: Path, trace: Path | None, click_db: float, dropout_db: float,
            processor: str = "") -> dict:
    x, rate = read_wav(wav)
    events = detect_clicks(x, rate, click_db) + detect_dropouts(x, rate, dropout_db)
    events.sort(key=lambda e: e.sample)
    report: dict = {"wav": str(wav), "rate": rate, "samples": len(x), "events": []}
    blocks: list[Block] = []
    if trace is not None:
        tp = resolve_processor(processor)
        if not tp:
            raise RuntimeError("trace_processor not found; set PULP_TRACE_PROCESSOR")
        blocks = blocks_from_rows(run_query(tp, trace, BLOCK_QUERY))
        if not blocks:
            report["trace_error"] = ("no 'process' slices with stream_pos/frames args: the "
                                     "processor must stamp them to join samples to time")
        else:
            attach_children(blocks, run_query(tp, trace, CHILD_QUERY))
            report["block_time"] = block_stats(blocks, rate)
    for e in events:
        item = {"kind": e.kind, "sample": e.sample, "seconds": e.sample / rate,
                "score_db": round(e.score_db, 1), "length": e.length}
        b = locate(e.sample, blocks) if blocks else None
        if b:
            item["block"] = {"stream_pos": b.stream_pos, "frames": b.frames,
                             "wall_ms": b.dur / 1e6, "deadline_ms": 1e3 * b.frames / rate,
                             "children": sorted(b.children, key=lambda c: -c[1])[:6]}
        report["events"].append(item)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--wav", type=Path, required=True)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--click-db", type=float, default=30.0)
    parser.add_argument("--dropout-db", type=float, default=20.0)
    parser.add_argument("--processor", default="")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        report = analyse(args.wav, args.trace, args.click_db, args.dropout_db, args.processor)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"glitch_trace: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(report, indent=1))
        return 1 if report["events"] else 0
    print(f"{report['wav']}: {report['samples']} samples at {report['rate']:.0f} Hz")
    if "trace_error" in report:
        print(f"  trace: {report['trace_error']}")
    if "block_time" in report:
        t = report["block_time"]
        print(f"  block time: {t['blocks']} blocks  p50 {t['p50_ms']:.3f} ms  p99 {t['p99_ms']:.3f} ms"
              f"  max {t['max_ms']:.3f} ms  deadline misses {t['deadline_misses']}"
              f"  (worst {100 * t['worst_load']:.0f}% of its deadline at sample {t['worst_block_pos']})")
    if not report["events"]:
        print("  no clicks or dropouts")
    for ev in report["events"]:
        line = f"  {ev['kind']:7s} at sample {ev['sample']} ({ev['seconds']:.4f} s)  {ev['score_db']:+.1f} dB"
        if ev["kind"] == "dropout":
            line += f"  for {1e3 * ev['length'] / report['rate']:.1f} ms"
        print(line)
        if "block" in ev:
            b = ev["block"]
            print(f"          block [{b['stream_pos']}, +{b['frames']}) took {b['wall_ms']:.3f} ms"
                  f" of {b['deadline_ms']:.3f} ms" + "".join(
                      f"; {name} {dur / 1e6:.3f} ms" for name, dur in b["children"]))
    return 1 if report["events"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
