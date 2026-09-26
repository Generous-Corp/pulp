#!/usr/bin/env python3
"""
Diff two Pulp zero-copy benchmark JSON files (baseline vs current).

Part of the zero-copy initiative (#516). The ralph loop's per-slice
procedure runs a benchmark after each implementation slice and feeds
the output here to produce a readable before/after table.

Input schema (per-widget JSON, see examples/ui-preview/CMakeLists):

    {
      "host": "mba-m2",
      "date": "2026-04-20T12:00:00Z",
      "pulp_commit": "<sha>",
      "platform": "darwin-arm64",
      "widget": "oscilloscope",
      "seconds": 10,
      "target_fps": 60,
      "samples": 600,
      "per_frame_us": {
        "audio_to_triplebuffer_copy": 12.4,
        "triplebuffer_publish_latency": 0.8,
        "gpu_upload_us": 18.2,
        "gpu_readback_us": 0.0,
        "gpu_dispatch_us": 45.1,
        "total_frame_us": 120.5
      },
      "per_frame_bytes": {
        "cpu_to_gpu_bytes": 32768,
        "gpu_to_cpu_bytes": 0
      },
      "frame_budget_us": 16666,
      "memory_bandwidth_fraction": 0.112
    }

A second, generic input shape — "sections" — carries any before/after metric
set (the build-speed scorecard writes it):

    {
      "schema": "pulp-bench-sections/1",
      "title": "Build speed", "host": "m3", "date": "...", "pulp_commit": "<sha>",
      "sections": [
        {"title": "Required macos gate job (min)", "unit": "min",
         "lower_is_better": true, "higher_is_better_keys": [],
         "values": {"pull_request p50": 39.5, "merge_group p50": 40.3}}
      ]
    }

A value that is absent or null is UNKNOWN and renders as such, never as 0.
``higher_is_better_keys`` flips the direction for individual rows of a
section whose default is ``lower_is_better``.

Usage:

    tools/scripts/bench_diff.py baseline.json current.json
    tools/scripts/bench_diff.py baseline.json current.json --threshold 0.05
    tools/scripts/bench_diff.py --format markdown baseline.json current.json

Exit code: 0 if benchmarks are readable; nonzero if inputs are malformed.
The script does NOT fail the shell when current is worse than baseline —
the ralph loop interprets the diff and decides whether to merge.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


def load(path: Path) -> dict[str, Any]:
    with path.open() as fh:
        return json.load(fh)


def fmt_us(v: float) -> str:
    return f"{v:7.2f} µs"


def fmt_bytes(v: float) -> str:
    if v >= 1_000_000:
        return f"{v / 1_000_000:6.2f} MB"
    if v >= 1_000:
        return f"{v / 1_000:6.2f} KB"
    return f"{v:6.0f} B "


def fmt_pct(v: float) -> str:
    return f"{v * 100:5.2f}%"


def fmt_delta(before: float, after: float, lower_is_better: bool = True) -> str:
    if before == 0 and after == 0:
        return "  =   "
    if before == 0:
        return "  new "
    delta = (after - before) / before
    sign = "-" if delta < 0 else "+"
    arrow = "↓" if (delta < 0) == lower_is_better else "↑"
    return f"{arrow} {sign}{abs(delta) * 100:5.1f}%"


def diff_section(
    title: str,
    before_map: dict[str, float],
    after_map: dict[str, float],
    formatter,
    lower_is_better: bool = True,
) -> list[str]:
    lines = [f"## {title}", ""]
    lines.append(f"| Metric | Baseline | Current | Δ |")
    lines.append(f"|---|---|---|---|")
    keys = sorted(set(before_map) | set(after_map))
    for k in keys:
        b = before_map.get(k, 0.0)
        a = after_map.get(k, 0.0)
        lines.append(
            f"| {k} | {formatter(b)} | {formatter(a)} | "
            f"{fmt_delta(b, a, lower_is_better)} |"
        )
    lines.append("")
    return lines


def _fmt_value(v: Any, unit: str) -> str:
    if v is None:
        return "—"
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        # Sub-10 values keep three significant figures: a 0.13 ns/frame kernel
        # and a 0.028 ns/element one must not both render as "0.1".
        if float(v).is_integer():
            text = f"{v:,.0f}"
        elif abs(v) < 10:
            text = f"{v:.3g}"
        else:
            text = f"{v:,.1f}"
    else:
        return str(v)
    return f"{text} {unit}" if unit and unit != "count" else text


def section_delta(before: float, after: float, lower_is_better: bool | None) -> str:
    """Signed relative change plus whether it is better or worse.

    ``lower_is_better=None`` marks a metric with no good direction (a count to
    watch, not to optimize); it shows the change and never judges it.
    """
    if before == after:
        return "="
    if before == 0:
        return "new"
    rel = (after - before) / abs(before)
    if lower_is_better is None:
        return f"{rel * 100:+.1f}%"
    better = (rel < 0) == lower_is_better
    return f"{rel * 100:+.1f}% ({'better' if better else 'worse'})"


def render_sections(baseline: dict[str, Any], current: dict[str, Any],
                    threshold: float = 0.05) -> str:
    """Before/after tables for two ``sections`` documents.

    A row whose change is worse than ``threshold`` (relative) in the section's
    direction is listed under Regressions. Rows missing on either side are
    shown as unknown and never counted as a change.
    """
    out = [f"# Bench diff: {current.get('title') or baseline.get('title') or 'sections'}", "",
           f"- **Baseline:** {baseline.get('date', '?')} "
           f"({str(baseline.get('pulp_commit', '?'))[:10]} on {baseline.get('host', '?')})",
           f"- **Current:** {current.get('date', '?')} "
           f"({str(current.get('pulp_commit', '?'))[:10]} on {current.get('host', '?')})", ""]
    base_secs = {s["title"]: s for s in baseline.get("sections", [])}
    cur_secs = {s["title"]: s for s in current.get("sections", [])}
    titles = list(base_secs) + [t for t in cur_secs if t not in base_secs]
    regressions: list[str] = []
    for title in titles:
        b, c = base_secs.get(title, {}), cur_secs.get(title, {})
        unit = c.get("unit", b.get("unit", ""))
        lower = c.get("lower_is_better", b.get("lower_is_better", True))
        bv, cv = b.get("values", {}), c.get("values", {})
        keys = list(bv) + [k for k in cv if k not in bv]
        out += [f"## {title}", "", "| Metric | Baseline | Current | Δ |", "|---|---|---|---|"]
        higher = set(c.get("higher_is_better_keys", b.get("higher_is_better_keys", [])))
        for k in keys:
            x, y = bv.get(k), cv.get(k)
            lower = c.get("lower_is_better", b.get("lower_is_better", True))
            if lower is not None and k in higher:
                lower = False
            num = all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in (x, y))
            delta = section_delta(float(x), float(y), lower) if num else "unknown"
            out.append(f"| {k} | {_fmt_value(x, unit)} | {_fmt_value(y, unit)} | {delta} |")
            if num and x and lower is not None:
                rel = (y - x) / abs(x)
                if (rel > threshold and lower) or (rel < -threshold and not lower):
                    regressions.append(f"{title} / {k}: {_fmt_value(x, unit)} → {_fmt_value(y, unit)}")
        notes = c.get("notes") or b.get("notes")
        if notes:
            out += [""] + [f"- {n}" for n in notes]
        out.append("")
    out += ["## Regressions beyond threshold", ""]
    out += [f"- {r}" for r in regressions] or [f"- none beyond {threshold * 100:.0f}%"]
    return "\n".join(out) + "\n"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("baseline", type=Path)
    p.add_argument("current", type=Path)
    p.add_argument(
        "--format",
        choices=["markdown", "text"],
        default="markdown",
        help="Output format (default markdown, suitable for PR descriptions)",
    )
    p.add_argument(
        "--threshold",
        type=float,
        default=0.05,
        help="Memory-bandwidth threshold (default 0.05 = 5%% of frame budget)",
    )
    args = p.parse_args()

    try:
        baseline = load(args.baseline)
        current = load(args.current)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if "sections" in baseline or "sections" in current:
        text = render_sections(baseline, current, args.threshold)
        print(text if args.format == "markdown" else text.replace("|", "  "))
        return 0

    if baseline.get("widget") != current.get("widget"):
        print(
            f"warning: widget mismatch "
            f"(baseline={baseline.get('widget')}, current={current.get('widget')})",
            file=sys.stderr,
        )

    out: list[str] = []
    out.append(f"# Bench diff: {baseline.get('widget', '?')}")
    out.append("")
    out.append(
        f"- **Baseline:** {args.baseline.name} "
        f"({baseline.get('pulp_commit', '?')[:8]} on {baseline.get('host', '?')})"
    )
    out.append(
        f"- **Current:** {args.current.name} "
        f"({current.get('pulp_commit', '?')[:8]} on {current.get('host', '?')})"
    )
    out.append(f"- **Platform:** {current.get('platform', '?')}")
    out.append(f"- **Samples:** {current.get('samples', '?')} frames over {current.get('seconds', '?')}s")
    out.append("")

    out.extend(
        diff_section(
            "Per-frame latency",
            baseline.get("per_frame_us", {}),
            current.get("per_frame_us", {}),
            fmt_us,
        )
    )

    out.extend(
        diff_section(
            "Per-frame bytes moved",
            baseline.get("per_frame_bytes", {}),
            current.get("per_frame_bytes", {}),
            fmt_bytes,
        )
    )

    # Missing `memory_bandwidth_fraction` is UNKNOWN, not zero. Defaulting to
    # 0.0 would silently emit "below threshold — ROI may be limited" for
    # snapshots where the field was never captured (schema drift, partial
    # profiling runs, etc.) and mask a real missing-data problem. Distinguish
    # "absent" from "0%".
    base_mb = baseline.get("memory_bandwidth_fraction")
    curr_mb = current.get("memory_bandwidth_fraction")
    out.append("## Memory-bandwidth fraction")
    out.append("")
    out.append(f"- Baseline: {fmt_pct(base_mb) if base_mb is not None else '(not reported)'}")
    out.append(f"- Current:  {fmt_pct(curr_mb) if curr_mb is not None else '(not reported)'}")
    if base_mb is not None and curr_mb is not None:
        out.append(f"- Δ:        {fmt_delta(base_mb, curr_mb)}")
    out.append(f"- Threshold: {fmt_pct(args.threshold)} of frame budget")
    out.append("")

    verdict = []
    if base_mb is None:
        verdict.append(
            "**Baseline** did not report `memory_bandwidth_fraction` — "
            "threshold evaluation skipped. This field is required for the "
            "zero-copy go/no-go decision; check the capture harness."
        )
    elif base_mb >= args.threshold:
        verdict.append(
            f"**Baseline** exceeds {fmt_pct(args.threshold)} threshold — "
            "zero-copy has leverage here."
        )
    else:
        verdict.append(
            f"**Baseline** below {fmt_pct(args.threshold)} threshold — "
            "zero-copy ROI may be limited on this widget."
        )

    if curr_mb is None:
        verdict.append(
            "**Current** did not report `memory_bandwidth_fraction` — "
            "no before/after comparison possible on this metric."
        )
    elif base_mb is None:
        verdict.append(
            "Current reported the fraction but baseline didn't — record "
            "a fresh baseline before drawing conclusions."
        )
    elif curr_mb < base_mb:
        saved = (base_mb - curr_mb) / base_mb if base_mb > 0 else 0.0
        verdict.append(
            f"**Current** reduced memory-bandwidth fraction by {fmt_pct(saved)} "
            "relative to baseline."
        )
    elif curr_mb > base_mb:
        verdict.append(
            "**Current** regressed — investigate before merging."
        )
    else:
        verdict.append("No change in memory-bandwidth fraction.")
    out.append("## Verdict")
    out.append("")
    out.extend(f"- {line}" for line in verdict)
    out.append("")

    if args.format == "markdown":
        print("\n".join(out))
    else:
        # Plain-text: strip markdown table pipes
        for line in out:
            print(line.replace("|", "  "))

    return 0


if __name__ == "__main__":
    sys.exit(main())
