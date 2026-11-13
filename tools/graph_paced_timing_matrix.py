#!/usr/bin/env python3
"""Shared paced/deadline timing matrix for graph fixtures.

Runs the same fixed-origin paced-callback measurement over several named graph
fixtures and writes one versioned JSON receipt per run. Each fixture is an
existing Catch2 test executable with a hidden matrix-cell case (see
``test/harness/paced_timing_cell.hpp``); the G-12 activation fixture keeps its
own row format and is read through ``graph_performance_qualification``.

The C++ side only records raw per-callback steady-clock offsets. Everything a
verdict depends on (percentiles, deadline misses, thresholds, admission) is
computed here from those raw rows, and ``validate`` recomputes it again from
the retained raw files.

Two modes:

* ``short`` (``--dry-run``): a tiny callback count that exercises every cell
  and the receipt end to end. It can never claim timing qualification.
* ``qualification``: 100000 paced callbacks per cell on an admitted quiet
  host. The run is refused before any cell starts when the host's 1-minute
  load is above the admission threshold.

What a receipt can never prove: WCET, behaviour under a real audio device or
DAW callback thread, other hosts or platforms, or resource safety (timing runs
carry no allocation/lock probes; those are separate fixtures).
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import math
import os
import platform
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable, Iterable

SCHEMA = "pulp.graph_paced_timing_matrix_receipt.v1"
CELL_HEADER = "pulp_paced_timing_cell_v1"
QUALIFICATION_CALLBACKS = 100000
SHORT_CALLBACKS = 32
SHORT_CALLBACK_RANGE = (8, 512)
DEFAULT_MAX_LOAD_PER_CPU = 0.25
REFUSED_EXIT = 3

TIMING_RULE = (
    "timing_qualified requires mode=qualification, 100000 callbacks in every registered "
    "fixture cell, admission pass before and after (1-minute load <= max_load_per_cpu x "
    "logical CPUs), Release without sanitizer, clean source equal to every cell's compiled "
    "source, no planted fault, and for every cell: zero deadline misses (callback end later "
    "than target + one block), zero oracle failures, nearest-rank p99 callback duration < 70% "
    "of the block period and nearest-rank p99 absolute wake < 0.1 sample")

ADMISSION_RULE = ("admit when os.getloadavg()[0] <= max_load_per_cpu x os.cpu_count(); "
                  "qualification refuses to start otherwise and is disqualified when the "
                  "post-run sample also exceeds it; short mode only records the verdict")


class MatrixError(ValueError):
    """A matrix run or receipt is malformed or cannot prove what it claims."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise MatrixError(message)


@dataclasses.dataclass(frozen=True)
class Fixture:
    name: str
    executable: str | None  # relative to the build dir; None for the in-process reference
    test_case: str | None
    source: str
    variants: tuple[str, ...]
    rates: tuple[int, ...]
    frames: tuple[int, ...]
    row_format: str  # "cell" | "g12" | "reference"
    qualifying: bool = True
    planted_supported: bool = True


FIXTURES: dict[str, Fixture] = {f.name: f for f in (
    Fixture("g5-graph-event", "test/pulp-test-group-trigger-graph",
            "G5 graph event paced matrix cell", "test/fixtures/g5_graph_event_rt_cases.inc",
            ("direct", "routed"), (48000,), (64, 128, 256, 4096), "cell"),
    Fixture("g7-edge-probe", "test/pulp-test-signal-graph-executor-parity",
            "G7 edge probe paced matrix cell", "test/test_signal_graph_executor_parity.cpp",
            ("attached", "detached"), (48000,), (32, 64, 128, 256), "cell"),
    Fixture("g6-voice-bank", "test/pulp-test-voice-bank-binding",
            "G6 voice bank paced matrix cell", "test/test_voice_bank_binding.cpp",
            ("eight-slot",), (48000,), (32, 64, 128, 256), "cell"),
    Fixture("g10-routed-spectral", "test/pulp-test-routed-spectral-ports",
            "G10 routed spectral paced matrix cell", "test/test_routed_spectral_ports.cpp",
            ("identity-mask",), (48000,), (64, 128, 256, 4096), "cell"),
    Fixture("g12-activation", "test/pulp-test-group-trigger-graph",
            "G12 paced activation receipt cell", "test/fixtures/g5_graph_event_rt_cases.inc",
            ("direct", "routed"), (44100, 48000, 96000), (32, 64, 128, 256), "g12",
            planted_supported=False),
    # In-process control used by the self-test: proves the miss counter and the
    # receipt path without a C++ build. Never admissible for qualification.
    Fixture("python-reference", None, None, "tools/graph_paced_timing_matrix.py",
            ("reference",), (48000,), (4096,), "reference", qualifying=False),
)}
QUALIFYING_FIXTURES = tuple(name for name, f in FIXTURES.items() if f.qualifying)


