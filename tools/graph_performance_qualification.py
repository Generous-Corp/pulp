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


SCHEMA = "pulp.graph_r2_qualification_receipt.v1"
FIXTURE_VERSION = "graph_r2_measurement_v1"
SHAPES = ("passthrough", "chain", "fan-out")
SCALES = (1, 4, 16, 64)
BLOCKS = (16, 32, 64, 128, 512)


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


def expected_output_bits(shape: str, scale: int) -> int:
    # This mirrors fixed_input(frame=0) and the fixture's fan-out reduction.
    value = _float32(-0.73)
    if shape == "fan-out":
        value = _float32(value * _float32(float(scale)))
    return struct.unpack("<I", struct.pack("<f", value))[0]


_TOKEN = re.compile(r"(?P<key>[A-Za-z_][A-Za-z0-9_-]*)=(?P<value>[^\s]+)")


def _tokens(line: str) -> dict[str, str]:
    return {match.group("key"): match.group("value") for match in _TOKEN.finditer(line)}


def parse_output(stdout: str) -> list[dict[str, Any]]:
    """Parse the benchmark's activation/timing rows and require 40 cells."""
    cells: dict[tuple[str, int, int], dict[str, Any]] = {}
    for line_number, line in enumerate(stdout.splitlines(), 1):
        fields = _tokens(line)
        if "shape" not in fields or "scale" not in fields or "block" not in fields:
            continue
        try:
            key = (fields["shape"], int(fields["scale"]), int(fields["block"]))
        except ValueError as exc:
            raise ReceiptError(f"line {line_number}: invalid matrix key") from exc
        if key not in EXPECTED_KEYS:
            raise ReceiptError(f"line {line_number}: unexpected matrix cell {key!r}")
        cell = cells.setdefault(key, {"shape": key[0], "scale": key[1], "block": key[2]})
        if "oracle" in fields:
            if "timing" in cell:
                raise ReceiptError(f"line {line_number}: duplicate timing row for {key!r}")
            try:
                cell.update(
                    timing={
                        name: float(fields[name])
                        for name in ("p50_ns", "p99_sample_ns", "max_ns", "jitter_ns",
                                     "observed_wcet_proxy_ns")
                        if name in fields
                    },
                    warmup=int(fields["warmup"]),
                    iterations=int(fields["iterations"]),
                    samples=int(fields["samples"]),
                    oracle=fields["oracle"],
                    process_allocations=int(fields["process_allocations"]),
                    output_bits=int(fields["output_bits"].removeprefix("0x"), 16),
                )
            except (KeyError, ValueError) as exc:
                raise ReceiptError(f"line {line_number}: malformed timing row") from exc
        else:
            if "activation" in cell:
                raise ReceiptError(f"line {line_number}: duplicate activation row for {key!r}")
            try:
                cell.update(
                    activation={
                        name: float(fields[name])
                        for name in ("activation_p50_ns", "activation_p99_sample_ns",
                                     "activation_max_ns")
                        if name in fields
                    },
                    scratch_slots=int(fields["scratch_slots"]),
                )
            except (KeyError, ValueError) as exc:
                raise ReceiptError(f"line {line_number}: malformed activation row") from exc
    if frozenset(cells) != EXPECTED_KEYS:
        missing = sorted(EXPECTED_KEYS - cells.keys())
        extra = sorted(cells.keys() - EXPECTED_KEYS)
        raise ReceiptError(f"matrix is incomplete: missing={missing!r} extra={extra!r}")
    for key, cell in cells.items():
        if "activation" not in cell or "timing" not in cell:
            raise ReceiptError(f"cell {key!r} lacks activation or timing row")
    return [cells[key] for key in sorted(cells)]


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
    cells = parse_output(stdout)
    for cell in cells:
        cell["expected_output_bits"] = expected_output_bits(cell["shape"], cell["scale"])
    receipt: dict[str, Any] = {
        "schema": SCHEMA,
        "fixture": FIXTURE_VERSION,
        "status": "PARTIAL",
        "qualification_scope": "R2 bounded graph runtime measurement",
        "qualification_boundary": (
            "This receipt does not qualify callback deadlines, WCET, cross-platform RT safety, "
            "hosted plugins, fleet execution, or final G-12 flagship coverage."
        ),
        "source_sha": _source_sha(root),
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
            "warmup_blocks": 500,
            "timed_blocks": 100000,
            "timing_samples": 31,
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
        },
    }
    receipt["receipt_sha256"] = receipt_digest(receipt)
    return receipt


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ReceiptError(message)


