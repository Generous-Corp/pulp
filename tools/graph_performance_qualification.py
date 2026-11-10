#!/usr/bin/env python3
"""Record and validate the bounded G-12/R2 graph benchmark slice.

This tool wraps the existing ``pulp-graph-r2-measurement`` executable.  It is
deliberately a receipt and control harness, not a realtime qualification
runner: the executable still owns the graph fixture and its allocation probe,
while this layer binds the run to its source/build/artifacts and checks the
complete 40-cell matrix.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "pulp.graph_r2_qualification_receipt.v2"
FIXTURE_VERSION = "graph_r2_measurement_v2"
SHAPES = ("passthrough", "chain", "fan-out")
SCALES = (1, 4, 16, 64)
BLOCKS = (16, 32, 64, 128, 512)
RUN_CONSTANTS = {
    "warmup": 500,
    "iterations": 100000,
    "samples": 31,
}
PHASES = ("compile", "snapshot_prepare", "pool_allocate")
SUMMARY_FIELDS = ("min_ns", "p50_ns", "p99_sample_ns", "max_ns", "spread_ns")
MEMORY_FIELDS = ("node_count", "edge_count", "logical_port_slots", "no_reuse_slots",
                 "scratch_slots", "reused_slots", "sample_bytes", "scratch_payload_bytes",
                 "no_reuse_payload_bytes")
MEASUREMENT_CONTRACT = {
    "compile": "build_graph_runtime_plan only; authored nodes, edges and bindings excluded",
    "snapshot_prepare": "GraphRuntimeSnapshot.reset only; off-thread preparation, not live activation",
    "pool_allocate": "fresh GraphRuntimeBufferPool.reset only; allocation and zero fill",
    "allocation_bytes": "requested C++ new bytes per phase, including temporaries; not retained memory or RSS",
    "memory": "mono float scratch payload only; excludes graph metadata, host buffers and allocator overhead",
    "reuse": "serial assigned slots versus public reuse-disabled assignment for the same plan",
    "timing": "31 unpaced 100000-block batch means after empty-loop subtraction; not callback percentiles, jitter or WCET",
    "scaling": "fixed topology matrix; per-cell counts and measures, no cross-run speedup claim",
    "live_activation": "not measured; requires the publication and paced callback lane",
}
TIMED_ALLOCATION_FIELDS = ("timed_allocations", "timed_allocation_samples")
FIXTURE_SOURCE = Path("test/test_graph_r2_measurement.cpp")


class ReceiptError(ValueError):
    """A receipt is malformed or does not prove the bounded slice."""


def expected_keys() -> tuple[tuple[str, int, int], ...]:
    return tuple(
        (shape, scale, block)
        for shape in SHAPES
        for scale in SCALES
        if not (shape == "passthrough" and scale != 1)
        and not (shape == "fan-out" and scale == 1)
        for block in BLOCKS
    )


EXPECTED_KEYS = frozenset(expected_keys())


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_bytes(value: dict[str, Any]) -> bytes:
    unsigned = {key: item for key, item in value.items() if key != "receipt_sha256"}
    return (json.dumps(unsigned, indent=2, sort_keys=True, separators=(",", ": ")) + "\n").encode()


def receipt_digest(receipt: dict[str, Any]) -> str:
    return hashlib.sha256(canonical_bytes(receipt)).hexdigest()


def _float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _bits_float(value: int) -> float:
    return struct.unpack("<f", struct.pack("<I", value))[0]


def _ordered_float_bits(value: int) -> int:
    """Map IEEE-754 float32 bits to monotonically ordered integer bits."""
    return (~value & 0xFFFFFFFF) if value & 0x80000000 else (value | 0x80000000)


def expected_output_bits(shape: str, scale: int) -> int:
    # This mirrors fixed_input(frame=0) and the fixture's fan-out reduction.
    value = _float32(-0.73)
    if shape == "fan-out":
        value = _float32(value * _float32(float(scale)))
    return struct.unpack("<I", struct.pack("<f", value))[0]


_TOKEN = re.compile(r"(?P<key>[A-Za-z_][A-Za-z0-9_-]*)=(?P<value>[^\s]+)")


def _tokens(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for match in _TOKEN.finditer(line):
        name = match.group("key")
        _require(name not in fields, f"duplicate raw field {name}")
        fields[name] = match.group("value")
    return fields


def summarize(samples: list[float]) -> dict[str, float]:
    _require(len(samples) == RUN_CONSTANTS["samples"], "measurement sample count mismatch")
    _require(all(type(value) in (int, float) and math.isfinite(value) and value >= 0
                 for value in samples), "measurement samples must be finite and nonnegative")
    ordered = sorted(samples)
    return dict(zip(SUMMARY_FIELDS, (ordered[0], ordered[len(ordered) // 2],
                                    ordered[math.ceil(len(ordered) * .99) - 1],
                                    ordered[-1], ordered[-1] - ordered[0])))


def expected_memory(shape: str, scale: int, block: int) -> dict[str, int]:
    # Independent topology arithmetic: one host input/output, scale mono
    # processors, and either serial edges or two edges per fan-out branch.
    logical = 2 * scale + 2
    slots = scale + 2 if shape == "fan-out" else 3
    return dict(zip(MEMORY_FIELDS, (scale + 2, 2 * scale if shape == "fan-out" else scale + 1,
                                   logical, logical, slots, logical - slots, 4,
                                   slots * block * 4, logical * block * 4)))


def parse_output(stdout: str) -> list[dict[str, Any]]:
    """Parse v2 phase samples, memory controls and unpaced batch means."""
    cells: dict[tuple[str, int, int], dict[str, Any]] = {}
    header_seen = False
    overhead_ns: float | None = None
    for line_number, line in enumerate(stdout.splitlines(), 1):
        if line.strip() == FIXTURE_VERSION:
            _require(not header_seen, "duplicate fixture header")
            header_seen = True
            continue
        fields = _tokens(line)
        if line.startswith("graph_direct_copy_benchmark_v2"):
            _require(overhead_ns is None, "duplicate timer overhead")
            try:
                overhead_ns = float(fields["timer_overhead_ns"])
            except (KeyError, ValueError) as exc:
                raise ReceiptError(f"line {line_number}: malformed timer overhead") from exc
            _require(math.isfinite(overhead_ns) and overhead_ns >= 0, "invalid timer overhead")
            continue
        if not line.strip():
            continue
        try:
            key = (fields["shape"], int(fields["scale"]), int(fields["block"]))
            _require(key in EXPECTED_KEYS, f"unexpected matrix cell {key!r}")
            cell = cells.setdefault(key, {"shape": key[0], "scale": key[1], "block": key[2]})
            if "oracle" in fields:
                _require("timing" not in cell, f"duplicate timing row for {key!r}")
                required = {"shape", "scale", "block", "warmup", "iterations", "samples", "oracle",
                            "process_allocations", *TIMED_ALLOCATION_FIELDS, "output_bits",
                            "batch_mean_samples_ns"}
                _require(set(fields) == required, "malformed timing row fields")
                samples = [float(value) for value in fields["batch_mean_samples_ns"].split(",")]
                cell["timing"] = {"batch_mean_samples_ns": samples, **summarize(samples)}
                for name in ("warmup", "iterations", "samples", "process_allocations",
                             *TIMED_ALLOCATION_FIELDS):
                    cell[name] = int(fields[name])
                cell["oracle"] = fields["oracle"]
                cell["output_bits"] = int(fields["output_bits"], 16)
            else:
                _require("preparation" not in cell, f"duplicate preparation row for {key!r}")
                required = {"shape", "scale", "block", *MEMORY_FIELDS,
                            *(f"{phase}_{field}" for phase in PHASES
                              for field in ("samples_ns", "allocation_bytes"))}
                _require(set(fields) == required, "malformed preparation or memory row fields")
                cell["preparation"] = {}
                for phase in PHASES:
                    samples = [float(value) for value in fields[f"{phase}_samples_ns"].split(",")]
                    allocated = [int(value) for value in fields[f"{phase}_allocation_bytes"].split(",")]
                    cell["preparation"][phase] = {"samples_ns": samples,
                                                  "allocation_bytes": allocated, **summarize(samples)}
                cell["memory"] = {name: int(fields[name]) for name in MEMORY_FIELDS}
        except (KeyError, ValueError) as exc:
            raise ReceiptError(f"line {line_number}: {exc}") from exc
    _require(header_seen and overhead_ns is not None, "benchmark header or timer overhead is missing")
    _require(frozenset(cells) == EXPECTED_KEYS, "matrix is incomplete")
    for key, cell in cells.items():
        cell["timer_overhead_ns"] = overhead_ns
        _validate_output_cell(cell, key)
    return [cells[key] for key in sorted(cells)]


def _validate_output_cell(cell: dict[str, Any], key: tuple[str, int, int]) -> None:
    preparation = cell.get("preparation")
    _require(isinstance(preparation, dict) and set(preparation) == set(PHASES),
             f"preparation phases incomplete for {key!r}")
    memory = cell.get("memory")
    _require(isinstance(memory, dict) and set(memory) == set(MEMORY_FIELDS), "memory fields incomplete")
    _require(all(type(value) is int for value in memory.values()), "memory counts must be integers")
    _require(memory == expected_memory(*key), f"memory/reuse topology mismatch for {key!r}")
    for phase, measured in preparation.items():
        _require(isinstance(measured, dict) and
                 set(measured) == {"samples_ns", "allocation_bytes", *SUMMARY_FIELDS},
                 f"{phase} fields incomplete")
        samples = measured["samples_ns"]
        _require(isinstance(samples, list), "phase samples missing")
        summary = summarize(samples)
        _require(all(type(measured[name]) in (int, float) and measured[name] == value
                     for name, value in summary.items()), f"{phase} timing summary mismatch")
        allocated = measured["allocation_bytes"]
        _require(isinstance(allocated, list) and len(allocated) == RUN_CONSTANTS["samples"] and
                 all(type(value) is int and value > 0 for value in allocated),
                 f"{phase} allocation bytes invalid")
        if phase == "pool_allocate":
            _require(all(value == memory["scratch_payload_bytes"] for value in allocated),
                     "pool allocation bytes disagree with scratch payload")
    timing = cell.get("timing")
    _require(isinstance(timing, dict) and set(timing) == {"batch_mean_samples_ns", *SUMMARY_FIELDS},
             "timing fields incomplete")
    _require(isinstance(timing["batch_mean_samples_ns"], list), "batch means missing")
    summary = summarize(timing["batch_mean_samples_ns"])
    _require(all(type(timing[name]) in (int, float) and timing[name] == value
                 for name, value in summary.items()), "batch mean timing summary mismatch")
    for name in ("warmup", "iterations", "samples", "process_allocations", *TIMED_ALLOCATION_FIELDS):
        _require(type(cell.get(name)) is int and cell[name] >= 0, f"invalid count field {name!r}")
    _require(all(cell[name] == value for name, value in RUN_CONSTANTS.items()), "run constants mismatch")
    _require(cell["timed_allocation_samples"] == cell["samples"], "timed allocation samples mismatch")


def _cache_values(build_dir: Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    values: dict[str, str] = {}
    if not cache.is_file():
        return values
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("//") or line.startswith("#") or ":" not in line:
            continue
        name, value = line.split("=", 1)
        values[name.split(":", 1)[0]] = value
    return values


def _source_sha(root: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True, encoding="utf-8",
    )
    return result.stdout.strip()


def _verify_committed_fixture(root: Path, source_sha: str, digest: str) -> None:
    result = subprocess.run(["git", "-C", str(root), "show", f"{source_sha}:{FIXTURE_SOURCE.as_posix()}"],
                            check=True, capture_output=True)
    _require(hashlib.sha256(result.stdout).hexdigest() == digest,
             "source commit does not contain the recorded fixture bytes")


def _relative_path(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError as exc:
        raise ReceiptError(f"path is outside source root: {path}") from exc


def make_receipt(
    *, root: Path, build_dir: Path, executable: Path, stdout_path: Path,
    stderr_path: Path, returncode: int, stdout: str,
) -> dict[str, Any]:
    values = _cache_values(build_dir)
    compiler = values.get("CMAKE_CXX_COMPILER", "unknown")
    compiler_id = values.get("CMAKE_CXX_COMPILER_ID", "unknown")
    compiler_version = values.get("CMAKE_CXX_COMPILER_VERSION", "unknown")
    fixture_source = root / FIXTURE_SOURCE
    _require(fixture_source.is_file(), f"fixture source does not exist: {fixture_source}")
    cells = parse_output(stdout)
    timer_overhead_ns = cells[0]["timer_overhead_ns"]
    for cell in cells:
        cell.pop("timer_overhead_ns", None)
    for cell in cells:
        cell["expected_output_bits"] = expected_output_bits(cell["shape"], cell["scale"])
    receipt: dict[str, Any] = {
        "schema": SCHEMA,
        "fixture": FIXTURE_VERSION,
        "status": "PARTIAL",
        "qualification_scope": "R2 bounded graph runtime measurement",
        "measurement_contract": MEASUREMENT_CONTRACT,
        "qualification_boundary": (
            "This receipt does not qualify callback deadlines, WCET, cross-platform RT safety, "
            "hosted plugins, fleet execution, or final G-12 flagship coverage."
        ),
        "source_sha": _source_sha(root),
        "source_fixture": {
            "path": FIXTURE_SOURCE.as_posix(),
            "sha256": sha256_file(fixture_source),
        },
        "executable": _relative_path(executable, root),
        "executable_sha256": sha256_file(executable),
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
        },
        "compiler": {
            "path": compiler,
            "id": compiler_id,
            "version": compiler_version,
        },
        "build_profile": {
            key: values.get(key, "unknown")
            for key in (
                "CMAKE_BUILD_TYPE", "PULP_ENABLE_GPU", "PULP_BUILD_EXAMPLES",
                "PULP_GRAPH_DIRECT_COPY_BENCHMARK",
            )
        },
        "matrix": {
            "shapes": list(SHAPES),
            "scales": list(SCALES),
            "blocks": list(BLOCKS),
            "expected_cell_count": len(EXPECTED_KEYS),
            "cells": cells,
        },
        "run": {
            "returncode": returncode,
            "warmup_blocks": RUN_CONSTANTS["warmup"],
            "timed_blocks": RUN_CONSTANTS["iterations"],
            "timing_samples": RUN_CONSTANTS["samples"],
            "timer_overhead_ns": timer_overhead_ns,
        },
        "artifacts": {
            "raw_stdout": {
                "path": _relative_path(stdout_path, stdout_path.parent),
                "sha256": sha256_file(stdout_path),
            },
            "raw_stderr": {
                "path": _relative_path(stderr_path, stderr_path.parent),
                "sha256": sha256_file(stderr_path),
            },
            "executable": {
                "path": "benchmark-executable",
                "sha256": sha256_file(executable),
            },
            "source_fixture": {
                "path": "source-fixture.cpp",
                "sha256": sha256_file(fixture_source),
            },
        },
    }
    receipt["receipt_sha256"] = receipt_digest(receipt)
    return receipt


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ReceiptError(message)


def validate_receipt(receipt_path: Path, source_root: Path | None = None) -> None:
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReceiptError(f"cannot read receipt: {receipt_path}") from exc
    if receipt.get("schema") == ACTIVATION_SCHEMA:
        validate_activation_receipt(receipt_path, receipt, source_root)
        return
    _require(receipt.get("schema") == SCHEMA, "unexpected receipt schema")
    _require(receipt.get("fixture") == FIXTURE_VERSION, "unexpected fixture version")
    _require(receipt.get("status") == "PARTIAL", "R2 receipt must remain PARTIAL")
    _require(receipt.get("measurement_contract") == MEASUREMENT_CONTRACT, "measurement contract mismatch")
    _require(receipt.get("receipt_sha256") == receipt_digest(receipt), "receipt digest mismatch")
    source_sha = receipt.get("source_sha")
    _require(isinstance(source_sha, str) and re.fullmatch(r"[0-9a-f]{40,64}", source_sha) is not None,
             "source_sha must be a full git object id")
    executable_sha = receipt.get("executable_sha256")
    _require(isinstance(executable_sha, str) and re.fullmatch(r"[0-9a-f]{64}", executable_sha) is not None,
             "executable_sha256 must be a SHA-256")
    source_fixture = receipt.get("source_fixture")
    _require(isinstance(source_fixture, dict), "source fixture provenance is missing")
    _require(isinstance(source_fixture.get("path"), str) and
             re.fullmatch(r"test/test_graph_r2_measurement\.cpp", source_fixture["path"]),
             "source fixture path is not bound to the benchmark fixture")
    _require(isinstance(source_fixture.get("sha256"), str) and
             re.fullmatch(r"[0-9a-f]{64}", source_fixture["sha256"]),
             "source fixture hash is missing")
    matrix = receipt.get("matrix")
    _require(isinstance(matrix, dict), "matrix is missing")
    _require(matrix.get("shapes") == list(SHAPES), "matrix shape dimensions mismatch")
    _require(matrix.get("scales") == list(SCALES), "matrix scale dimensions mismatch")
    _require(matrix.get("blocks") == list(BLOCKS), "matrix block dimensions mismatch")
    _require(matrix.get("expected_cell_count") == len(EXPECTED_KEYS),
             "matrix expected cell count mismatch")
    cells = matrix.get("cells")
    _require(isinstance(cells, list) and len(cells) == len(EXPECTED_KEYS), "matrix cell count mismatch")
    seen: set[tuple[str, int, int]] = set()
    for cell in cells:
        _require(isinstance(cell, dict), "matrix cell is not an object")
        try:
            key = (cell["shape"], int(cell["scale"]), int(cell["block"]))
        except (KeyError, TypeError, ValueError) as exc:
            raise ReceiptError("matrix cell key is malformed") from exc
        _require(key in EXPECTED_KEYS, f"unexpected matrix cell {key!r}")
        _require(key not in seen, f"duplicate matrix cell {key!r}")
        seen.add(key)
        _require(cell.get("oracle") == "pass", f"oracle did not pass for {key!r}")
        _require(cell.get("process_allocations") == 0,
                 f"process allocations were nonzero for {key!r}")
        actual_bits = cell.get("output_bits")
        expected_bits = cell.get("expected_output_bits")
        _require(isinstance(actual_bits, int) and isinstance(expected_bits, int) and
                 0 <= actual_bits <= 0xFFFFFFFF and 0 <= expected_bits <= 0xFFFFFFFF,
                 f"output oracle fields are malformed for {key!r}")
        _require(cell.get("expected_output_bits") == expected_output_bits(key[0], key[1]),
                 f"fixture expected output mismatch for {key!r}")
        max_ulp = max(1, key[1] // 8) if key[0] == "fan-out" else 0
        _require(abs(_ordered_float_bits(actual_bits) - _ordered_float_bits(expected_bits)) <= max_ulp,
                 f"deterministic output oracle mismatch for {key!r}")
        _validate_output_cell(cell, key)
        _require(cell.get("timed_allocations") == 0,
                 f"timed allocations were nonzero for {key!r}")
    _require(frozenset(seen) == EXPECTED_KEYS, "matrix does not cover the expected cells")
    run = receipt.get("run")
    _require(isinstance(run, dict), "run metadata is missing")
    _require(run.get("returncode") == 0, "benchmark did not exit successfully")
    _require(run.get("warmup_blocks") == RUN_CONSTANTS["warmup"], "warmup constant mismatch")
    _require(run.get("timed_blocks") == RUN_CONSTANTS["iterations"], "timed block constant mismatch")
    _require(run.get("timing_samples") == RUN_CONSTANTS["samples"], "timing sample constant mismatch")
    _require(isinstance(run.get("timer_overhead_ns"), (int, float)) and
             math.isfinite(run["timer_overhead_ns"]) and run["timer_overhead_ns"] >= 0,
             "timer overhead is invalid")
    profile = receipt.get("build_profile")
    _require(isinstance(profile, dict), "build profile is missing")
    _require(profile.get("CMAKE_BUILD_TYPE") == "Release", "receipt is not a Release build")
    _require(profile.get("PULP_GRAPH_DIRECT_COPY_BENCHMARK") == "ON",
             "receipt does not prove the benchmark build option")
    artifacts = receipt.get("artifacts")
    _require(isinstance(artifacts, dict), "artifacts are missing")
    for name in ("raw_stdout", "raw_stderr", "source_fixture", "executable"):
        artifact = artifacts.get(name)
        _require(isinstance(artifact, dict), f"{name} artifact is missing")
        relative = artifact.get("path")
        _require(isinstance(relative, str) and not os.path.isabs(relative),
                 f"{name} path must be relative")
        path = (receipt_path.parent / relative).resolve()
        _require(path.parent == receipt_path.parent.resolve(), f"{name} escapes receipt directory")
        _require(path.is_file(), f"{name} artifact is missing on disk")
        _require(artifact.get("sha256") == sha256_file(path), f"{name} digest mismatch")
        if name == "executable":
            _require(artifact.get("sha256") == executable_sha, "executable artifact hash is not bound")
        if name == "source_fixture":
            _require(artifact.get("sha256") == source_fixture["sha256"],
                     "source fixture artifact is not bound to recorded fixture hash")
    if source_root is not None:
        _verify_committed_fixture(source_root, source_sha, source_fixture["sha256"])
    raw_stdout_path = (receipt_path.parent / artifacts["raw_stdout"]["path"]).resolve()
    parsed_cells = parse_output(raw_stdout_path.read_text(encoding="utf-8"))
    parsed_timer_overhead = parsed_cells[0]["timer_overhead_ns"]
    for cell in parsed_cells:
        cell.pop("timer_overhead_ns", None)
    _require(parsed_timer_overhead == run["timer_overhead_ns"],
             "raw stdout timer overhead does not match receipt")
    expected_cells = {(
        cell["shape"], int(cell["scale"]), int(cell["block"])): cell for cell in parsed_cells
    }
    for recorded in cells:
        key = (recorded["shape"], int(recorded["scale"]), int(recorded["block"]))
        expected = expected_cells[key]
        expected["expected_output_bits"] = expected_output_bits(*key[:2])
        _require(recorded == expected, f"receipt cell does not match raw stdout for {key!r}")


def _copy_receipt_directory(receipt_path: Path, destination: Path) -> Path:
    destination.mkdir(parents=True)
    for source in receipt_path.parent.iterdir():
        if source.is_file():
            shutil.copy2(source, destination / source.name)
    return destination / receipt_path.name


def run_controls(receipt_path: Path, source_root: Path | None = None) -> None:
    if json.loads(receipt_path.read_text()).get("schema") == ACTIVATION_SCHEMA:
        run_activation_controls(receipt_path, source_root)
        return
    validate_receipt(receipt_path)
    original = json.loads(receipt_path.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="pulp-g12-r2-controls-") as temporary:
        root = Path(temporary)
        # Each negative is re-signed: the validator must reject the semantic
        # mutation, rather than merely noticing a stale receipt digest.
        for name, mutate in (
            ("expected_output_cell", lambda value: value["matrix"]["cells"][0].__setitem__(
                "expected_output_bits", 0)),
            ("allocation_count", lambda value: value["matrix"]["cells"][0].__setitem__(
                "process_allocations", 1)),
            ("timed_allocation_count", lambda value: value["matrix"]["cells"][0].__setitem__(
                "timed_allocations", 1)),
            ("timing_order", lambda value: value["matrix"]["cells"][0]["timing"].__setitem__(
                "p99_sample_ns", -1)),
        ):
            case_dir = root / name
            case_path = _copy_receipt_directory(receipt_path, case_dir)
            mutated = json.loads(json.dumps(original))
            mutate(mutated)
            mutated["receipt_sha256"] = receipt_digest(mutated)
            case_path.write_text(json.dumps(mutated, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            try:
                validate_receipt(case_path)
            except ReceiptError:
                pass
            else:
                raise ReceiptError(f"planted negative passed: {name}")
        # Change raw and parsed cells together, updating both hashes. These
        # controls exercise the semantic checks rather than raw/receipt equality.
        for name, section, field, value in (
            ("false_memory", "memory", "scratch_payload_bytes", 0),
            ("omitted_memory", "memory", "scratch_payload_bytes", None),
            ("false_reuse", "memory", "reused_slots", 999),
            ("omitted_reuse", "memory", "reused_slots", None),
            ("false_phase_timing", "compile", "samples_ns", [-1.0] * RUN_CONSTANTS["samples"]),
            ("omitted_phase_timing", "compile", "samples_ns", None),
            ("false_batch_timing", "timing", "batch_mean_samples_ns", [-1.0] * RUN_CONSTANTS["samples"]),
            ("omitted_batch_timing", "timing", "batch_mean_samples_ns", None),
            ("false_pool_bytes", "pool_allocate", "allocation_bytes", [1] * RUN_CONSTANTS["samples"]),
        ):
            case_path = _copy_receipt_directory(receipt_path, root / name)
            mutated = json.loads(json.dumps(original))
            cell = mutated["matrix"]["cells"][0]
            container = cell[section] if section in ("memory", "timing") else cell["preparation"][section]
            if value is None:
                del container[field]
            else:
                container[field] = value
            raw_field = field if section in ("memory", "timing") else f"{section}_{field}"
            raw_path = case_path.parent / mutated["artifacts"]["raw_stdout"]["path"]
            lines = raw_path.read_text(encoding="utf-8").splitlines()
            for i, line in enumerate(lines):
                fields = _tokens(line)
                if (fields.get("shape"), fields.get("scale"), fields.get("block")) != (
                        cell["shape"], str(cell["scale"]), str(cell["block"])) or raw_field not in fields:
                    continue
                token = f"{raw_field}={fields[raw_field]}"
                replacement = "" if value is None else f"{raw_field}=" + (
                    ",".join(map(str, value)) if isinstance(value, list) else str(value))
                lines[i] = line.replace(token, replacement)
            raw_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            mutated["artifacts"]["raw_stdout"]["sha256"] = sha256_file(raw_path)
            mutated["receipt_sha256"] = receipt_digest(mutated)
            case_path.write_text(json.dumps(mutated, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            try:
                validate_receipt(case_path)
            except ReceiptError:
                pass
            else:
                raise ReceiptError(f"planted negative passed: {name}")
        for name in ("omitted_cell", "duplicate_cell"):
            case_path = _copy_receipt_directory(receipt_path, root / name)
            mutated = json.loads(json.dumps(original))
            cell = mutated["matrix"]["cells"].pop(0)
            if name == "duplicate_cell":
                mutated["matrix"]["cells"].append(mutated["matrix"]["cells"][0])
            raw_path = case_path.parent / mutated["artifacts"]["raw_stdout"]["path"]
            lines = raw_path.read_text(encoding="utf-8").splitlines()
            prefix = f"shape={cell['shape']} scale={cell['scale']} block={cell['block']} "
            lines = [line for line in lines if not line.startswith(prefix)]
            raw_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            mutated["artifacts"]["raw_stdout"]["sha256"] = sha256_file(raw_path)
            mutated["receipt_sha256"] = receipt_digest(mutated)
            case_path.write_text(json.dumps(mutated, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            try:
                validate_receipt(case_path)
            except ReceiptError:
                pass
            else:
                raise ReceiptError(f"planted negative passed: {name}")
        raw_case_dir = root / "raw_output_binding"
        raw_case_path = _copy_receipt_directory(receipt_path, raw_case_dir)
        raw_path = raw_case_dir / "raw-output.txt"
        raw_path.write_text(raw_path.read_text(encoding="utf-8").replace(
            "output_bits=0xbf3ae148", "output_bits=0xbf3ae149", 1), encoding="utf-8")
        mutated = json.loads(json.dumps(original))
        mutated["artifacts"]["raw_stdout"]["sha256"] = sha256_file(raw_path)
        mutated["receipt_sha256"] = receipt_digest(mutated)
        raw_case_path.write_text(json.dumps(mutated, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        try:
            validate_receipt(raw_case_path)
        except ReceiptError:
            pass
        else:
            raise ReceiptError("planted negative passed: raw_output_binding")
        restored_dir = root / "restored"
        restored_path = _copy_receipt_directory(receipt_path, restored_dir)
        validate_receipt(restored_path)
    print("r2_controls=pass positive=pass planted_expected_output_cell=fail "
          "planted_allocation_count=fail planted_timed_allocation_count=fail "
          "planted_timing_order=fail planted_raw_output_binding=fail "
          "planted_memory_false_omitted=fail planted_reuse_false_omitted=fail "
          "planted_phase_timing_false_omitted=fail planted_batch_timing_false_omitted=fail "
          "planted_pool_bytes=fail planted_cell_omitted_duplicate=fail")


def record(args: argparse.Namespace) -> int:
    root = Path(args.source_root).resolve()
    executable = Path(args.executable).resolve()
    build_dir = Path(args.build_dir).resolve()
    output_dir = Path(args.output_dir).resolve()
    _require(executable.is_file(), f"benchmark executable does not exist: {executable}")
    output_dir.mkdir(parents=True, exist_ok=True)
    fixture_source = root / FIXTURE_SOURCE
    _require(fixture_source.is_file(), f"fixture source does not exist: {fixture_source}")
    _require(executable.is_relative_to(build_dir), "executable must be inside the selected build directory")
    values = _cache_values(build_dir)
    _require(values.get("CMAKE_BUILD_TYPE") == "Release", "benchmark build must be Release")
    _require(values.get("PULP_GRAPH_DIRECT_COPY_BENCHMARK") == "ON",
             "direct-copy benchmark must be enabled in the build profile")
    stdout_path = output_dir / "raw-output.txt"
    stderr_path = output_dir / "raw-stderr.txt"
    fixture_artifact = output_dir / "source-fixture.cpp"
    source_sha = _source_sha(root)
    _verify_committed_fixture(root, source_sha, sha256_file(fixture_source))
    _require(values.get("CMAKE_HOME_DIRECTORY") == str(root), "build directory belongs to another source tree")
    shutil.copy2(fixture_source, fixture_artifact)
    executable_artifact = output_dir / "benchmark-executable"
    shutil.copy2(executable, executable_artifact)
    try:
        completed = subprocess.run(
            [str(executable_artifact)], capture_output=True, text=True, encoding="utf-8", check=False,
            timeout=args.timeout_seconds,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", errors="replace")
        stderr += f"\nbenchmark timed out after {args.timeout_seconds:.1f}s\n"
        stdout_path.write_text(stdout, encoding="utf-8")
        stderr_path.write_text(stderr, encoding="utf-8")
        returncode = 124
        receipt = {
            "schema": SCHEMA,
            "fixture": FIXTURE_VERSION,
            "status": "PARTIAL",
            "qualification_scope": "R2 bounded graph runtime measurement",
            "measurement_contract": MEASUREMENT_CONTRACT,
            "qualification_boundary": "benchmark timed out before a complete receipt could be produced",
            "source_sha": _source_sha(root),
            "source_fixture": {"path": FIXTURE_SOURCE.as_posix(), "sha256": sha256_file(fixture_source)},
            "executable": _relative_path(executable, root),
            "executable_sha256": sha256_file(executable),
            "build_profile": values,
            "run": {"returncode": returncode, "timeout_seconds": args.timeout_seconds},
            "artifacts": {
                "raw_stdout": {"path": stdout_path.name, "sha256": sha256_file(stdout_path)},
                "raw_stderr": {"path": stderr_path.name, "sha256": sha256_file(stderr_path)},
                "source_fixture": {"path": fixture_artifact.name, "sha256": sha256_file(fixture_artifact)},
            },
        }
        receipt["receipt_sha256"] = receipt_digest(receipt)
        receipt_path = output_dir / "receipt.json"
        receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"r2_receipt_timeout={receipt_path}", file=sys.stderr)
        return returncode
    stdout_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")
    _require(_source_sha(root) == source_sha, "source HEAD changed during measurement")
    _require(sha256_file(fixture_source) == sha256_file(fixture_artifact), "fixture changed during measurement")
    _require(sha256_file(executable) == sha256_file(executable_artifact), "executable changed during measurement")
    receipt = make_receipt(
        root=root, build_dir=build_dir, executable=executable,
        stdout_path=stdout_path, stderr_path=stderr_path,
        returncode=completed.returncode, stdout=completed.stdout,
    )
    receipt_path = output_dir / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    validate_receipt(receipt_path, root)
    print(f"receipt={receipt_path}")
    print(f"receipt_sha256={receipt['receipt_sha256']}")
    return completed.returncode



# The paced publication lane has its own type and dimensions. The offline v2
# receipt above continues to require its original 40 cells and batch semantics.
ACTIVATION_SCHEMA = "pulp.graph_paced_activation_receipt.v1"
ACTIVATION_FIXTURE = "g12_paced_activation_v1"
ACTIVATION_SOURCE = Path("test/fixtures/g5_graph_event_rt_cases.inc")
ACTIVATION_RATES = (44100, 48000, 96000)
ACTIVATION_BLOCKS = (32, 64, 128, 256)
ACTIVATION_BACKENDS = ("direct", "routed")
ACTIVATION_KEYS = frozenset((rate, frames, backend) for rate in ACTIVATION_RATES
                            for frames in ACTIVATION_BLOCKS for backend in ACTIVATION_BACKENDS)
ACTIVATION_CONTRACT = {
    "publication": "public prepared gain edit; prepare precedes callbacks; commit on separate thread",
    "activation": "request to first entirely changed output; output may change before commit returns",
    "audio": "mono float32 input 0.5; gain 0.5 then 1; every sample retained as lossless hex RLE",
    "schedule": "fixed original origin; late callback never resets the schedule",
    "timing": "nearest-rank p99 callback duration <70% block; p99 absolute wake <0.1 sample; zero deadline misses",
    "resources": "prepared buffer components and retained harness payload; excludes stack/allocator/RSS",
    "safety": "timing runs use no scoped probes; RTSan is separate source/binary-bound evidence",
    "scope": "three nodes/two edges, zero voices/grains/PDC; not WCET or full G12 qualification",
}
ACTIVATION_META = frozenset("rate frames backend callbacks prepare_start_ns prepare_end_ns request_ns return_ns "
    "committed first_new first_new_sample latency_samples snapshot_latency snapshot_nodes snapshot_edges "
    "node_latencies edge_delays nodes edges voices grains probes node_audio_bytes automation_bytes delay_bytes "
    "spectral_bytes prepared_bytes row_stride row_bytes audio_bytes io_bytes harness_payload_bytes misses "
    "executor_blocks parallel_levels parallel_enabled walk_fallbacks execution_failures "
    "compiled_source build_type build_dirty".split())


def activation_percentile(values: list[int]) -> int:
    _require(bool(values), "empty callback sample set")
    return sorted(values)[math.ceil(len(values) * .99) - 1]


def activation_thresholds(metrics: dict[str, Any], rate: int, frames: int) -> bool:
    # Cross multiplication preserves strict bounds without float rounding at a gate.
    return (metrics["p99_duration_ns"] * rate * 10 < 7 * frames * 1_000_000_000 and
            metrics["p99_absolute_wake_ns"] * rate * 10 < 1_000_000_000 and
            metrics["deadline_misses"] == 0)


def parse_activation_output(stdout: str) -> dict[str, Any]:
    header = False
    meta: dict[str, Any] | None = None
    rows: list[dict[str, Any]] = []
    for line in stdout.splitlines():
        if line == ACTIVATION_FIXTURE:
            _require(not header, "duplicate activation header")
            header = True
        elif line.startswith("G12_CELL "):
            _require(meta is None, "duplicate activation cell")
            fields = _tokens(line)
            _require(set(fields) == ACTIVATION_META, "activation metadata fields mismatch")
            try:
                meta = {key: value if key in ("backend", "compiled_source", "build_type") else [int(v) for v in value.split(",")]
                        if key in ("node_latencies", "edge_delays") else int(value)
                        for key, value in fields.items()}
            except ValueError as exc:
                raise ReceiptError("malformed activation metadata") from exc
        elif line.startswith("G12_ROW "):
            fields = _tokens(line)
            _require(set(fields) == {"index", "sample", "target_ns", "start_ns", "end_ns", "audio_class", "bits"},
                     "activation callback fields mismatch")
            try:
                row = {key: int(value) for key, value in fields.items() if key != "bits"}
                row["bits"] = [[int(bits, 16), int(count)] for bits, count in
                               (item.split(":") for item in fields["bits"].split(","))]
            except ValueError as exc:
                raise ReceiptError("malformed callback audio") from exc
            rows.append(row)
    _require(header and meta is not None, "activation header or metadata missing")
    cell = {"metadata": meta, "callbacks": rows}
    cell["metrics"] = validate_activation_cell(cell)
    return cell


def validate_activation_cell(cell: dict[str, Any]) -> dict[str, Any]:
    meta, rows = cell["metadata"], cell["callbacks"]
    _require(set(meta) == ACTIVATION_META, "activation metadata fields mismatch")
    for key in ACTIVATION_META - {"backend", "compiled_source", "build_type", "node_latencies", "edge_delays"}:
        _require(type(meta[key]) is int and meta[key] >= 0, f"invalid activation integer {key}")
    _require(isinstance(meta["compiled_source"], str) and re.fullmatch(r"[0-9a-f]{40}", meta["compiled_source"]) and
             meta["build_type"] == "Release" and meta["build_dirty"] == 0, "compiled source/build stamp mismatch")
    rate, frames, backend = meta["rate"], meta["frames"], meta["backend"]
    _require((rate, frames, backend) in ACTIVATION_KEYS, "unexpected activation matrix cell")
    count = meta["callbacks"]
    _require(meta["executor_blocks"] == (count if backend == "routed" else 0) and
             meta["parallel_levels"] == meta["parallel_enabled"] == meta["walk_fallbacks"] == meta["execution_failures"] == 0,
             "actual serial backend dispatch mismatch")
    _require(16 <= count <= 100000 and len(rows) == count, "callback count mismatch")
    first = meta["first_new"]
    _require(meta["committed"] == 1 and 8 <= first < count, "missing commit or audio activation")
    _require(meta["first_new_sample"] == first * frames, "activation absolute sample mismatch")
    _require(meta["prepare_start_ns"] <= meta["prepare_end_ns"] < meta["request_ns"] <= meta["return_ns"],
             "prepare/commit timestamps inconsistent")
    _require(meta["prepare_end_ns"] < rows[0]["target_ns"], "prepare overlaps callback interval")
    _require(rows[7]["end_ns"] <= meta["request_ns"] <= rows[first]["end_ns"],
             "request/activation timing inconsistent")
    _require(meta["latency_samples"] == meta["snapshot_latency"] == 0 and
             meta["snapshot_nodes"] == meta["nodes"] == 3 and
             meta["snapshot_edges"] == meta["edges"] == 2 and
             meta["node_latencies"] == [0, 0, 0] and meta["edge_delays"] == [0, 0] and
             all(type(v) is int for v in meta["node_latencies"] + meta["edge_delays"]),
             "zero-PDC snapshot contract mismatch")
    _require(meta["voices"] == meta["grains"] == meta["probes"] == 0,
             "activation fixture resource scope mismatch")
    # This scalar graph retains input/output audio per node plus no automation,
    # delay or spectral payload. This arithmetic is independent of the C++ stats.
    _require(meta["node_audio_bytes"] == frames * 6 * 4 and
             meta["automation_bytes"] == meta["delay_bytes"] == meta["spectral_bytes"] == 0 and
             meta["prepared_bytes"] == frames * 6 * 4, "prepared buffer components mismatch")
    _require(meta["row_stride"] == 72 and meta["row_bytes"] == count * meta["row_stride"] and
             meta["audio_bytes"] == count * frames * 4 and meta["io_bytes"] == 2 * frames * 4 and
             meta["harness_payload_bytes"] == meta["row_bytes"] + meta["audio_bytes"] + meta["io_bytes"],
             "harness retained payload mismatch")
    durations, wake = [], []
    misses = 0
    origin = rows[0]["target_ns"]
    for i, row in enumerate(rows):
        _require(set(row) == {"index", "sample", "target_ns", "start_ns", "end_ns", "audio_class", "bits"},
                 "callback fields mismatch")
        _require(all(type(row[key]) is int and row[key] >= 0 for key in row if key != "bits"),
                 "invalid callback integer")
        _require(row["index"] == i and row["sample"] == i * frames, "callback index/sample mismatch")
        # chrono duration_cast truncates floating seconds to nanoseconds. Allow
        # only its one-nanosecond representation difference from exact arithmetic.
        _require(abs(row["target_ns"] - origin - frames * i * 1_000_000_000 // rate) <= 1,
                 "callback schedule drifted from fixed origin")
        _require(row["target_ns"] <= row["start_ns"] <= row["end_ns"], "callback timestamps inconsistent")
        if i:
            _require(rows[i - 1]["end_ns"] <= row["start_ns"], "callbacks overlap")
        expected_bits = 0x3e800000 if i < first else 0x3f000000
        _require(row["audio_class"] == (0 if i < first else 1), "stale/mixed/silent audio class")
        _require(isinstance(row["bits"], list) and bool(row["bits"]), "callback audio missing")
        samples = 0
        for run in row["bits"]:
            _require(isinstance(run, list) and len(run) == 2 and
                     all(type(v) is int for v in run) and run[1] > 0 and run[0] == expected_bits,
                     "independent every-sample gain oracle failed")
            samples += run[1]
        _require(samples == frames, "callback audio sample count mismatch")
        durations.append(row["end_ns"] - row["start_ns"])
        wake.append(abs(row["start_ns"] - row["target_ns"]))
        misses += (row["end_ns"] - row["target_ns"]) * rate > frames * 1_000_000_000
    _require(meta["misses"] == misses, "raw deadline miss count mismatch")
    metrics = {"p99_duration_ns": activation_percentile(durations),
               "max_duration_ns": max(durations), "p99_absolute_wake_ns": activation_percentile(wake),
               "max_absolute_wake_ns": max(wake), "deadline_misses": misses,
               "prepare_duration_ns": meta["prepare_end_ns"] - meta["prepare_start_ns"],
               "commit_duration_ns": meta["return_ns"] - meta["request_ns"],
               "request_to_changed_output_ns": rows[first]["end_ns"] - meta["request_ns"]}
    metrics["thresholds_met"] = activation_thresholds(metrics, rate, frames)
    return metrics


def _activation_artifact(receipt_path: Path, artifact: dict[str, str]) -> Path:
    _require(isinstance(artifact, dict) and set(artifact) == {"path", "sha256"}, "malformed activation artifact")
    name = artifact["path"]
    _require(isinstance(name, str) and Path(name).name == name and name not in ("", ".", ".."),
             "activation artifact escapes receipt directory")
    path = receipt_path.parent / name
    _require(path.resolve().parent == receipt_path.parent.resolve() and path.is_file(),
             "activation artifact missing or outside directory")
    _require(artifact["sha256"] == sha256_file(path), "activation artifact digest mismatch")
    return path


def validate_activation_receipt(path: Path, receipt: dict[str, Any], source_root: Path | None) -> None:
    _require(receipt.get("receipt_sha256") == receipt_digest(receipt), "receipt digest mismatch")
    _require(receipt.get("fixture") == ACTIVATION_FIXTURE and receipt.get("status") == "PARTIAL" and
             receipt.get("measurement_contract") == ACTIVATION_CONTRACT, "activation type/contract mismatch")
    _require(receipt.get("mode") in ("pilot", "qualification"), "unknown activation mode")
    qualifying = receipt["mode"] == "qualification"
    _require(receipt.get("timing_qualified") is qualifying, "pilot cannot claim timing qualification")
    count = receipt.get("callback_count")
    _require(type(count) is int and (count == 100000 if qualifying else 16 <= count <= 128),
             "activation callback count does not match lane")
    _require(isinstance(receipt.get("source_sha"), str) and re.fullmatch(r"[0-9a-f]{40}", receipt["source_sha"]),
             "activation source commit invalid")
    _require(receipt.get("build_profile", {}).get("CMAKE_BUILD_TYPE") == "Release" and
             not receipt["build_profile"].get("PULP_SANITIZER"), "activation timing requires ordinary Release")
    _require(receipt.get("matrix") == {"rates": list(ACTIVATION_RATES), "frames": list(ACTIVATION_BLOCKS),
             "backends": list(ACTIVATION_BACKENDS), "expected_cell_count": 24}, "activation matrix dimensions mismatch")
    artifacts = receipt["artifacts"]
    source = _activation_artifact(path, artifacts["source_fixture"])
    executable = _activation_artifact(path, artifacts["executable"])
    commands = _activation_artifact(path, artifacts["compile_commands"])
    fixture_commands = [entry["command"] for entry in json.loads(commands.read_text())
                        if entry["file"].endswith("/test/test_trigger_graph.cpp")]
    _require(len(fixture_commands) == 1 and "-O3" in fixture_commands[0].split() and
             "-DNDEBUG" in fixture_commands[0].split() and "-fsanitize=" not in fixture_commands[0],
             "activation fixture compile flags are not ordinary Release")
    _require(receipt.get("source_fixture") == {"path": ACTIVATION_SOURCE.as_posix(), "sha256": sha256_file(source)},
             "activation source fixture binding mismatch")
    _require(receipt.get("executable_sha256") == sha256_file(executable), "activation executable binding mismatch")
    source_root = source_root or Path(__file__).resolve().parents[1]
    if source_root is not None:
        committed = subprocess.run(["git", "-C", str(source_root), "show",
                                   f"{receipt['source_sha']}:{ACTIVATION_SOURCE.as_posix()}"],
                                  check=True, capture_output=True).stdout
        _require(hashlib.sha256(committed).hexdigest() == sha256_file(source), "activation source commit mismatch")
    cells = receipt.get("cells")
    _require(isinstance(cells, list) and len(cells) == 24, "activation matrix cell count mismatch")
    seen = set()
    for cell in cells:
        _require(isinstance(cell, dict) and set(cell) == {"metadata", "metrics", "returncode", "raw_stdout", "raw_stderr"},
                 "activation cell fields mismatch")
        meta = cell["metadata"]
        key = (meta["rate"], meta["frames"], meta["backend"])
        _require(key in ACTIVATION_KEYS and key not in seen, "duplicate/unexpected activation cell")
        seen.add(key)
        _require(meta["compiled_source"] == receipt["source_sha"], "stale executable source commit")
        _require(meta["callbacks"] == count and type(cell.get("returncode")) is int and cell["returncode"] == 0, "shortened or failed activation run")
        raw = _activation_artifact(path, cell["raw_stdout"])
        stderr = _activation_artifact(path, cell["raw_stderr"])
        _require(not stderr.read_bytes(), "activation run wrote stderr")
        parsed = parse_activation_output(raw.read_text(encoding="utf-8"))
        metrics = validate_activation_cell({"metadata": meta, "callbacks": parsed["callbacks"]})
        _require(all(type(cell["metrics"].get(key)) is type(value) for key, value in metrics.items()),
                 "activation summary field types mismatch")
        _require(cell["metrics"] == metrics, "activation raw percentile/miss/transition summary mismatch")
        _require(all(cell[field] == parsed[field] for field in ("metadata", "metrics")),
                 "activation receipt differs from raw callbacks/audio")
        _require(not qualifying or metrics["thresholds_met"], "paced activation timing thresholds failed")
    _require(seen == ACTIVATION_KEYS, "activation matrix incomplete")



def run_activation_controls(path: Path, source_root: Path | None = None) -> None:
    validate_receipt(path, source_root)
    original_text = path.read_text(encoding="utf-8")
    original = json.loads(original_text)
    # Each mutation is re-signed; restoration is checked after each rejection.
    mutations = (
        ("missing_cell", lambda r: r["cells"].pop()),
        ("duplicate_cell", lambda r: r["cells"].__setitem__(1, r["cells"][0])),
        ("short_count", lambda r: r.__setitem__("callback_count", r["callback_count"] - 1)),
        ("false_percentile", lambda r: r["cells"][0]["metrics"].__setitem__("p99_duration_ns", 0)),
        ("false_misses", lambda r: r["cells"][0]["metrics"].__setitem__("deadline_misses", -1)),
        ("false_pdc", lambda r: r["cells"][0]["metadata"].__setitem__("latency_samples", 1)),
        ("false_memory", lambda r: r["cells"][0]["metadata"].__setitem__("prepared_bytes", 0)),
        ("false_source", lambda r: r["source_fixture"].__setitem__("sha256", "0" * 64)),
        ("false_binary", lambda r: r.__setitem__("executable_sha256", "0" * 64)),
    )
    for name, mutate in mutations:
        changed = json.loads(original_text)
        mutate(changed)
        changed["receipt_sha256"] = receipt_digest(changed)
        try:
            path.write_text(json.dumps(changed, indent=2, sort_keys=True) + "\n")
            try:
                validate_receipt(path, source_root)
            except ReceiptError:
                print(f"activation_control={name} rejected=semantic restored=pending")
            else:
                raise ReceiptError(f"planted activation negative passed: {name}")
        finally:
            path.write_text(original_text)
        validate_receipt(path, source_root)
    # Also re-sign a raw every-sample audio lie. This is distinct from an
    # unbound receipt summary and does not rely on stale file hashes.
    raw = path.parent / original["cells"][0]["raw_stdout"]["path"]
    saved = raw.read_bytes()
    try:
        _require(b"bits=3f000000:" in saved, "raw positive has no new audio")
        raw.write_bytes(saved.replace(b"bits=3f000000:", b"bits=3ec00000:", 1))
        changed = json.loads(original_text)
        changed["cells"][0]["raw_stdout"]["sha256"] = sha256_file(raw)
        changed["receipt_sha256"] = receipt_digest(changed)
        path.write_text(json.dumps(changed, indent=2, sort_keys=True) + "\n")
        try:
            validate_receipt(path, source_root)
        except ReceiptError:
            print("activation_control=resigned_raw_audio rejected=semantic restored=pending")
        else:
            raise ReceiptError("planted raw audio negative passed")
    finally:
        raw.write_bytes(saved)
        path.write_text(original_text)
    validate_receipt(path, source_root)
    print("activation_controls=pass positive=pass broken=10_rejected restored=pass")


def record_activation(args: argparse.Namespace) -> int:
    root, build = Path(args.source_root).resolve(), Path(args.build_dir).resolve()
    executable, output = Path(args.executable).resolve(), Path(args.output_dir).resolve()
    values = _cache_values(build)
    _require(values.get("CMAKE_HOME_DIRECTORY") == str(root) and executable.is_relative_to(build),
             "activation build/source provenance mismatch")
    _require(values.get("CMAKE_BUILD_TYPE") == "Release" and not values.get("PULP_SANITIZER"),
             "activation timing requires ordinary Release")
    _require(args.mode != "qualification" or args.callbacks == 100000, "qualification requires 100000 callbacks")
    _require(args.mode != "pilot" or 16 <= args.callbacks <= 128, "pilot requires 16..128 callbacks")
    _require(subprocess.run(["git", "-C", str(root), "diff", "--quiet", "HEAD", "--"]).returncode == 0,
             "commit tracked source changes before recording")
    sha = _source_sha(root)
    source = root / ACTIVATION_SOURCE
    committed = subprocess.run(["git", "-C", str(root), "show", f"{sha}:{ACTIVATION_SOURCE.as_posix()}"],
                               check=True, capture_output=True).stdout
    _require(hashlib.sha256(committed).hexdigest() == sha256_file(source), "commit activation fixture before recording")
    output.mkdir(parents=True, exist_ok=True)
    binary_copy, source_copy = output / "activation-executable", output / "source-fixture.inc"
    shutil.copy2(executable, binary_copy)
    shutil.copy2(source, source_copy)
    commands_copy = output / "compile_commands.json"
    shutil.copy2(build / "compile_commands.json", commands_copy)
    def artifact(p: Path) -> dict[str, str]:
        return {"path": p.name, "sha256": sha256_file(p)}
    receipt = {"schema": ACTIVATION_SCHEMA, "fixture": ACTIVATION_FIXTURE, "status": "PARTIAL",
               "mode": args.mode, "timing_qualified": args.mode == "qualification",
               "measurement_contract": ACTIVATION_CONTRACT, "callback_count": args.callbacks,
               "source_sha": sha, "source_fixture": {"path": ACTIVATION_SOURCE.as_posix(), "sha256": sha256_file(source)},
               "executable_sha256": sha256_file(binary_copy),
               "build_profile": {key: values.get(key, "") for key in ("CMAKE_BUILD_TYPE", "PULP_SANITIZER")},
               "matrix": {"rates": list(ACTIVATION_RATES), "frames": list(ACTIVATION_BLOCKS),
                          "backends": list(ACTIVATION_BACKENDS), "expected_cell_count": 24},
               "artifacts": {"source_fixture": artifact(source_copy), "executable": artifact(binary_copy),
                             "compile_commands": artifact(commands_copy)}, "cells": []}
    for rate, frames, backend in sorted(ACTIVATION_KEYS):
        env = dict(os.environ)
        for key in tuple(env):
            if key.startswith(("G12_ACTIVATION_", "G12_RTSAN_")):
                env.pop(key)
        env.update(G12_ACTIVATION_RATE=str(rate), G12_ACTIVATION_FRAMES=str(frames),
                   G12_ACTIVATION_CALLBACKS=str(args.callbacks), G12_ACTIVATION_BACKEND=backend)
        stem = f"{rate}-{frames}-{backend}"
        stdout_path, stderr_path = output / (stem + ".txt"), output / (stem + ".stderr")
        with stdout_path.open("w") as out, stderr_path.open("w") as err:
            result = subprocess.run([str(binary_copy), "G12 paced activation receipt cell"], env=env,
                                    stdout=out, stderr=err, timeout=args.timeout_seconds)
        cell = parse_activation_output(stdout_path.read_text(encoding="utf-8"))
        _require(cell["metadata"]["compiled_source"] == sha, "stale executable source commit")
        cell.pop("callbacks")
        cell.update(returncode=result.returncode, raw_stdout=artifact(stdout_path), raw_stderr=artifact(stderr_path))
        receipt["cells"].append(cell)
    receipt["receipt_sha256"] = receipt_digest(receipt)
    path = output / "receipt.json"
    path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    validate_receipt(path, root)
    print(f"activation_receipt={path} mode={args.mode} timing_qualified={receipt['timing_qualified']}")
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    sub = root.add_subparsers(dest="command", required=True)
    record_parser = sub.add_parser("record", help="run benchmark and write a receipt")
    record_parser.add_argument("--source-root", default=".")
    record_parser.add_argument("--build-dir", default="build")
    record_parser.add_argument("--executable", required=True)
    record_parser.add_argument("--output-dir", required=True)
    record_parser.add_argument("--timeout-seconds", type=float, default=300.0)
    activation_parser = sub.add_parser("record-activation", help="record the typed paced activation lane")
    activation_parser.add_argument("--source-root", default=".")
    activation_parser.add_argument("--build-dir", default="build")
    activation_parser.add_argument("--executable", required=True)
    activation_parser.add_argument("--output-dir", required=True)
    activation_parser.add_argument("--mode", choices=("pilot", "qualification"), default="pilot")
    activation_parser.add_argument("--callbacks", type=int, default=64)
    activation_parser.add_argument("--timeout-seconds", type=float, default=660.0)
    validate_parser = sub.add_parser("validate", help="validate a receipt and its artifacts")
    validate_parser.add_argument("receipt", type=Path)
    validate_parser.add_argument("--source-root", type=Path, help="also verify fixture bytes in the recorded Git commit")
    controls_parser = sub.add_parser("controls", help="run positive and planted-negative controls")
    controls_parser.add_argument("receipt", type=Path)
    controls_parser.add_argument("--source-root", type=Path)
    return root


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(list(argv) if argv is not None else None)
    try:
        if args.command == "record-activation":
            return record_activation(args)
        if args.command == "record":
            return record(args)
        if args.command == "validate":
            validate_receipt(args.receipt, args.source_root)
            label = "activation_receipt" if json.loads(args.receipt.read_text()).get("schema") == ACTIVATION_SCHEMA else "r2_receipt"
            print(f"{label}=pass")
            return 0
        run_controls(args.receipt, args.source_root)
        return 0
    except (ReceiptError, OSError, subprocess.SubprocessError, KeyError, TypeError, ValueError) as exc:
        print(f"r2_receipt_error={exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