# --------------------------------------------------------------------------- utils

def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def receipt_digest(receipt: dict[str, Any]) -> str:
    unsigned = {k: v for k, v in receipt.items() if k != "receipt_sha256"}
    return hashlib.sha256((json.dumps(unsigned, indent=2, sort_keys=True) + "\n").encode()).hexdigest()


def nearest_rank(values: list[int], fraction: float) -> int:
    _require(bool(values), "empty sample set")
    ordered = sorted(values)
    return ordered[max(1, math.ceil(len(ordered) * fraction)) - 1]


def _run_text(command: list[str]) -> str:
    try:
        return subprocess.run(command, check=True, capture_output=True, text=True,
                              timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def host_identity_class() -> dict[str, Any]:
    """Hardware/OS class only. Deliberately excludes hostname and user identity."""
    system = platform.system()
    identity: dict[str, Any] = {
        "os": system, "os_release": platform.release(), "machine": platform.machine(),
        "logical_cpus": os.cpu_count() or 0, "cpu_model": "", "hardware_model": "",
        "memory_gib": 0,
    }
    if system == "Darwin":
        identity["cpu_model"] = _run_text(["sysctl", "-n", "machdep.cpu.brand_string"])
        identity["hardware_model"] = _run_text(["sysctl", "-n", "hw.model"])
        memory = _run_text(["sysctl", "-n", "hw.memsize"])
        identity["memory_gib"] = round(int(memory) / 2**30) if memory.isdigit() else 0
    elif system == "Linux":
        try:
            for line in Path("/proc/cpuinfo").read_text().splitlines():
                if line.startswith("model name"):
                    identity["cpu_model"] = line.split(":", 1)[1].strip()
                    break
            for line in Path("/proc/meminfo").read_text().splitlines():
                if line.startswith("MemTotal:"):
                    identity["memory_gib"] = round(int(line.split()[1]) / 2**20)
                    break
        except OSError:
            pass
    return identity


def read_load() -> tuple[list[float], int]:
    try:
        load = [round(value, 2) for value in os.getloadavg()]
    except (AttributeError, OSError):
        load = []
    return load, os.cpu_count() or 0


def admission_verdict(load: list[float], ncpu: int, max_load_per_cpu: float) -> dict[str, Any]:
    if not load or ncpu <= 0 or not all(math.isfinite(v) for v in load):
        return {"load": load, "verdict": "unknown", "admitted": False, "reason": "load_unknown"}
    limit = max_load_per_cpu * ncpu
    admitted = load[0] <= limit
    return {"load": load, "verdict": "admit" if admitted else "refuse", "admitted": admitted,
            "reason": "" if admitted else f"load1 {load[0]} > {limit:g} ({max_load_per_cpu:g} x {ncpu} cpus)"}


# --------------------------------------------------------------------------- parsing

_TOKEN = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\S+)")
CELL_META_INT = ("rate", "frames", "callbacks", "planted_sleep_us", "planted_every", "build_dirty")
CELL_META_STR = ("fixture", "variant", "compiled_source", "build_type")


