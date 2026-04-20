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

    base_mb = baseline.get("memory_bandwidth_fraction", 0.0)
    curr_mb = current.get("memory_bandwidth_fraction", 0.0)
    out.append("## Memory-bandwidth fraction")
    out.append("")
    out.append(f"- Baseline: {fmt_pct(base_mb)}")
    out.append(f"- Current:  {fmt_pct(curr_mb)}")
    out.append(f"- Δ:        {fmt_delta(base_mb, curr_mb)}")
    out.append(f"- Threshold: {fmt_pct(args.threshold)} of frame budget")
    out.append("")

    verdict = []
    if base_mb >= args.threshold:
        verdict.append(
            f"**Baseline** exceeds {fmt_pct(args.threshold)} threshold — "
            "zero-copy has leverage here."
        )
    else:
        verdict.append(
            f"**Baseline** below {fmt_pct(args.threshold)} threshold — "
            "zero-copy ROI may be limited on this widget."
        )
    if curr_mb < base_mb:
        saved = (base_mb - curr_mb) / base_mb
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
