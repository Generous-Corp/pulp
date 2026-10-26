#!/usr/bin/env python3
"""Run the design-import live-vs-baked benchmark and compute the Phase 9 gate.

The C++ harness emits one JSON file per lane. This script drives the live,
baked-native, and baked-cpp lanes, aggregates the three Phase 5 metric groups,
and estimates the removable live-runtime linked footprint by summing the object
files that live behind the Phase 9 script-runtime target split.

The optional baked-native-rebuild lane rematerializes the same IR on every
interactive step. It is a diagnostic control for retained-vs-rebuild work and
is opt-in because it is intentionally much more expensive than the normal
live-vs-baked report.

`--compare BASELINE CANDIDATE` instead runs the two lanes as separate
processes in ABAB order and judges the candidate against the speed-win
criterion: a declared mechanism counter must move as predicted, the median of
the named stage must be at most 0.80x the baseline over at least five reps per
side, p95, allocations per frame and the cycle memory slope must not be worse,
the paint stream and slot read-back must agree, and no receipt may be
confounded by host load or thermal state. Apple thermal state is observed
around every rep with tools/scripts/p2_thermal_state.swift as well as inside
each receipt. Anything short of that is reported as what it is ("work-moved",
"no-win", "confounded", "insufficient-reps"), never as a win.
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
    "core/view/CMakeFiles/pulp-view-script.dir/src/js_quickjs_engine.cpp.o",
    "core/view/CMakeFiles/pulp-view-script.dir/src/js_engine_factory.cpp.o",
    "core/view/CMakeFiles/pulp-view-script.dir/src/script_engine.cpp.o",
    "core/view/CMakeFiles/pulp-view-script.dir/src/widget_bridge.cpp.o",
    "core/view/CMakeFiles/pulp-view-script.dir/src/widget_bridge_input.cpp.o",
]

BAKED_LANES = ("baked-native", "baked-native-rebuild", "baked-cpp")
# v2 adds per-frame resource work (decodes, uploads, paragraph builds, scene
# and image cache) beside the v1 fields, which it keeps unchanged.
COUNTER_SCHEMAS = (
    "pulp-design-import-benchmark-counters-v1",
    "pulp-design-import-benchmark-counters-v2",
)
COUNTER_FIELDS = (
    "bridge_calls",
    "layout_passes",
    "paint_calls",
    "paint_commands_total",
    "invalidation_requests",
    "unmarked_paint_mutations",
)


def now_utc() -> str:
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def format_bytes(value: float | int | None) -> str:
    if value is None:
        return "n/a"
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


def phase_rss(phase: dict[str, Any], stat: str) -> int | None:
    """One RSS statistic of a phase: v2 `memory.rss.<stat>_bytes`, or the v1
    flat `rss_<stat>_bytes`. An absent reading stays None, never 0."""
    memory = phase.get("memory")
    if isinstance(memory, dict) and isinstance(memory.get("rss"), dict):
        return memory["rss"].get(f"{stat}_bytes")
    return phase.get(f"rss_{stat}_bytes")


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


def archive_candidates(build_dir: Path, target: str) -> list[Path]:
    return [
        build_dir / "core" / "view" / f"lib{target}.a",
        build_dir / "core" / "view" / "Debug" / f"{target}.lib",
        build_dir / "core" / "view" / "Release" / f"{target}.lib",
        build_dir / "core" / "view" / f"{target}.lib",
    ]


def default_pulp_view_archive(build_dir: Path) -> Path | None:
    return first_existing(archive_candidates(build_dir, "pulp-view"))


def default_pulp_view_archives(build_dir: Path) -> list[Path]:
    split_archives = [
        archive
        for target in ("pulp-view-core", "pulp-view-script", "pulp-view")
        if (archive := first_existing(archive_candidates(build_dir, target))) is not None
    ]
    if any("pulp-view-core" in archive.name or "pulp-view-script" in archive.name
           for archive in split_archives):
        return split_archives

    fallback = default_pulp_view_archive(build_dir)
    return [fallback] if fallback else []


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

    preferred_fragment = str(Path("core") / "view" / "CMakeFiles" / "pulp-view-script.dir")
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
    view_archives = default_pulp_view_archives(build_dir)

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

    pulp_view_bytes = sum(stat_size(path) for path in view_archives)
    pulp_view_linked_bytes = sum(linked_size_bytes(path) for path in view_archives)
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
        "pulp_view_archive_path": ";".join(str(path) for path in view_archives),
        "pulp_view_archive_paths": [str(path) for path in view_archives],
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
        report = json.load(fh)
    validate_lane_counters(report)
    return report


def validate_lane_counters(report: dict[str, Any]) -> None:
    """Fail closed if a lane omits the additive deterministic counters."""
    for phase_name in ("idle", "interactive"):
        phase = report.get(phase_name)
        if not isinstance(phase, dict):
            raise ValueError(f"benchmark lane missing phase {phase_name}")
        counters = phase.get("counters")
        if not isinstance(counters, dict):
            raise ValueError(f"benchmark lane missing {phase_name}.counters")
        if counters.get("counter_schema") not in COUNTER_SCHEMAS:
            raise ValueError(f"benchmark lane has invalid {phase_name}.counters.counter_schema")
        for field in COUNTER_FIELDS:
            value = counters.get(field)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(f"benchmark lane has invalid {phase_name}.counters.{field}")


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
        "interactive_update_median_delta_ratio": delta_ratio(
            baked_interactive.get("update_ms_median"),
            live_interactive.get("update_ms_median"),
        ),
        "interactive_update_p99_delta_ratio": delta_ratio(
            baked_interactive.get("update_ms_p99"),
            live_interactive.get("update_ms_p99"),
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
            phase_rss(baked_idle, "p99"),
            phase_rss(live_idle, "p99"),
        ),
        "interactive_rss_p99_delta_ratio": delta_ratio(
            phase_rss(baked_interactive, "p99"),
            phase_rss(live_interactive, "p99"),
        ),
        "live_interactive_js_evaluations": live_interactive.get("js_evaluations_total", 0),
        "baked_interactive_js_evaluations": baked_interactive.get("js_evaluations_total", 0),
    }


def build_report(
    live: dict[str, Any],
    baked_native: dict[str, Any],
    binary_size: dict[str, Any],
    build_dir: Path,
    baked_cpp: dict[str, Any] | None = None,
    baked_native_rebuild: dict[str, Any] | None = None,
) -> dict[str, Any]:
    lanes = {
        "live": live,
        "baked-native": baked_native,
    }
    comparisons = {
        "baked-native": compute_comparison(live, baked_native),
    }
    if baked_cpp is not None:
        lanes["baked-cpp"] = baked_cpp
        comparisons["baked-cpp"] = compute_comparison(live, baked_cpp)
    if baked_native_rebuild is not None:
        lanes["baked-native-rebuild"] = baked_native_rebuild
        comparisons["baked-native-rebuild"] = compute_comparison(live, baked_native_rebuild)

    return {
        "schema": "pulp-design-import-benchmark-summary-v1",
        "generated_at": now_utc(),
        "build_dir": str(build_dir.resolve()),
        "lanes": lanes,
        "comparison": comparisons["baked-native"],
        "comparisons": comparisons,
        "binary_size": binary_size,
    }


def render_markdown(report: dict[str, Any]) -> str:
    lanes = report["lanes"]
    live = lanes["live"]
    binary = report["binary_size"]
    comparisons = report.get("comparisons", {"baked-native": report["comparison"]})
    lane_order = ["live"] + [name for name in BAKED_LANES if name in lanes]

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
    ]
    for name in lane_order:
        lane = lanes[name]
        lines.append(
            f"| {name} | {format_ms(lane.get('startup', {}).get('first_frame_ms'))} | "
            f"{format_ms(lane.get('startup', {}).get('build_ms'))} | "
            f"{format_ms(lane.get('startup', {}).get('first_frame_render_ms'))} | "
            f"{lane.get('startup', {}).get('first_frame_paint_commands', 0)} |"
        )

    lines.extend([
        "",
        "## Steady State",
        "",
        "| Phase | Lane | Samples | CPU total | CPU frame median | CPU frame p99 | Wall frame median | Wall frame p99 | Update median | Update p99 | RSS median | RSS p99 | RSS peak | JS evals |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ])

    for phase in ("idle", "interactive"):
        for name in lane_order:
            lane = lanes[name]
            data = lane.get(phase, {})
            lines.append(
                f"| {phase} | {name} | {data.get('samples', 0)} | "
                f"{format_ms(data.get('cpu_ms'))} | "
                f"{format_ms(data.get('cpu_frame_ms_median'))} | "
                f"{format_ms(data.get('cpu_frame_ms_p99'))} | "
                f"{format_ms(data.get('frame_ms_median'))} | "
                f"{format_ms(data.get('frame_ms_p99'))} | "
                f"{format_ms(data.get('update_ms_median'))} | "
                f"{format_ms(data.get('update_ms_p99'))} | "
                f"{format_bytes(phase_rss(data, 'median'))} | "
                f"{format_bytes(phase_rss(data, 'p99'))} | "
                f"{format_bytes(phase_rss(data, 'peak'))} | "
                f"{data.get('js_evaluations_total', 0)} |"
            )

    lines.extend([
        "",
        "## Deterministic Work Counters",
        "",
        "| Phase | Lane | Bridge calls | Layout passes | Paint calls | Paint commands | Invalidation requests | Unmarked paint mutations |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ])
    for phase in ("idle", "interactive"):
        for name in lane_order:
            counters = lanes[name].get(phase, {}).get("counters", {})
            lines.append(
                f"| {phase} | {name} | {counters.get('bridge_calls', 0)} | "
                f"{counters.get('layout_passes', 0)} | {counters.get('paint_calls', 0)} | "
                f"{counters.get('paint_commands_total', 0)} | "
                f"{counters.get('invalidation_requests', 0)} | "
                f"{counters.get('unmarked_paint_mutations', 0)} |"
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
    ])
    for target in (name for name in BAKED_LANES if name in comparisons):
        comparison = comparisons[target]
        lines.extend([
            f"- Startup first-frame delta ({target} vs live): {format_pct(comparison['startup_first_frame_delta_ratio'])}",
            f"- Idle CPU delta ({target} vs live): {format_pct(comparison['idle_cpu_delta_ratio'])}",
            f"- Interactive CPU delta ({target} vs live): {format_pct(comparison['interactive_cpu_delta_ratio'])}",
            f"- Interactive CPU-frame p99 delta ({target} vs live): {format_pct(comparison['interactive_cpu_frame_p99_delta_ratio'])}",
            f"- Interactive update median delta ({target} vs live): {format_pct(comparison['interactive_update_median_delta_ratio'])}",
            f"- Interactive update p99 delta ({target} vs live): {format_pct(comparison['interactive_update_p99_delta_ratio'])}",
            f"- Interactive p99 frame delta ({target} vs live): {format_pct(comparison['interactive_frame_p99_delta_ratio'])}",
            f"- Idle RSS p99 delta ({target} vs live): {format_pct(comparison['idle_rss_p99_delta_ratio'])}",
            f"- Interactive RSS p99 delta ({target} vs live): {format_pct(comparison['interactive_rss_p99_delta_ratio'])}",
            (
                f"- JS execution churn ({target} vs live): "
                f"live interactive evaluations={comparison['live_interactive_js_evaluations']}, "
                f"{target} interactive evaluations={comparison['baked_interactive_js_evaluations']}"
            ),
        ])
    lines.append("")
    return "\n".join(lines)


# ── ABAB comparison against the speed-win criterion ─────────────────────────

WIN_MAX_RATIO = 0.80
WIN_MIN_REPS = 5
THERMAL_HELPER = Path(__file__).resolve().parent / "p2_thermal_state.swift"
THERMAL_SCHEMA = "pulp.gpu-audio.p2.thermal-observation.v1"
STAGES = ("frame", "update", "layout", "record", "submit")
# Bytes per cycle the candidate's memory slope may exceed the baseline's
# before it counts as worse; settled cycles of an unchanged binary differ by
# allocator noise well below this.
SLOPE_TOLERANCE_BYTES = 256 * 1024


def observe_thermal(helper: Path = THERMAL_HELPER, runner: Any = None) -> dict[str, Any]:
    """One Apple thermal-state observation from the checked-in Swift helper.

    A host without the helper or without Swift yields an explicit absence,
    which the judge treats as unproven (and so confounding), not as nominal.
    """
    runner = runner or subprocess.run
    swift = shutil.which("swift") or "/usr/bin/swift"
    if sys.platform != "darwin" or not Path(swift).exists() or not helper.exists():
        return {"thermal_state": None, "reason": "no Apple thermal-state helper on this host"}
    try:
        result = runner([swift, str(helper)], capture_output=True, text=True, timeout=120,
                        check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"thermal_state": None, "reason": f"thermal helper failed: {error}"}
    if result.returncode != 0:
        return {"thermal_state": None, "reason": f"thermal helper exited {result.returncode}"}
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError:
        return {"thermal_state": None, "reason": "thermal helper printed invalid JSON"}
    if not isinstance(value, dict) or value.get("schema") != THERMAL_SCHEMA:
        return {"thermal_state": None, "reason": "thermal helper schema mismatch"}
    return {"thermal_state": value.get("thermal_state"), "source": value.get("source"),
            "sampled_at": value.get("sampled_at")}


def abab_order(baseline: str, candidate: str, reps: int) -> list[tuple[str, str]]:
    """Interleave the two sides so host drift lands on both equally."""
    order: list[tuple[str, str]] = []
    for _ in range(reps):
        order.append(("baseline", baseline))
        order.append(("candidate", candidate))
    return order


def stage_value(phase: dict[str, Any], stage: str, stat: str) -> float | None:
    """`stat` (median or p95) of one stage of a v2 phase, in ms."""
    if stage == "frame":
        value = phase.get(f"frame_ms_{stat}")
    else:
        value = phase.get("stages", {}).get(stage, {}).get(f"{stat}_ms")
    return float(value) if isinstance(value, (int, float)) else None


def counter_total(counters: dict[str, Any], counter: str) -> Any:
    """A v1 counter by name, or a v2 resource series by dotted name
    (`image_decodes`, `scene_cache.replays`), as its total."""
    if counter in counters:
        return counters[counter]
    node: Any = counters.get("resources", {})
    for part in counter.split("."):
        node = node.get(part) if isinstance(node, dict) else None
    return node.get("total") if isinstance(node, dict) else None


def per_frame_counter(phase: dict[str, Any], counter: str) -> float | None:
    samples = phase.get("samples")
    value = counter_total(phase.get("counters", {}), counter)
    if not isinstance(samples, int) or samples <= 0 or not isinstance(value, (int, float)):
        return None
    return float(value) / samples


def median(values: list[float]) -> float | None:
    values = sorted(v for v in values if v is not None)
    if not values:
        return None
    mid = len(values) // 2
    return values[mid] if len(values) % 2 else (values[mid - 1] + values[mid]) / 2.0


def cycle_slope(receipt: dict[str, Any]) -> float | None:
    cycles = receipt.get("cycles")
    if not isinstance(cycles, dict):
        return None
    for key in ("phys_footprint_slope_bytes_per_cycle", "rss_slope_bytes_per_cycle"):
        if isinstance(cycles.get(key), (int, float)):
            return float(cycles[key])
    return None


def parse_mechanism(text: str | None) -> tuple[str, str] | None:
    """`counter:down|up|zero` names the counter a change claims to move."""
    if not text:
        return None
    name, _, direction = text.partition(":")
    if not name or direction not in ("down", "up", "zero"):
        raise ValueError("--mechanism must be COUNTER:down|up|zero")
    return name, direction


def judge(baseline: list[dict[str, Any]], candidate: list[dict[str, Any]], *,
          stage: str = "frame", mechanism: tuple[str, str] | None = None,
          thermal: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    """Apply the speed-win criterion to two sets of separate-process receipts."""
    findings: list[str] = []
    blockers: list[str] = []
    phase = "interactive"

    def side(receipts: list[dict[str, Any]], getter: Any) -> float | None:
        return median([getter(r.get(phase, {})) for r in receipts])

    confounders = sorted({reason for r in baseline + candidate
                          for reason in r.get("host_conditions", {}).get("confounders", [])
                          if r.get("host_conditions", {}).get("confounded")})
    confounded = any(r.get("host_conditions", {}).get("confounded", True)
                     for r in baseline + candidate)
    for observation in thermal or []:
        state = observation.get("thermal_state")
        if state != "nominal":
            confounded = True
            confounders.append(f"thermal helper: {state or observation.get('reason')}")
    inconsistent = [r.get("lane") for r in baseline + candidate
                    if not r.get(phase, {}).get("stages", {}).get("consistent", False)]

    base_stage = side(baseline, lambda p: stage_value(p, stage, "median"))
    cand_stage = side(candidate, lambda p: stage_value(p, stage, "median"))
    ratio = (cand_stage / base_stage) if base_stage and cand_stage is not None else None
    base_p95 = side(baseline, lambda p: stage_value(p, stage, "p95"))
    cand_p95 = side(candidate, lambda p: stage_value(p, stage, "p95"))
    base_alloc = side(baseline, lambda p: p.get("allocations_per_frame_median"))
    cand_alloc = side(candidate, lambda p: p.get("allocations_per_frame_median"))
    base_slope = median([cycle_slope(r) for r in baseline])
    cand_slope = median([cycle_slope(r) for r in candidate])

    if ratio is None:
        blockers.append(f"no {stage} timing on one side")
    elif ratio > WIN_MAX_RATIO:
        blockers.append(f"{stage} median ratio {ratio:.3f} > {WIN_MAX_RATIO:.2f}")
    if base_p95 is not None and cand_p95 is not None and cand_p95 > base_p95:
        blockers.append(f"{stage} p95 worse ({cand_p95:.4f} ms > {base_p95:.4f} ms)")
    if base_alloc is not None and cand_alloc is not None and cand_alloc > base_alloc:
        blockers.append(f"allocations/frame worse ({cand_alloc:.1f} > {base_alloc:.1f})")
    if base_slope is None or cand_slope is None:
        blockers.append("cycle memory slope not measured on both sides")
    elif cand_slope > base_slope + SLOPE_TOLERANCE_BYTES:
        blockers.append(f"memory slope worse ({cand_slope:.0f} > {base_slope:.0f} B/cycle)")

    # Agreement: the two lanes paint the same stream, every slot reads back,
    # and each side is deterministic across its own reps.
    def stable(receipts: list[dict[str, Any]], key: str) -> bool:
        values = {r.get(phase, {}).get("counters", {}).get(key) for r in receipts}
        return len(values) == 1 and None not in values
    # The untimed RecordingCanvas count of the final tree; a Skia target
    # reports 0 paint_commands_last, which would make every pair "agree".
    paint = {r.get("paint_commands_recorded", r.get(phase, {}).get("paint_commands_last"))
             for r in baseline + candidate}
    agreement = {
        "paint_stream_count_equal": len(paint) == 1 and next(iter(paint)) not in (None, 0),
        "slot_read_back_clean": all(
            r.get(phase, {}).get("counters", {}).get("slot_text_mismatches", 1) == 0
            for r in baseline + candidate),
        "baseline_deterministic": stable(baseline, "paint_commands_total"),
        "candidate_deterministic": stable(candidate, "paint_commands_total"),
    }
    for name, ok in agreement.items():
        if not ok:
            blockers.append(f"agreement check failed: {name}")

    mechanism_result: dict[str, Any] = {"declared": None}
    mechanism_moved = False
    if mechanism:
        name, direction = mechanism
        base_count = side(baseline, lambda p: per_frame_counter(p, name))
        cand_count = side(candidate, lambda p: per_frame_counter(p, name))
        if base_count is not None and cand_count is not None:
            mechanism_moved = {
                "down": cand_count < base_count,
                "up": cand_count > base_count,
                "zero": cand_count == 0 and base_count > 0,
            }[direction]
        mechanism_result = {"declared": f"{name}:{direction}", "baseline_per_frame": base_count,
                            "candidate_per_frame": cand_count, "moved": mechanism_moved}
        if not mechanism_moved:
            findings.append(f"mechanism {name} did not move {direction}")
    else:
        findings.append("no mechanism counter declared")

    reps_ok = len(baseline) >= WIN_MIN_REPS and len(candidate) >= WIN_MIN_REPS
    if confounded:
        verdict = "confounded"
    elif not reps_ok:
        verdict = "insufficient-reps"
    elif inconsistent:
        verdict = "instrument-inconsistent"
    elif not blockers and mechanism_moved:
        verdict = "win"
    elif not blockers or mechanism_moved:
        # Timing without the counter, or the counter without the timing.
        verdict = "work-moved"
    else:
        verdict = "no-win"
    return {
        "schema": "pulp-design-import-benchmark-verdict-v1",
        "verdict": verdict,
        "criterion": {"max_ratio": WIN_MAX_RATIO, "min_reps_per_side": WIN_MIN_REPS,
                      "stage": stage, "phase": phase},
        "reps": {"baseline": len(baseline), "candidate": len(candidate)},
        "confounders": confounders,
        "inconsistent_stage_clocks": inconsistent,
        "stage_median_ms": {"baseline": base_stage, "candidate": cand_stage},
        "ratio": ratio,
        "stage_p95_ms": {"baseline": base_p95, "candidate": cand_p95},
        "allocations_per_frame": {"baseline": base_alloc, "candidate": cand_alloc},
        "cycle_slope_bytes": {"baseline": base_slope, "candidate": cand_slope},
        "agreement": agreement,
        "mechanism": mechanism_result,
        "blockers": blockers,
        "findings": findings,
    }


def run_compare(bench_exe: Path, baseline: str, candidate: str, reps: int,
                bench_args: list[str], output_dir: Path, *, stage: str = "frame",
                mechanism: tuple[str, str] | None = None, observe: Any = observe_thermal,
                runner: Any = None, side_args: dict[str, list[str]] | None = None
                ) -> dict[str, Any]:
    """`side_args` adds arguments to one side only, so one lane can be compared
    with itself under a different setting (`--paragraph-cache=off`, say)."""
    runner = runner or subprocess.run
    side_args = side_args or {}
    receipts: dict[str, list[dict[str, Any]]] = {"baseline": [], "candidate": []}
    thermal: list[dict[str, Any]] = []
    order = abab_order(baseline, candidate, reps)
    for index, (role, lane) in enumerate(order):
        out_path = output_dir / f"{index:02d}-{role}-{lane}.json"
        thermal.append(observe())
        cmd = [str(bench_exe), f"--lane={lane}", *bench_args, *side_args.get(role, []),
               f"--output={out_path}"]
        runner(cmd, check=True, timeout=3600, env=no_launch_env())
        thermal.append(observe())
        with out_path.open(encoding="utf-8") as fh:
            receipt = json.load(fh)
        validate_lane_counters(receipt)
        receipts[role].append(receipt)
    result = judge(receipts["baseline"], receipts["candidate"], stage=stage,
                   mechanism=mechanism, thermal=thermal)
    result.update({"generated_at": now_utc(), "baseline_lane": baseline,
                   "candidate_lane": candidate, "order": [lane for _, lane in order],
                   "bench_args": bench_args, "side_args": side_args,
                   "thermal_observations": thermal})
    return result


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
    parser.add_argument(
        "--include-rebuild-lane",
        action="store_true",
        help="Also run the diagnostic baked-native-rebuild lane.",
    )
    parser.add_argument(
        "--compare", nargs=2, metavar=("BASELINE", "CANDIDATE"),
        help="Run two lanes ABAB as separate processes and judge the candidate "
             "against the speed-win criterion instead of writing the live-vs-baked report.",
    )
    parser.add_argument("--reps", type=int, default=WIN_MIN_REPS,
                        help="Separate-process reps per side for --compare.")
    parser.add_argument("--stage", choices=STAGES, default="frame",
                        help="Interactive stage --compare judges (frame includes update).")
    parser.add_argument("--mechanism",
                        help="COUNTER:down|up|zero the candidate claims to move, per frame.")
    parser.add_argument("--bench-arg", action="append", default=[], dest="bench_args",
                        help="Argument passed to every --compare rep (repeatable), e.g. "
                             "--bench-arg=--frames=600 --bench-arg=--ir=panel.ir.json.")
    parser.add_argument("--baseline-arg", action="append", default=[], dest="baseline_args",
                        help="Argument passed to the baseline side's reps only (repeatable).")
    parser.add_argument("--candidate-arg", action="append", default=[], dest="candidate_args",
                        help="Argument passed to the candidate side's reps only (repeatable), "
                             "e.g. to compare a lane with itself: --compare baked-native "
                             "baked-native --baseline-arg=--paragraph-cache=off.")
    args = parser.parse_args(argv)

    build_dir = args.build_dir.resolve()
    bench_exe = (args.bench_exe or default_bench_exe(build_dir)).resolve()
    if args.compare:
        if not bench_exe.exists():
            print(f"error: benchmark executable not found: {bench_exe}", file=sys.stderr)
            return 1
        try:
            mechanism = parse_mechanism(args.mechanism)
        except ValueError as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
        bench_args = list(args.bench_args)
        # The criterion needs the memory slope; ask every rep for it unless
        # the caller chose a cycle count.
        if not any(arg.startswith("--cycles") for arg in bench_args):
            bench_args.append("--cycles=3")
        with tempfile.TemporaryDirectory(prefix="pulp-design-import-compare-") as td:
            result = run_compare(bench_exe, args.compare[0], args.compare[1], args.reps,
                                 bench_args, Path(td), stage=args.stage, mechanism=mechanism,
                                 side_args={"baseline": list(args.baseline_args),
                                            "candidate": list(args.candidate_args)})
        text = json.dumps(result, indent=2, sort_keys=True) + "\n"
        if args.output_json:
            write_text(args.output_json, text)
        else:
            print(text, end="")
        return 0
    object_paths = args.objects if args.objects else DEFAULT_LIVE_RUNTIME_OBJECTS
    binary_size = measure_binary_sizes(build_dir, bench_exe, object_paths)

    if args.skip_run:
        live = {"lane": "live", "fixture": "not-run", "target_fps": args.target_fps}
        baked = {"lane": "baked-native", "fixture": "not-run", "target_fps": args.target_fps}
        baked_cpp = {"lane": "baked-cpp", "fixture": "not-run", "target_fps": args.target_fps}
        baked_rebuild = (
            {"lane": "baked-native-rebuild", "fixture": "not-run", "target_fps": args.target_fps}
            if args.include_rebuild_lane
            else None
        )
    else:
        if not bench_exe.exists():
            print(f"error: benchmark executable not found: {bench_exe}", file=sys.stderr)
            return 1
        with tempfile.TemporaryDirectory(prefix="pulp-design-import-bench-") as td:
            temp_dir = Path(td)
            live = run_lane(bench_exe, "live", args.idle_ms, args.interactive_ms, args.target_fps, temp_dir)
            baked = run_lane(bench_exe, "baked-native", args.idle_ms, args.interactive_ms, args.target_fps, temp_dir)
            baked_cpp = run_lane(bench_exe, "baked-cpp", args.idle_ms, args.interactive_ms, args.target_fps, temp_dir)
            baked_rebuild = (
                run_lane(
                    bench_exe,
                    "baked-native-rebuild",
                    args.idle_ms,
                    args.interactive_ms,
                    args.target_fps,
                    temp_dir,
                )
                if args.include_rebuild_lane
                else None
            )

    report = build_report(live, baked, binary_size, build_dir, baked_cpp, baked_rebuild)
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