def parse_cell_output(stdout: str) -> dict[str, Any]:
    """Parse the shared ``pulp_paced_timing_cell_v1`` format into meta + raw rows."""
    lines = stdout.splitlines()
    _require(lines.count(CELL_HEADER) == 1, "cell header missing or duplicated")
    meta: dict[str, Any] | None = None
    rows: list[tuple[int, int, int, int]] = []
    end: dict[str, int] | None = None
    for line in lines:
        if line.startswith("PACED_CELL "):
            _require(meta is None, "duplicate PACED_CELL line")
            fields = dict(_TOKEN.findall(line))
            _require(all(k in fields for k in CELL_META_INT + CELL_META_STR), "PACED_CELL fields missing")
            meta = {k: fields.pop(k) for k in CELL_META_STR}
            try:
                meta.update({k: int(fields.pop(k)) for k in CELL_META_INT})
            except ValueError as exc:
                raise MatrixError("malformed PACED_CELL integer") from exc
            meta["extras"] = fields
        elif line.startswith("P "):
            parts = line.split()
            _require(len(parts) == 6, "malformed callback row")
            try:
                index, target, start, finish, ok = (int(p) for p in parts[1:])
            except ValueError as exc:
                raise MatrixError("malformed callback row integer") from exc
            _require(index == len(rows) and ok in (0, 1), "callback row index/ok mismatch")
            rows.append((target, start, finish, ok))
        elif line.startswith("PACED_END "):
            _require(end is None, "duplicate PACED_END line")
            try:
                end = {k: int(v) for k, v in _TOKEN.findall(line)}
            except ValueError as exc:
                raise MatrixError("malformed PACED_END") from exc
    _require(meta is not None and end is not None, "cell metadata or terminator missing")
    _require(end.get("rows") == len(rows) == meta["callbacks"], "callback row count mismatch")
    _require(end.get("oracle_failures") == sum(1 for r in rows if r[3] == 0),
             "oracle failure count disagrees with rows")
    for i, (target, _start, _end, _ok) in enumerate(rows):
        _require(target == i * meta["frames"] * 1_000_000_000 // meta["rate"],
                 "callback schedule drifted from fixed origin")
    return {"meta": meta, "rows": rows}


