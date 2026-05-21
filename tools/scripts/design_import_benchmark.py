#!/usr/bin/env python3
"""Run the design-import live-vs-baked benchmark and compute the Phase 9 gate.

The C++ harness emits one JSON file per lane. This script drives both lanes,
aggregates the three Phase 5 metric groups, and estimates the removable
live-runtime linked footprint by summing the object files Phase 9 would move
behind a target split.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable


PHASE9_DELTA_BYTES_THRESHOLD = 2 * 1024 * 1024
PHASE9_DELTA_PERCENT_THRESHOLD = 0.30

DEFAULT_LIVE_RUNTIME_OBJECTS = [
    "core/view/CMakeFiles/pulp-view.dir/src/js_quickjs_engine.cpp.o",
    "core/view/CMakeFiles/pulp-view.dir/src/js_engine_factory.cpp.o",
    "core/view/CMakeFiles/pulp-view.dir/src/script_engine.cpp.o",
    "core/view/CMakeFiles/pulp-view.dir/src/widget_bridge.cpp.o",
    "core/view/CMakeFiles/pulp-view.dir/src/widget_bridge_input.cpp.o",
]


def now_utc() -> str:
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def format_bytes(value: float | int) -> str:
    value = float(value)
    if value >= 1024 * 1024:
        return f"{value / (1024 * 1024):.2f} MiB"
    if value >= 1024:
        return f"{value / 1024:.2f} KiB"
    return f"{value:.0f} B"


def format_ms(value: float | int | None) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):.2f} ms"


def format_pct(value: float | int | None) -> str:
    if value is None:
        return "n/a"
    return f"{float(value) * 100:.2f}%"


def first_existing(candidates: Iterable[Path]) -> Path | None:
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def default_bench_exe(build_dir: Path) -> Path:
    exe_name = "pulp-design-import-bench.exe" if os.name == "nt" else "pulp-design-import-bench"
    candidates = [
        build_dir / "tools" / "import-design" / exe_name,
        build_dir / "tools" / "import-design" / "Debug" / exe_name,
        build_dir / "tools" / "import-design" / "Release" / exe_name,
        build_dir / "bin" / exe_name,
    ]
    found = first_existing(candidates)
    return found if found else candidates[0]


def default_pulp_view_archive(build_dir: Path) -> Path | None:
    candidates = [
        build_dir / "core" / "view" / "libpulp-view.a",
        build_dir / "core" / "view" / "Debug" / "pulp-view.lib",
        build_dir / "core" / "view" / "Release" / "pulp-view.lib",
        build_dir / "core" / "view" / "pulp-view.lib",
    ]
    return first_existing(candidates)


def stat_size(path: Path) -> int:
    try:
        return path.stat().st_size
    except OSError:
        return 0


def object_name_variants(rel: str) -> set[str]:
    name = Path(rel).name
    variants = {name}
    if name.endswith(".o"):
        variants.add(name[:-2] + ".obj")
    if name.endswith(".obj"):
        variants.add(name[:-4] + ".o")
    return variants


def find_build_object(build_dir: Path, rel: str) -> Path | None:
    direct = build_dir / rel
    if direct.exists():
        return direct

    preferred_fragment = str(Path("core") / "view" / "CMakeFiles" / "pulp-view.dir")
    matches: list[Path] = []
    for name in object_name_variants(rel):
        matches.extend(build_dir.rglob(name))
    if not matches:
        return None

    matches = sorted(set(matches), key=lambda p: (preferred_fragment not in str(p), str(p)))
    return matches[0]


def parse_size_output(stdout: str) -> int:
    total = 0
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        if not re.fullmatch(r"\d+", parts[0]) or not re.fullmatch(r"\d+", parts[1]):
            continue
        total += int(parts[0]) + int(parts[1])
    return total


def linked_size_bytes(path: Path) -> int:
    tool = os.environ.get("PULP_SIZE_TOOL") or shutil.which("llvm-size") or shutil.which("size")
    if not tool or not path.exists():
        return stat_size(path)
    try:
        result = subprocess.run(
            [tool, str(path)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError):
        return stat_size(path)
    if result.returncode != 0:
        return stat_size(path)
    parsed = parse_size_output(result.stdout)
    return parsed if parsed > 0 else stat_size(path)


def measure_binary_sizes(
    build_dir: Path,
    bench_exe: Path | None = None,
    object_paths: Iterable[str] = DEFAULT_LIVE_RUNTIME_OBJECTS,
) -> dict[str, Any]:
    build_dir = build_dir.resolve()
    bench_exe = (bench_exe or default_bench_exe(build_dir)).resolve()
    view_archive = default_pulp_view_archive(build_dir)

    objects = []
    live_runtime_file_bytes = 0
    live_runtime_linked_bytes = 0
    for rel in object_paths:
        path = find_build_object(build_dir, rel)
        file_size = stat_size(path) if path else 0
        linked_size = linked_size_bytes(path) if path else 0
        objects.append({
            "path": rel,
            "resolved_path": str(path) if path else "",
            "file_bytes": file_size,
            "linked_bytes": linked_size,
            "present": path is not None and path.exists(),
        })
        live_runtime_file_bytes += file_size
        live_runtime_linked_bytes += linked_size

    pulp_view_bytes = stat_size(view_archive) if view_archive else 0
    pulp_view_linked_bytes = linked_size_bytes(view_archive) if view_archive else 0
    app_bytes = stat_size(bench_exe)
    app_linked_bytes = linked_size_bytes(bench_exe)
    delta_percent_of_pulp_view = (
        live_runtime_linked_bytes / pulp_view_linked_bytes if pulp_view_linked_bytes else None
    )
    delta_percent_of_app = live_runtime_linked_bytes / app_linked_bytes if app_linked_bytes else None
    phase9_threshold_met = (
        live_runtime_linked_bytes >= PHASE9_DELTA_BYTES_THRESHOLD
        or (
            delta_percent_of_pulp_view is not None
            and delta_percent_of_pulp_view >= PHASE9_DELTA_PERCENT_THRESHOLD
        )
    )

    return {
        "pulp_view_archive_path": str(view_archive) if view_archive else "",
        "pulp_view_archive_bytes": pulp_view_bytes,
        "pulp_view_archive_linked_bytes": pulp_view_linked_bytes,
        "benchmark_app_path": str(bench_exe),
        "benchmark_app_bytes": app_bytes,
        "benchmark_app_linked_bytes": app_linked_bytes,
        "live_runtime_estimated_object_bytes": live_runtime_file_bytes,
        "live_runtime_estimated_linked_bytes": live_runtime_linked_bytes,
        "hypothetical_baked_only_pulp_view_linked_bytes": max(
            0, pulp_view_linked_bytes - live_runtime_linked_bytes),
        "delta_percent_of_pulp_view_archive": delta_percent_of_pulp_view,
        "delta_percent_of_benchmark_app": delta_percent_of_app,
        "phase9_gate_percent_denominator": "pulp-view linked section bytes",
        "measurement_method": "size text+data linked sections; widget_bridge.cpp.o includes generated web_compat_preludes_gen.hpp",
        "phase9_threshold_bytes": PHASE9_DELTA_BYTES_THRESHOLD,
        "phase9_threshold_percent": PHASE9_DELTA_PERCENT_THRESHOLD,
        "phase9_threshold_met": phase9_threshold_met,
        "objects": objects,
    }


def no_launch_env() -> dict[str, str]:
    env = os.environ.copy()
    env.update({
        "PULP_DISABLE_PLUGIN_EDITOR": "1",
        "PULP_HEADLESS": "1",
        "PULP_TEST_MODE": "1",
        "PULP_INSPECTOR_NO_LAUNCH": "1",
    })
    return env


def run_lane(
    bench_exe: Path,
    lane: str,
    idle_ms: int,
    interactive_ms: int,
    target_fps: int,
    output_dir: Path,
) -> dict[str, Any]:
    out_path = output_dir / f"{lane}.json"
    timeout_s = max(30, int((idle_ms + interactive_ms) / 1000) + 30)
    cmd = [
        str(bench_exe),
        f"--lane={lane}",
        f"--idle-ms={idle_ms}",
        f"--interactive-ms={interactive_ms}",
        f"--target-fps={target_fps}",
        f"--output={out_path}",
    ]
    subprocess.run(cmd, check=True, timeout=timeout_s, env=no_launch_env())
    with out_path.open(encoding="utf-8") as fh:
        return json.load(fh)


def delta_ratio(after: float | int | None, before: float | int | None) -> float | None:
    if before in (None, 0) or after is None:
        return None
    return (float(after) - float(before)) / float(before)


def compute_comparison(live: dict[str, Any], baked: dict[str, Any]) -> dict[str, Any]:
    live_idle = live.get("idle", {})
    baked_idle = baked.get("idle", {})
    live_interactive = live.get("interactive", {})
    baked_interactive = baked.get("interactive", {})
    return {
        "startup_first_frame_delta_ratio": delta_ratio(
            baked.get("startup", {}).get("first_frame_ms"),
            live.get("startup", {}).get("first_frame_ms"),
        ),
        "idle_cpu_delta_ratio": delta_ratio(
            baked_idle.get("cpu_ms"),
            live_idle.get("cpu_ms"),
        ),
        "idle_frame_median_delta_ratio": delta_ratio(
            baked_idle.get("frame_ms_median"),
            live_idle.get("frame_ms_median"),
        ),
        "interactive_cpu_delta_ratio": delta_ratio(
            baked_interactive.get("cpu_ms"),
            live_interactive.get("cpu_ms"),
        ),
        "interactive_cpu_frame_p99_delta_ratio": delta_ratio(
            baked_interactive.get("cpu_frame_ms_p99"),
            live_interactive.get("cpu_frame_ms_p99"),
        ),
        "interactive_frame_median_delta_ratio": delta_ratio(
            baked_interactive.get("frame_ms_median"),
            live_interactive.get("frame_ms_median"),
        ),
        "interactive_frame_p99_delta_ratio": delta_ratio(
            baked_interactive.get("frame_ms_p99"),
            live_interactive.get("frame_ms_p99"),
        ),
        "idle_rss_p99_delta_ratio": delta_ratio(
            baked_idle.get("rss_p99_bytes"),
            live_idle.get("rss_p99_bytes"),
        ),
        "interactive_rss_p99_delta_ratio": delta_ratio(
            baked_interactive.get("rss_p99_bytes"),
            live_interactive.get("rss_p99_bytes"),
        ),
        "live_interactive_js_evaluations": live_interactive.get("js_evaluations_total", 0),
        "baked_interactive_js_evaluations": baked_interactive.get("js_evaluations_total", 0),
    }


def build_report(
    live: dict[str, Any],
    baked: dict[str, Any],
    binary_size: dict[str, Any],
    build_dir: Path,
) -> dict[str, Any]:
    return {
        "schema": "pulp-design-import-benchmark-summary-v1",
        "generated_at": now_utc(),
        "build_dir": str(build_dir.resolve()),
        "lanes": {
            "live": live,
            "baked-native": baked,
        },
        "comparison": compute_comparison(live, baked),
        "binary_size": binary_size,
    }


def render_markdown(report: dict[str, Any]) -> str:
    live = report["lanes"]["live"]
    baked = report["lanes"]["baked-native"]
    binary = report["binary_size"]
    comparison = report["comparison"]

    lines = [
        "# Design-Import Benchmark Results",
        "",
        f"- Generated: `{report['generated_at']}`",
        f"- Fixture: `{live.get('fixture', '?')}`",
        f"- Build dir: `{report['build_dir']}`",
        f"- Target FPS: `{live.get('target_fps', '?')}`",
        "",
        "## Startup",
        "",
        "| Lane | First frame | Build | First render | Paint commands |",
        "|---|---:|---:|---:|---:|",
        (
            f"| live | {format_ms(live.get('startup', {}).get('first_frame_ms'))} | "
            f"{format_ms(live.get('startup', {}).get('build_ms'))} | "
            f"{format_ms(live.get('startup', {}).get('first_frame_render_ms'))} | "
            f"{live.get('startup', {}).get('first_frame_paint_commands', 0)} |"
        ),
        (
            f"| baked-native | {format_ms(baked.get('startup', {}).get('first_frame_ms'))} | "
            f"{format_ms(baked.get('startup', {}).get('build_ms'))} | "
            f"{format_ms(baked.get('startup', {}).get('first_frame_render_ms'))} | "
            f"{baked.get('startup', {}).get('first_frame_paint_commands', 0)} |"
        ),
        "",
        "## Steady State",
        "",
        "| Phase | Lane | Samples | CPU total | CPU frame median | CPU frame p99 | Wall frame median | Wall frame p99 | RSS median | RSS p99 | RSS peak | JS evals |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]

    for phase in ("idle", "interactive"):
        for name, lane in (("live", live), ("baked-native", baked)):
            data = lane.get(phase, {})
            lines.append(
                f"| {phase} | {name} | {data.get('samples', 0)} | "
                f"{format_ms(data.get('cpu_ms'))} | "
                f"{format_ms(data.get('cpu_frame_ms_median'))} | "
                f"{format_ms(data.get('cpu_frame_ms_p99'))} | "
                f"{format_ms(data.get('frame_ms_median'))} | "
                f"{format_ms(data.get('frame_ms_p99'))} | "
                f"{format_bytes(data.get('rss_median_bytes', 0))} | "
                f"{format_bytes(data.get('rss_p99_bytes', 0))} | "
                f"{format_bytes(data.get('rss_peak_bytes', 0))} | "
                f"{data.get('js_evaluations_total', 0)} |"
            )

    lines.extend([
        "",
        "## Binary Size",
        "",
        "| Metric | Value |",
        "|---|---:|",
        f"| pulp-view archive on disk | {format_bytes(binary['pulp_view_archive_bytes'])} |",
        f"| pulp-view linked text+data | {format_bytes(binary['pulp_view_archive_linked_bytes'])} |",
        f"| Benchmark app on disk | {format_bytes(binary['benchmark_app_bytes'])} |",
        f"| Benchmark app linked text+data | {format_bytes(binary['benchmark_app_linked_bytes'])} |",
        f"| Estimated live-runtime linked footprint | {format_bytes(binary['live_runtime_estimated_linked_bytes'])} |",
        f"| Estimated live-runtime object files on disk | {format_bytes(binary['live_runtime_estimated_object_bytes'])} |",
        f"| Hypothetical baked-only pulp-view linked text+data | {format_bytes(binary['hypothetical_baked_only_pulp_view_linked_bytes'])} |",
        f"| Delta as pct of pulp-view linked text+data | {format_pct(binary['delta_percent_of_pulp_view_archive'])} |",
        f"| Delta as pct of benchmark app linked text+data | {format_pct(binary['delta_percent_of_benchmark_app'])} |",
        f"| Measurement method | {binary['measurement_method']} |",
        "",
        "## Phase 9 Gate",
        "",
    ])

    gate = "MET" if binary["phase9_threshold_met"] else "NOT MET"
    lines.append(
        f"Phase 9 gate: **{gate}** "
        f"(threshold: >= {format_bytes(PHASE9_DELTA_BYTES_THRESHOLD)} "
        f"or >= {format_pct(PHASE9_DELTA_PERCENT_THRESHOLD)} of "
        f"{binary['phase9_gate_percent_denominator']})."
    )
    lines.extend([
        "",
        "## Live Runtime Objects",
        "",
        "| Object | Linked text+data | Object file bytes | Present |",
        "|---|---:|---:|---|",
    ])
    for obj in binary["objects"]:
        lines.append(
            f"| `{obj['path']}` | {format_bytes(obj['linked_bytes'])} | "
            f"{format_bytes(obj['file_bytes'])} | {obj['present']} |"
        )

    lines.extend([
        "",
        "## Comparison",
        "",
        f"- Startup first-frame delta (baked vs live): {format_pct(comparison['startup_first_frame_delta_ratio'])}",
        f"- Idle CPU delta (baked vs live): {format_pct(comparison['idle_cpu_delta_ratio'])}",
        f"- Interactive CPU delta (baked vs live): {format_pct(comparison['interactive_cpu_delta_ratio'])}",
        f"- Interactive CPU-frame p99 delta (baked vs live): {format_pct(comparison['interactive_cpu_frame_p99_delta_ratio'])}",
        f"- Interactive p99 frame delta (baked vs live): {format_pct(comparison['interactive_frame_p99_delta_ratio'])}",
        f"- Idle RSS p99 delta (baked vs live): {format_pct(comparison['idle_rss_p99_delta_ratio'])}",
        f"- Interactive RSS p99 delta (baked vs live): {format_pct(comparison['interactive_rss_p99_delta_ratio'])}",
        (
            "- JS execution churn: "
            f"live interactive evaluations={comparison['live_interactive_js_evaluations']}, "
            f"baked-native interactive evaluations={comparison['baked_interactive_js_evaluations']}"
        ),
        "",
    ])
    return "\n".join(lines)


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--bench-exe", type=Path)
    parser.add_argument("--idle-ms", type=int, default=60000)
    parser.add_argument("--interactive-ms", type=int, default=60000)
    parser.add_argument("--target-fps", type=int, default=60)
    parser.add_argument("--output-json", type=Path)
    parser.add_argument("--output-md", type=Path)
    parser.add_argument(
        "--object",
        action="append",
        dest="objects",
        help="Additional/override live-runtime object path relative to build dir. "
             "If provided, defaults are replaced.",
    )
    parser.add_argument("--skip-run", action="store_true", help="Only compute binary-size data.")
    args = parser.parse_args(argv)

    build_dir = args.build_dir.resolve()
    bench_exe = (args.bench_exe or default_bench_exe(build_dir)).resolve()
    object_paths = args.objects if args.objects else DEFAULT_LIVE_RUNTIME_OBJECTS
    binary_size = measure_binary_sizes(build_dir, bench_exe, object_paths)

    if args.skip_run:
        live = {"lane": "live", "fixture": "not-run", "target_fps": args.target_fps}
        baked = {"lane": "baked-native", "fixture": "not-run", "target_fps": args.target_fps}
    else:
        if not bench_exe.exists():
            print(f"error: benchmark executable not found: {bench_exe}", file=sys.stderr)
            return 1
        with tempfile.TemporaryDirectory(prefix="pulp-design-import-bench-") as td:
            temp_dir = Path(td)
            live = run_lane(bench_exe, "live", args.idle_ms, args.interactive_ms, args.target_fps, temp_dir)
            baked = run_lane(bench_exe, "baked-native", args.idle_ms, args.interactive_ms, args.target_fps, temp_dir)

    report = build_report(live, baked, binary_size, build_dir)
    markdown = render_markdown(report)

    if args.output_json:
        write_text(args.output_json, json.dumps(report, indent=2, sort_keys=True) + "\n")
    if args.output_md:
        write_text(args.output_md, markdown + "\n")
    if not args.output_json and not args.output_md:
        print(markdown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
