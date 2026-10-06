#!/usr/bin/env python3
"""Per-frame cost of a Perfetto capture, by pipeline stage, as one command.

The question every UI optimization starts and ends with -- "what does one
frame of this cost, and where does it go?" -- is several queries and a
spreadsheet by hand: find the frame spans, attribute every slice inside each
frame to a stage by EXCLUSIVE (self) time so nesting is not double counted,
count the repaint requests and layout passes, and take p50 / p95 / max per
stage, separately for each scenario the capture walked through. This does it:

    tools/scripts/trace_frame_cost.py --trace capture.pftrace \\
        --frame plugin_editor_frame            # or frame, modctl_frame, ...
        [--group modctl_scenario --labels idle,knobs,morph]
        [--max-p95-ms 4 --max-layout-frames 0 --baseline idle] [--json]

Stages are Pulp trace categories (layout, canvas, js, text, state, render;
see core/runtime/include/pulp/runtime/trace.hpp). `repaints` counts
`view_repaint_request` slices -- each one is a whole-surface repaint request
(a bounded one takes a different path and emits none) -- and `layout` counts
frames that ran `layout_children`. `damage` is the share of `--surface` a
frame asked to repaint: 100 % for any whole-surface request, else the summed
area of its bounded requests (`view_repaint_bounded`, an upper bound on their
union). `--group` splits the frames by the
enclosing span of that name, in time order; `--labels` names those spans.

Budgets make it a gate: a breach prints one line per failure and exits 1.
A capture whose frame span never fired, or whose ring wrapped, exits 2 and
says so rather than reporting an empty table as a pass.

The SQL is checked in beside the trace-sql skill
(.agents/skills/trace-sql/pulp_frame_stage_cost.sql); this tool embeds the
same definition so it runs without a skill checkout.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
from pathlib import Path
import shutil
import subprocess

from script_argv import argv_for
import sys
import tempfile

MARKER = "PULP_FRAME_COST"
STAGES = ("layout", "canvas", "js", "text", "state", "render")


def _sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def build_query(frame: str, group: str) -> str:
    """One marker row per frame: group index, frame ms, stage self ms,
    whole-surface repaint requests, layout passes. Plus integrity rows."""
    frame_name = _sql_string(frame)
    group_name = _sql_string(group) if group else "NULL"
    stage_cols = ",\n  ".join(
        f"COALESCE(SUM(CASE WHEN i.category = '{s}' THEN i.self_dur END), 0) AS {s}_ns"
        for s in STAGES)
    stage_out = " || '|' || ".join(f"printf('%.6f', {s}_ns / 1e6)" for s in STAGES)
    return f"""
WITH frames AS (
  SELECT id, ts, dur, track_id, depth FROM slice
  WHERE name = {frame_name} AND dur >= 0
), child_sum AS (
  SELECT parent_id, SUM(dur) AS dur FROM slice
  WHERE dur >= 0 AND parent_id IS NOT NULL GROUP BY parent_id
), inner_slices AS (
  SELECT f.id AS frame_id, s.category, s.name, s.arg_set_id,
         s.dur - COALESCE(cs.dur, 0) AS self_dur
  FROM frames f
  JOIN slice s ON s.track_id = f.track_id AND s.depth > f.depth
              AND s.ts >= f.ts AND s.ts + s.dur <= f.ts + f.dur
  LEFT JOIN child_sum cs ON cs.parent_id = s.id
  WHERE s.dur >= 0
), groups AS (
  SELECT ts, dur, ROW_NUMBER() OVER (ORDER BY ts) - 1 AS idx FROM slice
  WHERE name = {group_name} AND dur >= 0
), per_frame AS (
  SELECT f.id, f.ts, f.dur,
  {stage_cols},
  COALESCE(SUM(CASE WHEN i.name = 'view_repaint_request' THEN 1 END), 0) AS repaints,
  COALESCE(SUM(CASE WHEN i.name = 'layout_children' THEN 1 END), 0) AS layouts,
  COALESCE(SUM(CASE WHEN i.name = 'view_repaint_bounded'
    THEN EXTRACT_ARG(i.arg_set_id, 'debug.w') * EXTRACT_ARG(i.arg_set_id, 'debug.h') END), 0)
    AS bounded_area
  FROM frames f LEFT JOIN inner_slices i ON i.frame_id = f.id
  GROUP BY f.id
)
SELECT '{MARKER}|frame|' ||
  COALESCE((SELECT g.idx FROM groups g WHERE p.ts >= g.ts AND p.ts < g.ts + g.dur), -1)
  || '|' || printf('%.6f', p.dur / 1e6) || '|' || {stage_out}
  || '|' || p.repaints || '|' || p.layouts || '|' || p.bounded_area