def parse_g12_output(stdout: str) -> dict[str, Any]:
    """Read the G-12 activation fixture through its own typed validator."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import graph_performance_qualification as gpq  # noqa: PLC0415  (lazy: avoids import cycle)
    oracle_ok = True
    try:
        cell = gpq.parse_activation_output(stdout)
        meta_in, callbacks = cell["metadata"], cell["callbacks"]
    except gpq.ReceiptError:
        # The audio/transition oracle failed; still read the raw timing rows so
        # the failure is reported against the same cell instead of dropped.
        oracle_ok = False
        meta_in, callbacks = None, []
        for line in stdout.splitlines():
            if line.startswith("G12_CELL "):
                meta_in = dict(_TOKEN.findall(line))
            elif line.startswith("G12_ROW "):
                callbacks.append({k: int(v) for k, v in _TOKEN.findall(line) if k != "bits"})
        _require(meta_in is not None and callbacks, "G12 cell metadata or rows missing")
    _require(len(callbacks) > 0, "G12 cell has no callback rows")
    origin = int(callbacks[0]["target_ns"])
    rate, frames = int(meta_in["rate"]), int(meta_in["frames"])
    rows = []
    for i, row in enumerate(callbacks):
        target = int(row["target_ns"]) - origin
        _require(abs(target - i * frames * 1_000_000_000 // rate) <= 1,
                 "G12 callback schedule drifted from fixed origin")
        rows.append((target, int(row["start_ns"]) - origin, int(row["end_ns"]) - origin,
                     1 if oracle_ok else 0))
    meta = {"fixture": "g12-activation", "variant": str(meta_in["backend"]), "rate": rate,
            "frames": frames, "callbacks": int(meta_in["callbacks"]), "planted_sleep_us": 0,
            "planted_every": 1, "build_dirty": int(meta_in["build_dirty"]),
            "compiled_source": str(meta_in["compiled_source"]),
            "build_type": str(meta_in["build_type"]), "extras": {"first_new": str(meta_in["first_new"])}}
    _require(len(rows) == meta["callbacks"], "G12 callback row count mismatch")
    return {"meta": meta, "rows": rows}


def compute_metrics(rows: list[tuple[int, int, int, int]], rate: int, frames: int) -> dict[str, Any]:
    _require(bool(rows), "cell has no callbacks")
    block_ns = frames * 1_000_000_000
    durations, wakes, misses, failures = [], [], 0, 0
    previous_end = None
    for target, start, end, ok in rows:
        _require(target <= start <= end, "callback timestamps inconsistent")
        _require(previous_end is None or previous_end <= start, "callbacks overlap")
        previous_end = end
        durations.append(end - start)
        wakes.append(abs(start - target))
        # Same rule as the G-12 lane: the block missed when it finished later
        # than one block period after its target.
        misses += (end - target) * rate > block_ns
        failures += ok == 0
    metrics = {
        "period_ns": block_ns // rate,
        "duration_ns": {"p50": nearest_rank(durations, .5), "p99": nearest_rank(durations, .99),
                        "max": max(durations)},
        "wake_ns": {"p50": nearest_rank(wakes, .5), "p99": nearest_rank(wakes, .99), "max": max(wakes)},
        "deadline_misses": misses,
        "oracle_failures": failures,
    }
    metrics["thresholds_met"] = bool(
        metrics["duration_ns"]["p99"] * rate * 10 < 7 * block_ns and
        metrics["wake_ns"]["p99"] * rate * 10 < 1_000_000_000 and
        misses == 0 and failures == 0)
    return metrics


def parse_raw(fixture: Fixture, stdout: str) -> dict[str, Any]:
    if fixture.row_format == "g12":
        return parse_g12_output(stdout)
    return parse_cell_output(stdout)


# --------------------------------------------------------------------------- running

def reference_cell_output(variant: str, rate: int, frames: int, callbacks: int,
                          planted_sleep_us: int, planted_every: int) -> str:
    """In-process fixed-origin loop emitting the shared cell format."""
    rows = []
    origin = time.monotonic_ns() + 10_000_000
    accumulator = 0.0
    for i in range(callbacks):
        target = i * frames * 1_000_000_000 // rate
        delay = origin + target - time.monotonic_ns()
        if delay > 0:
            time.sleep(delay / 1e9)
        start = time.monotonic_ns()
        for j in range(64):  # token workload
            accumulator += j * 0.5
        if planted_sleep_us and i % planted_every == 0:
            time.sleep(planted_sleep_us / 1e6)
        end = time.monotonic_ns()
        rows.append((target, start - origin, end - origin, 1 if math.isfinite(accumulator) else 0))
    out = [CELL_HEADER,
           f"PACED_CELL fixture=python-reference variant={variant} rate={rate} frames={frames} "
           f"callbacks={callbacks} planted_sleep_us={planted_sleep_us} planted_every={planted_every} "
           f"compiled_source=none build_type=python build_dirty=0"]
    out += [f"P {i} {t} {s} {e} {ok}" for i, (t, s, e, ok) in enumerate(rows)]
    out.append(f"PACED_END rows={len(rows)} oracle_failures={sum(1 for r in rows if r[3] == 0)}")
    return "\n".join(out) + "\n"


def expected_cell_seconds(rate: int, frames: int, callbacks: int) -> float:
    return callbacks * frames / rate


def cell_timeout(rate: int, frames: int, callbacks: int, planted_sleep_us: int,
                 planted_every: int) -> float:
    """Wall-clock budget for one cell: its paced schedule plus generous slack."""
    planted = math.ceil(callbacks / planted_every) * planted_sleep_us / 1e6
    return 1.5 * (expected_cell_seconds(rate, frames, callbacks) + planted) + 120.0


def _cell_env(fixture: Fixture, variant: str, rate: int, frames: int, callbacks: int,
              planted_sleep_us: int, planted_every: int) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith(("PULP_PACED_", "G12_"))}
    if fixture.row_format == "g12":
        env.update(G12_ACTIVATION_RATE=str(rate), G12_ACTIVATION_FRAMES=str(frames),
                   G12_ACTIVATION_CALLBACKS=str(callbacks), G12_ACTIVATION_BACKEND=variant)
    else:
        env.update(PULP_PACED_VARIANT=variant, PULP_PACED_RATE=str(rate),
                   PULP_PACED_FRAMES=str(frames), PULP_PACED_CALLBACKS=str(callbacks),
                   PULP_PACED_PLANTED_SLEEP_US=str(planted_sleep_us),
                   PULP_PACED_PLANTED_EVERY=str(planted_every))
    return env


def matrix_cells(fixtures: Iterable[str], frames_filter: set[int] | None) -> list[tuple[Fixture, str, int, int]]:
    cells = []
    for name in fixtures:
        fixture = FIXTURES[name]
        for variant in fixture.variants:
            for rate in fixture.rates:
                for frames in fixture.frames:
                    if frames_filter is None or frames in frames_filter:
                        cells.append((fixture, variant, rate, frames))
    return cells


def _source_state(root: Path) -> dict[str, Any]:
    sha = _run_text(["git", "-C", str(root), "rev-parse", "HEAD"])
    dirty = subprocess.run(["git", "-C", str(root), "diff", "--quiet", "HEAD", "--"],
                           capture_output=True).returncode != 0 if sha else True
    return {"sha": sha, "dirty": dirty}


def _build_profile(build: Path) -> dict[str, str]:
    profile = {"CMAKE_BUILD_TYPE": "", "PULP_SANITIZER": ""}
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            key, sep, value = line.partition("=")
            name = key.split(":", 1)[0]
            if sep and name in profile:
                profile[name] = value
    return profile


def disqualifiers_for(receipt: dict[str, Any]) -> list[str]:
    reasons = []
    if receipt["mode"] != "qualification":
        reasons.append("short_mode")
    if receipt["requested"]["callbacks"] != QUALIFICATION_CALLBACKS:
        reasons.append("callback_count_below_qualification")
    if sorted(receipt["requested"]["fixtures"]) != sorted(QUALIFYING_FIXTURES):
        reasons.append("fixture_set_incomplete_or_includes_reference")
    if receipt["requested"]["frames_filter"] is not None:
        reasons.append("frames_filtered")
    if receipt["planted"]["sleep_us"]:
        reasons.append("planted_fault")
    admission = receipt["admission"]
    if not admission["before"]["admitted"]:
        reasons.append("admission_before_failed")
    if not admission.get("after") or not admission["after"]["admitted"]:
        reasons.append("admission_after_failed")
    profile = receipt["build_profile"]
    if profile["CMAKE_BUILD_TYPE"] != "Release" or profile["PULP_SANITIZER"]:
        reasons.append("not_plain_release")
    if receipt["source"]["dirty"]:
        reasons.append("dirty_source")
    if receipt["status"] != "COMPLETE":
        reasons.append("run_not_complete")
    for cell in receipt["cells"]:
        tag = f"{cell['fixture']}/{cell['variant']}/{cell['rate']}/{cell['frames']}"
        if cell["returncode"] != 0:
            reasons.append(f"cell_failed:{tag}")
        if cell["compiled_source"] != receipt["source"]["sha"] or cell["build_dirty"]:
            reasons.append(f"stale_binary:{tag}")
        if cell["callbacks"] != receipt["requested"]["callbacks"]:
            reasons.append(f"short_cell:{tag}")
        if not cell["metrics"]["thresholds_met"]:
            reasons.append(f"thresholds_failed:{tag}")
    return reasons


def _finish(receipt: dict[str, Any]) -> dict[str, Any]:
    cells = receipt["cells"]
    receipt["totals"] = {
        "cells": len(cells), "callbacks": sum(c["callbacks"] for c in cells),
        "deadline_misses": sum(c["metrics"]["deadline_misses"] for c in cells),
        "oracle_failures": sum(c["metrics"]["oracle_failures"] for c in cells),
        "cells_meeting_thresholds": sum(1 for c in cells if c["metrics"]["thresholds_met"]),
    }
    receipt["disqualifiers"] = disqualifiers_for(receipt)
    receipt["timing_qualified"] = not receipt["disqualifiers"]
    receipt["receipt_sha256"] = receipt_digest(receipt)
    return receipt


def record_matrix(*, mode: str, source_root: Path, build_dir: Path, output_dir: Path,
                  fixtures: list[str], callbacks: int, frames_filter: set[int] | None,
                  planted_sleep_us: int, planted_every: int, max_load_per_cpu: float,
                  timeout_seconds: float | None,
                  load_reader: Callable[[], tuple[list[float], int]] = read_load) -> tuple[dict[str, Any], int]:
    _require(mode in ("short", "qualification"), "unknown mode")
    _require(all(name in FIXTURES for name in fixtures), "unknown fixture")
    if mode == "qualification":
        _require(callbacks == QUALIFICATION_CALLBACKS, "qualification requires 100000 callbacks per cell")
        _require(not planted_sleep_us, "qualification cannot carry a planted fault")
        _require(all(FIXTURES[name].qualifying for name in fixtures), "reference fixture is not admissible")
    else:
        lo, hi = SHORT_CALLBACK_RANGE
        _require(lo <= callbacks <= hi, f"short mode requires {lo}..{hi} callbacks")
    _require(planted_every >= 1 and planted_sleep_us >= 0, "invalid planted fault")
    if planted_sleep_us:
        _require(all(FIXTURES[n].planted_supported for n in fixtures),
                 "planted fault is not supported by the g12-activation row format")
    if any(FIXTURES[n].row_format == "g12" for n in fixtures):
        _require(callbacks >= 16, "g12-activation requires at least 16 callbacks")

    load, ncpu = load_reader()
    before = admission_verdict(load, ncpu, max_load_per_cpu)
    receipt: dict[str, Any] = {
        "schema": SCHEMA, "tool": "tools/graph_paced_timing_matrix.py", "mode": mode,
        "status": "RUNNING", "timing_rule": TIMING_RULE,
        "source": _source_state(source_root), "build_profile": _build_profile(build_dir),
        "host_identity_class": host_identity_class(),
        "admission": {"rule": ADMISSION_RULE, "max_load_per_cpu": max_load_per_cpu,
                      "logical_cpus": ncpu, "before": before, "after": None},
        "requested": {"fixtures": list(fixtures), "callbacks": callbacks,
                      "frames_filter": sorted(frames_filter) if frames_filter else None},
        "planted": {"sleep_us": planted_sleep_us, "every": planted_every},
        "cells": [],
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    if mode == "qualification" and not before["admitted"]:
        receipt["status"] = "REFUSED"
        _write(output_dir, _finish(receipt))
        return receipt, REFUSED_EXIT

    executable_hashes: dict[str, str] = {}
    for fixture, variant, rate, frames in matrix_cells(fixtures, frames_filter):
        stem = f"{fixture.name}-{variant}-{rate}-{frames}"
        raw_path = output_dir / f"{stem}.txt"
        if fixture.row_format == "reference":
            stdout = reference_cell_output(variant, rate, frames, callbacks, planted_sleep_us, planted_every)
            returncode, executable, exe_sha = 0, None, sha256_file(Path(__file__).resolve())
        else:
            executable = build_dir / fixture.executable
            _require(executable.is_file(), f"missing executable {executable}; build its target first")
            exe_sha = executable_hashes.setdefault(fixture.executable, sha256_file(executable))
            result = subprocess.run([str(executable), fixture.test_case],
                                    env=_cell_env(fixture, variant, rate, frames, callbacks,
                                                  planted_sleep_us, planted_every),
                                    capture_output=True, text=True,
                                    timeout=timeout_seconds or cell_timeout(
                                        rate, frames, callbacks, planted_sleep_us, planted_every))
            stdout, returncode = result.stdout, result.returncode
            # A rebuild during the run would silently mix binaries.
            _require(sha256_file(executable) == exe_sha, f"executable changed during run: {executable}")
        raw_path.write_text(stdout, encoding="utf-8")
        parsed = parse_raw(fixture, stdout)
        meta = parsed["meta"]
        _require((meta["fixture"], meta["variant"], meta["rate"], meta["frames"], meta["callbacks"]) ==
                 (fixture.name, variant, rate, frames, callbacks), f"cell identity mismatch for {stem}")
        receipt["cells"].append({
            "fixture": fixture.name, "variant": variant, "rate": rate, "frames": frames,
            "callbacks": meta["callbacks"], "test_case": fixture.test_case,
            "executable": fixture.executable, "executable_sha256": exe_sha,
            "source_file": fixture.source, "compiled_source": meta["compiled_source"],
            "build_type": meta["build_type"], "build_dirty": meta["build_dirty"],
            "extras": meta["extras"], "returncode": returncode,
            "raw_stdout": {"path": raw_path.name, "sha256": sha256_file(raw_path)},
            "metrics": compute_metrics(parsed["rows"], rate, frames),
        })
    load, ncpu = load_reader()
    receipt["admission"]["after"] = admission_verdict(load, ncpu, max_load_per_cpu)
    receipt["status"] = "COMPLETE"
    _write(output_dir, _finish(receipt))
    return receipt, 0


def _write(output_dir: Path, receipt: dict[str, Any]) -> Path:
    path = output_dir / "receipt.json"
    path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


# --------------------------------------------------------------------------- validation

REQUIRED_TOP = {"schema", "tool", "mode", "status", "timing_rule", "source", "build_profile",
                "host_identity_class", "admission", "requested", "planted", "cells", "totals",
                "disqualifiers", "timing_qualified", "receipt_sha256"}
REQUIRED_CELL = {"fixture", "variant", "rate", "frames", "callbacks", "test_case", "executable",
                 "executable_sha256", "source_file", "compiled_source", "build_type", "build_dirty",
                 "extras", "returncode", "raw_stdout", "metrics"}


def validate_receipt(path: Path) -> dict[str, Any]:
    """Recompute every verdict from the retained raw rows and the receipt's own inputs."""
    receipt = json.loads(path.read_text(encoding="utf-8"))
    _require(receipt.get("schema") == SCHEMA, "not a paced timing matrix receipt")
    _require(set(receipt) == REQUIRED_TOP, "receipt top-level fields mismatch")
    _require(receipt["receipt_sha256"] == receipt_digest(receipt), "receipt digest mismatch")
    _require(receipt["timing_rule"] == TIMING_RULE, "timing rule text changed")
    _require("hostname" not in json.dumps(receipt["host_identity_class"]).lower(),
             "host identity must be a class, not a hostname")
    _require(receipt["status"] in ("COMPLETE", "REFUSED"), "unknown status")
    if receipt["status"] == "REFUSED":
        _require(not receipt["cells"] and not receipt["admission"]["before"]["admitted"],
                 "refused receipt must have no cells and a refused admission")
    seen = set()
    for cell in receipt["cells"]:
        _require(set(cell) == REQUIRED_CELL, "cell fields mismatch")
        fixture = FIXTURES.get(cell["fixture"])
        _require(fixture is not None and cell["variant"] in fixture.variants and
                 cell["rate"] in fixture.rates and cell["frames"] in fixture.frames,
                 "unexpected matrix cell")
        key = (cell["fixture"], cell["variant"], cell["rate"], cell["frames"])
        _require(key not in seen, "duplicate matrix cell")
        seen.add(key)
        raw = path.parent / cell["raw_stdout"]["path"]
        _require(Path(cell["raw_stdout"]["path"]).name == cell["raw_stdout"]["path"] and raw.is_file(),
                 "raw output missing or outside the receipt directory")
        _require(sha256_file(raw) == cell["raw_stdout"]["sha256"], "raw output digest mismatch")
        parsed = parse_raw(fixture, raw.read_text(encoding="utf-8"))
        _require(parsed["meta"]["callbacks"] == cell["callbacks"], "cell callback count mismatch")
        _require(compute_metrics(parsed["rows"], cell["rate"], cell["frames"]) == cell["metrics"],
                 "cell metrics disagree with raw rows")
    if receipt["status"] == "COMPLETE":
        expected = {(f.name, v, r, fr) for f, v, r, fr in matrix_cells(
            receipt["requested"]["fixtures"],
            set(receipt["requested"]["frames_filter"]) if receipt["requested"]["frames_filter"] else None)}
        _require(seen == expected, "matrix incomplete")
    recomputed = disqualifiers_for(receipt)
    _require(receipt["disqualifiers"] == recomputed, "disqualifiers disagree with receipt")
    _require(receipt["timing_qualified"] is (not recomputed), "timing_qualified disagrees with rule")
    return receipt