def validate_receipt(receipt_path: Path) -> None:
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ReceiptError(f"cannot read receipt: {receipt_path}") from exc
    _require(receipt.get("schema") == SCHEMA, "unexpected receipt schema")
    _require(receipt.get("fixture") == FIXTURE_VERSION, "unexpected fixture version")
    _require(receipt.get("status") == "PARTIAL", "R2 receipt must remain PARTIAL")
    _require(receipt.get("receipt_sha256") == receipt_digest(receipt), "receipt digest mismatch")
    source_sha = receipt.get("source_sha")
    _require(isinstance(source_sha, str) and re.fullmatch(r"[0-9a-f]{40,64}", source_sha) is not None,
             "source_sha must be a full git object id")
    executable_sha = receipt.get("executable_sha256")
    _require(isinstance(executable_sha, str) and re.fullmatch(r"[0-9a-f]{64}", executable_sha) is not None,
             "executable_sha256 must be a SHA-256")
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
        _require(isinstance(actual_bits, int) and isinstance(expected_bits, int),
                 f"output oracle fields are malformed for {key!r}")
        _require(abs(_bits_float(actual_bits) - _bits_float(expected_bits)) <= 1.0e-4,
                 f"output oracle mismatch for {key!r}")
        _require(cell.get("expected_output_bits") == expected_output_bits(key[0], key[1]),
                 f"fixture expected output mismatch for {key!r}")
        _require(isinstance(cell.get("activation"), dict), f"activation missing for {key!r}")
        _require(isinstance(cell.get("timing"), dict), f"timing missing for {key!r}")
        for lane in (cell["activation"], cell["timing"]):
            _require(all(isinstance(number, (int, float)) and math.isfinite(number)
                         for number in lane.values()), f"non-finite timing field for {key!r}")
    _require(frozenset(seen) == EXPECTED_KEYS, "matrix does not cover the expected cells")
    artifacts = receipt.get("artifacts")
    _require(isinstance(artifacts, dict), "artifacts are missing")
    for name in ("raw_stdout", "raw_stderr"):
        artifact = artifacts.get(name)
        _require(isinstance(artifact, dict), f"{name} artifact is missing")
        relative = artifact.get("path")
        _require(isinstance(relative, str) and not os.path.isabs(relative),
                 f"{name} path must be relative")
        path = (receipt_path.parent / relative).resolve()
        _require(path.parent == receipt_path.parent.resolve(), f"{name} escapes receipt directory")
        _require(path.is_file(), f"{name} artifact is missing on disk")
        _require(artifact.get("sha256") == sha256_file(path), f"{name} digest mismatch")
    _require(receipt.get("run", {}).get("returncode") == 0, "benchmark did not exit successfully")


def _copy_receipt_directory(receipt_path: Path, destination: Path) -> Path:
    destination.mkdir(parents=True)
    for source in receipt_path.parent.iterdir():
        if source.is_file():
            shutil.copy2(source, destination / source.name)
    return destination / receipt_path.name


def run_controls(receipt_path: Path) -> None:
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
        restored_dir = root / "restored"
        restored_path = _copy_receipt_directory(receipt_path, restored_dir)
        validate_receipt(restored_path)
    print("r2_controls=pass positive=pass planted_expected_output_cell=fail planted_allocation_count=fail")


def record(args: argparse.Namespace) -> int:
    root = Path(args.source_root).resolve()
    executable = Path(args.executable).resolve()
    build_dir = Path(args.build_dir).resolve()
    output_dir = Path(args.output_dir).resolve()
    _require(executable.is_file(), f"benchmark executable does not exist: {executable}")
    output_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = output_dir / "raw-output.txt"
    stderr_path = output_dir / "raw-stderr.txt"
    completed = subprocess.run([str(executable)], capture_output=True, text=True,
                               encoding="utf-8", check=False)
    stdout_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")
    receipt = make_receipt(
        root=root, build_dir=build_dir, executable=executable,
        stdout_path=stdout_path, stderr_path=stderr_path,
        returncode=completed.returncode, stdout=completed.stdout,
    )
    receipt_path = output_dir / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    validate_receipt(receipt_path)
    print(f"receipt={receipt_path}")
    print(f"receipt_sha256={receipt['receipt_sha256']}")
    return completed.returncode


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    sub = root.add_subparsers(dest="command", required=True)
    record_parser = sub.add_parser("record", help="run benchmark and write a receipt")
    record_parser.add_argument("--source-root", default=".")
    record_parser.add_argument("--build-dir", default="build")
    record_parser.add_argument("--executable", required=True)
    record_parser.add_argument("--output-dir", required=True)
    validate_parser = sub.add_parser("validate", help="validate a receipt and its artifacts")
    validate_parser.add_argument("receipt", type=Path)
    controls_parser = sub.add_parser("controls", help="run positive and planted-negative controls")
    controls_parser.add_argument("receipt", type=Path)
    return root


def main(argv: Iterable[str] | None = None) -> int:
    args = parser().parse_args(list(argv) if argv is not None else None)
    try:
        if args.command == "record":
            return record(args)
        if args.command == "validate":
            validate_receipt(args.receipt)
            print("r2_receipt=pass")
            return 0
        run_controls(args.receipt)
        return 0
    except (ReceiptError, OSError, subprocess.SubprocessError) as exc:
        print(f"r2_receipt_error={exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