FROM per_frame p
UNION ALL
SELECT '{MARKER}|stat|' || name || '|' || value FROM stats
WHERE name IN ('traced_buf_write_wrap_count',
               'packet_skipped_seq_needs_incremental_state_invalid') AND value != 0;
"""


@dataclasses.dataclass
class Frame:
    group: int
    frame_ms: float
    stages: dict
    repaints: int
    layouts: int
    bounded_area: float = 0.0


def parse(output: str) -> tuple[list[Frame], dict]:
    frames: list[Frame] = []
    stats: dict = {}
    for raw in output.splitlines():
        line = raw.strip().strip('"')
        if not line.startswith(MARKER + "|"):
            continue
        parts = line.split("|")
        if parts[1] == "stat":
            stats[parts[2]] = int(float(parts[3]))
            continue
        values = parts[2:]
        if len(values) not in (4 + len(STAGES), 5 + len(STAGES)):
            raise ValueError(f"malformed frame row: {line}")
        area = float(values[4 + len(STAGES)]) if len(values) == 5 + len(STAGES) else 0.0
        frames.append(Frame(
            group=int(values[0]), frame_ms=float(values[1]),
            stages={s: float(v) for s, v in zip(STAGES, values[2:2 + len(STAGES)])},
            repaints=int(values[2 + len(STAGES)]), layouts=int(values[3 + len(STAGES)]),
            bounded_area=area))
    return frames, stats


def percentile(values: list[float], q: float) -> float:
    """Nearest-rank percentile (the value a frame actually had)."""
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = max(1, -(-int(q * len(ordered) * 100) // 10000))  # ceil(q% of n)
    return ordered[min(rank, len(ordered)) - 1]


SURFACE_AREA = [0.0]


def damage_fraction(f: Frame) -> float:
    """The share of the surface a frame asked to repaint: all of it for any
    whole-surface request, else its bounded rects' summed area (an upper bound
    on their union). 0 for a frame that requested nothing."""
    if f.repaints > 0:
        return 1.0
    if SURFACE_AREA[0] <= 0.0:
        return 0.0
    return min(1.0, f.bounded_area / SURFACE_AREA[0])


def summarize(frames: list[Frame]) -> dict:
    def pcts(values):
        return {"p50": percentile(values, 50), "p95": percentile(values, 95),
                "max": max(values) if values else 0.0}
    return {
        "frames": len(frames),
        "frame_ms": pcts([f.frame_ms for f in frames]),
        "stages_ms": {s: pcts([f.stages[s] for f in frames]) for s in STAGES},
        "repaints_per_frame": (sum(f.repaints for f in frames) / len(frames)) if frames else 0.0,
        "repaint_frames": sum(1 for f in frames if f.repaints > 0),
        "layout_frames": sum(1 for f in frames if f.layouts > 0),
        "damage_frac": pcts([damage_fraction(f) for f in frames]),
        "bounded_area_px2": pcts([f.bounded_area for f in frames]),
    }


def group_frames(frames: list[Frame], labels: list[str]) -> dict:
    groups: dict = {}
    for f in frames:
        key = labels[f.group] if 0 <= f.group < len(labels) else (
            "all" if f.group < 0 else f"group{f.group}")
        groups.setdefault(key, []).append(f)
    return {k: summarize(v) for k, v in groups.items()}


def check(summary: dict, args, baseline: dict | None) -> list[str]:
    failures = []
    for name, s in summary.items():
        if args.baseline and name == args.baseline:
            continue
        if args.max_p95_ms is not None:
            grown = s["frame_ms"]["p95"] - (baseline["frame_ms"]["p95"] if baseline else 0.0)
            if grown > args.max_p95_ms:
                failures.append(f"{name}: frame p95 {'grew ' if baseline else ''}"
                                f"{grown:.3f} ms > {args.max_p95_ms:.3f} ms")
        if args.max_layout_frames is not None and s["layout_frames"] > args.max_layout_frames:
            failures.append(f"{name}: {s['layout_frames']}/{s['frames']} frames ran a layout pass")
        if args.max_damage_frac is not None:
            if SURFACE_AREA[0] <= 0.0:
                failures.append("damage budget needs --surface WxH")
            elif s["damage_frac"]["p95"] > args.max_damage_frac:
                failures.append(f"{name}: p95 damage {100 * s['damage_frac']['p95']:.2f}% of the "
                                f"surface > {100 * args.max_damage_frac:.2f}%")
        if args.max_repaint_frames is not None and s["repaint_frames"] > args.max_repaint_frames:
            failures.append(f"{name}: {s['repaint_frames']}/{s['frames']} frames requested a "
                            "whole-surface repaint")
    return failures


def render_table(summary: dict) -> str:
    head = (f"{'group':<22} {'frames':>6}  {'frame p50/p95/max ms':>22}  "
            + "  ".join(f"{s + ' p95':>10}" for s in STAGES)
            + f"  {'repaint/f':>9} {'layout f':>8} {'damage p95':>10}")
    rows = [head, "-" * len(head)]
    for name, s in summary.items():
        f = s["frame_ms"]
        rows.append(f"{name:<22} {s['frames']:>6}  "
                    f"{f['p50']:>6.3f}/{f['p95']:>6.3f}/{f['max']:>7.3f}  "
                    + "  ".join(f"{s['stages_ms'][st]['p95']:>10.3f}" for st in STAGES)
                    + f"  {s['repaints_per_frame']:>9.2f} {s['layout_frames']:>8}"
                    + f" {100 * s['damage_frac']['p95']:>9.2f}%")
    return "\n".join(rows)


def resolve_processor(explicit: str) -> str | None:
    if explicit:
        return explicit
    configured = os.environ.get("PULP_TRACE_PROCESSOR", "")
    if configured:
        return configured
    pinned = Path.home() / ".pulp/tools/trace-processor/v57.2"
    for candidate in sorted(pinned.glob("*/trace_processor_shell")):
        return str(candidate)
    return shutil.which("trace_processor_shell") or shutil.which("trace_processor")


def run_processor(processor: str, trace: Path, query: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as handle:
        handle.write(query)
        query_path = Path(handle.name)
    try:
        result = subprocess.run([*argv_for(processor), "-q", str(query_path), str(trace)],
                                check=False, capture_output=True, text=True)
    finally:
        query_path.unlink(missing_ok=True)
    if result.returncode != 0:
        raise RuntimeError(f"trace_processor exited {result.returncode}: {result.stderr.strip()}")
    return result.stdout


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--frame", default="frame", help="frame span name")
    parser.add_argument("--group", default="", help="enclosing span that splits scenarios")
    parser.add_argument("--labels", default="", help="comma-separated names for the groups")
    parser.add_argument("--baseline", default="", help="group the p95 budget is relative to")
    parser.add_argument("--max-p95-ms", type=float, default=None)
    parser.add_argument("--max-layout-frames", type=int, default=None)
    parser.add_argument("--max-repaint-frames", type=int, default=None)
    parser.add_argument("--max-damage-frac", type=float, default=None,
                        help="p95 share of the surface a frame may repaint (needs --surface)")
    parser.add_argument("--surface", default="", help="logical surface size, WxH")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--processor", default="")
    args = parser.parse_args(argv)

    if not args.trace.is_file():
        parser.error(f"trace does not exist: {args.trace}")
    SURFACE_AREA[0] = 0.0
    if args.surface:
        w, _, h = args.surface.lower().partition("x")
        SURFACE_AREA[0] = float(w) * float(h)
    processor = resolve_processor(args.processor)
    if not processor:
        print("trace_processor not found; run `pulp trace fetch` or set PULP_TRACE_PROCESSOR",
              file=sys.stderr)
        return 2
    try:
        frames, stats = parse(run_processor(processor, args.trace,
                                            build_query(args.frame, args.group)))
    except (OSError, RuntimeError, ValueError) as error:
        print(f"trace frame cost error: {error}", file=sys.stderr)
        return 2
    if stats:
        print(f"capture is truncated ({stats}); re-capture with a bigger ring "
              "(PULP_TRACE_RING_KB) -- nothing in it can be trusted", file=sys.stderr)
        return 2
    if not frames:
        print(f"no '{args.frame}' span in the capture: the frame span never fired, or the "
              "build was not traced -- not a pass", file=sys.stderr)
        return 2
    labels = [l for l in args.labels.split(",") if l] if args.labels else []
    summary = group_frames(frames, labels)
    baseline = summary.get(args.baseline) if args.baseline else None
    if args.baseline and baseline is None:
        print(f"baseline group '{args.baseline}' is not in the capture", file=sys.stderr)
        return 2
    failures = check(summary, args, baseline)
    if args.json:
        print(json.dumps({"summary": summary, "failures": failures}, indent=2))
    else:
        print(render_table(summary))
        for line in failures:
            print(f"FAIL: {line}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