# --------------------------------------------------------------------------- cli

def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = root.add_subparsers(dest="command", required=True)
    rec = sub.add_parser("record", help="run the paced matrix and write receipt.json")
    rec.add_argument("--source-root", type=Path, default=Path("."))
    rec.add_argument("--build-dir", type=Path, default=Path("build"))
    rec.add_argument("--output-dir", type=Path, required=True)
    rec.add_argument("--mode", choices=("short", "qualification"), default="short")
    rec.add_argument("--dry-run", action="store_true",
                     help="alias for --mode short with the default tiny callback count")
    rec.add_argument("--fixtures", nargs="+", choices=sorted(FIXTURES), default=list(QUALIFYING_FIXTURES))
    rec.add_argument("--callbacks", type=int)
    rec.add_argument("--frames", type=int, nargs="+", help="restrict to these block sizes (never qualifies)")
    rec.add_argument("--planted-sleep-us", type=int, default=0,
                     help="planted negative: sleep inside the timed callback window")
    rec.add_argument("--planted-every", type=int, default=1)
    rec.add_argument("--max-load-per-cpu", type=float, default=DEFAULT_MAX_LOAD_PER_CPU)
    rec.add_argument("--plan", action="store_true",
                     help="print the cells and expected paced wall time, then exit")
    rec.add_argument("--timeout-seconds", type=float, default=None,
                     help="per-cell timeout; default scales with the cell's paced schedule")
    val = sub.add_parser("validate", help="recompute and check a matrix receipt")
    val.add_argument("receipt", type=Path)
    return root


def summary_lines(receipt: dict[str, Any]) -> list[str]:
    lines = [f"matrix status={receipt['status']} mode={receipt['mode']} "
             f"timing_qualified={str(receipt['timing_qualified']).lower()} "
             f"admission_before={receipt['admission']['before']['verdict']} "
             f"load_before={receipt['admission']['before']['load']}"]
    for cell in receipt["cells"]:
        m = cell["metrics"]
        lines.append(f"cell {cell['fixture']}/{cell['variant']} rate={cell['rate']} frames={cell['frames']} "
                     f"callbacks={cell['callbacks']} misses={m['deadline_misses']} "
                     f"oracle_failures={m['oracle_failures']} p50_us={m['duration_ns']['p50'] / 1000:.1f} "
                     f"p99_us={m['duration_ns']['p99'] / 1000:.1f} max_us={m['duration_ns']['max'] / 1000:.1f} "
                     f"period_us={m['period_ns'] / 1000:.1f}")
    lines.append(f"totals {json.dumps(receipt['totals'], sort_keys=True)}")
    lines.append(f"disqualifiers={','.join(receipt['disqualifiers']) or 'none'}")
    return lines


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(list(argv) if argv is not None else None)
    try:
        if args.command == "validate":
            receipt = validate_receipt(args.receipt)
            print(f"matrix_receipt=pass timing_qualified={str(receipt['timing_qualified']).lower()}")
            return 0
        mode = "short" if args.dry_run else args.mode
        callbacks = args.callbacks or (QUALIFICATION_CALLBACKS if mode == "qualification" else SHORT_CALLBACKS)
        if args.plan:
            cells = matrix_cells(dict.fromkeys(args.fixtures), set(args.frames) if args.frames else None)
            total = 0.0
            for fixture, variant, rate, frames in cells:
                seconds = expected_cell_seconds(rate, frames, callbacks)
                total += seconds
                print(f"plan {fixture.name}/{variant} rate={rate} frames={frames} "
                      f"callbacks={callbacks} paced_seconds={seconds:.0f}")
            print(f"plan cells={len(cells)} paced_seconds={total:.0f} paced_hours={total / 3600:.2f}")
            return 0
        receipt, code = record_matrix(
            mode=mode, source_root=args.source_root.resolve(), build_dir=args.build_dir.resolve(),
            output_dir=args.output_dir.resolve(), fixtures=list(dict.fromkeys(args.fixtures)),
            callbacks=callbacks, frames_filter=set(args.frames) if args.frames else None,
            planted_sleep_us=args.planted_sleep_us, planted_every=args.planted_every,
            max_load_per_cpu=args.max_load_per_cpu, timeout_seconds=args.timeout_seconds)
        for line in summary_lines(receipt):
            print(line)
        print(f"receipt={args.output_dir.resolve() / 'receipt.json'}")
        if code == REFUSED_EXIT:
            print(f"refused: {receipt['admission']['before']['reason']}", file=sys.stderr)
        return code
    except (MatrixError, OSError, subprocess.SubprocessError, ValueError, KeyError) as exc:
        print(f"matrix_error={exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
