#!/usr/bin/env python3
"""Validate durable sampler interpolation evaluator evidence."""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import tempfile
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EVIDENCE = (
    REPO_ROOT
    / "docs/validation/sampler-interpolation"
    / "apple-m5-max-mac17-7.release.json"
)
SCHEMA = "pulp.sampler-interpolation-benchmark.v2"
TIERS = (
    "hold",
    "nearest",
    "linear",
    "cubic-hermite",
    "cubic-lagrange",
    "ratio-sinc",
)
RATIOS = (0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0)
VOICES = (1, 8)
EXPECTED_BUDGETS = {
    "hold": 40.0,
    "nearest": 30.0,
    "linear": 30.0,
    "cubic-hermite": 45.0,
    "cubic-lagrange": 35.0,
    "ratio-sinc": 480.0,
}
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SOURCE_BUNDLE_PATHS = (
    "core/audio/include/pulp/audio/loop_types.hpp",
    "core/audio/include/pulp/audio/sample_interpolation.hpp",
    "core/audio/include/pulp/audio/sample_sinc_kernel.hpp",
    "core/signal/include/pulp/signal/interpolator.hpp",
    "test/support/sample_interpolation_render.hpp",
    "test/sample_interpolation_benchmark.cpp",
    "test/cmake/app_audio_host_tests.cmake",
)


def source_bundle_sha256() -> str:
    """Hash every compilation input owned by this focused benchmark.

    Paths and byte lengths are framed so concatenation ambiguity cannot make a
    different source bundle produce the same hashed byte stream.
    """
    digest = hashlib.sha256()
    for relative in SOURCE_BUNDLE_PATHS:
        payload = (REPO_ROOT / relative).read_bytes()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(len(payload)).encode("ascii"))
        digest.update(b"\0")
        digest.update(payload)
    return digest.hexdigest()


def _non_placeholder(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip()) and "unspecified" not in value.lower()


def validate(
    data: Any,
    benchmark_binary: Path | None = None,
    source_only: bool = False,
) -> list[str]:
    errors: list[str] = []
    if not isinstance(data, dict):
        return ["root must be an object"]
    if data.get("schema") != SCHEMA:
        errors.append(f"schema must be {SCHEMA}")
    if data.get("scope") != "interpolation-evaluator":
        errors.append("scope must remain interpolation-evaluator")

    machine = data.get("machine")
    if not isinstance(machine, dict):
        errors.append("machine must be an object")
    else:
        for field in ("label", "model"):
            if not _non_placeholder(machine.get(field)):
                errors.append(f"machine.{field} must be non-placeholder text")

    environment = data.get("environment")
    if not isinstance(environment, dict):
        errors.append("environment must be an object")
    else:
        for field in ("operating_system", "architecture", "compiler"):
            if not _non_placeholder(environment.get(field)):
                errors.append(f"environment.{field} must be non-placeholder text")
        revision = environment.get("source_base_revision")
        if not isinstance(revision, str) or not SHA_RE.fullmatch(revision):
            errors.append("environment.source_base_revision must be a full git SHA")
        bundle_digest = environment.get("source_bundle_sha256")
        if not isinstance(bundle_digest, str) or not SHA256_RE.fullmatch(bundle_digest):
            errors.append("environment.source_bundle_sha256 must be a SHA-256 digest")
        else:
            try:
                expected_bundle_digest = source_bundle_sha256()
            except OSError as exc:
                errors.append(f"cannot hash current source bundle: {exc}")
            else:
                if bundle_digest != expected_bundle_digest:
                    errors.append(
                        "environment.source_bundle_sha256 does not match the current benchmark source bundle"
                    )
        binary_digest = environment.get("benchmark_binary_sha256")
        if not isinstance(binary_digest, str) or not SHA256_RE.fullmatch(binary_digest):
            errors.append("environment.benchmark_binary_sha256 must be a SHA-256 digest")
        elif not source_only:
            if benchmark_binary is None:
                errors.append(
                    "benchmark binary verification requires --benchmark-binary or explicit --source-only"
                )
            else:
                try:
                    actual_binary_digest = hashlib.sha256(
                        benchmark_binary.read_bytes()
                    ).hexdigest()
                except OSError as exc:
                    errors.append(f"cannot hash benchmark binary: {exc}")
                else:
                    if binary_digest != actual_binary_digest:
                        errors.append(
                            "environment.benchmark_binary_sha256 does not match the supplied benchmark binary"
                        )
        if environment.get("source_state") != "content-addressed-source-bundle":
            errors.append("environment.source_state must disclose content-addressed provenance")
        try:
            generated = dt.datetime.fromisoformat(
                str(environment.get("generated_utc", "")).replace("Z", "+00:00")
            )
            if generated.tzinfo is None:
                raise ValueError
            now = dt.datetime.now(dt.timezone.utc)
            if generated.astimezone(dt.timezone.utc) > now + dt.timedelta(minutes=5):
                errors.append(
                    "environment.generated_utc is materially in the future"
                )
        except ValueError:
            errors.append("environment.generated_utc must be timezone-qualified ISO-8601")

    build = data.get("build")
    if not isinstance(build, dict) or build.get("type") != "Release":
        errors.append("build.type must be Release")
    elif build.get("flags") != "-O3 -DNDEBUG":
        errors.append("build.flags must record -O3 -DNDEBUG")

    measurement = data.get("measurement")
    if not isinstance(measurement, dict):
        errors.append("measurement must be an object")
    else:
        if measurement.get("frames_per_trial") != 8192:
            errors.append("measurement.frames_per_trial must be 8192")
        if measurement.get("trials") != 31:
            errors.append("measurement.trials must be 31")
        if measurement.get("repetitions_per_batch") != 5:
            errors.append("measurement.repetitions_per_batch must be 5")
        if measurement.get("sample_policy") != "median-per-batch":
            errors.append("measurement.sample_policy must be median-per-batch")
        if measurement.get("epochs") != 3:
            errors.append("measurement.epochs must be 3")
        if measurement.get("epoch_policy") != "median-p95-epoch":
            errors.append("measurement.epoch_policy must be median-p95-epoch")
        if measurement.get("statistics") != ["median", "p95"]:
            errors.append("measurement.statistics must be [median, p95]")

    acceptance = data.get("acceptance")
    budgets: dict[str, float] = {}
    if not isinstance(acceptance, dict):
        errors.append("acceptance must be an object")
    else:
        interpretation = acceptance.get("interpretation")
        if not isinstance(interpretation, str) or not all(
            term in interpretation for term in ("interpolation-evaluator", "excludes", "streaming", "mixing")
        ):
            errors.append("acceptance.interpretation must state evaluator-only exclusions")
        if acceptance.get("unit") != "ns_per_output_frame":
            errors.append("acceptance.unit must be ns_per_output_frame")
        raw_budgets = acceptance.get("tier_p95_budgets")
        if raw_budgets != EXPECTED_BUDGETS:
            errors.append("acceptance tier budgets do not match the ratcheted contract")
        else:
            budgets = raw_budgets
        if acceptance.get("status") != "pass":
            errors.append("acceptance.status must be pass")

    rows = data.get("rows")
    expected_keys = {(tier, ratio, voices) for tier in TIERS for ratio in RATIOS for voices in VOICES}
    actual_keys: set[tuple[str, float, int]] = set()
    if not isinstance(rows, list):
        errors.append("rows must be an array")
        rows = []
    if len(rows) != len(expected_keys):
        errors.append(f"rows must contain exactly {len(expected_keys)} entries")
    for index, row in enumerate(rows):
        prefix = f"rows[{index}]"
        if not isinstance(row, dict):
            errors.append(f"{prefix} must be an object")
            continue
        try:
            key = (str(row["tier"]), float(row["ratio"]), int(row["voices"]))
        except (KeyError, TypeError, ValueError):
            errors.append(f"{prefix} has an invalid matrix key")
            continue
        if key in actual_keys:
            errors.append(f"{prefix} duplicates matrix key {key}")
        actual_keys.add(key)
        if row.get("target_polyphony") is not (key[2] == 8):
            errors.append(f"{prefix}.target_polyphony disagrees with voices")
        values: dict[str, float] = {}
        for field in (
            "median_ns_per_frame",
            "p95_ns_per_frame",
            "median_ns_per_frame_per_voice",
        ):
            value = row.get(field)
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
                errors.append(f"{prefix}.{field} must be finite and positive")
            else:
                values[field] = float(value)
        if len(values) == 3:
            if values["p95_ns_per_frame"] < values["median_ns_per_frame"]:
                errors.append(f"{prefix} p95 must not be below median")
            expected_per_voice = values["median_ns_per_frame"] / key[2]
            if abs(values["median_ns_per_frame_per_voice"] - expected_per_voice) > 0.002:
                errors.append(f"{prefix} per-voice median is inconsistent")
            budget = budgets.get(key[0])
            if budget is not None and values["p95_ns_per_frame"] > budget:
                errors.append(f"{prefix} p95 exceeds {key[0]} budget {budget}")
    missing = expected_keys - actual_keys
    extra = actual_keys - expected_keys
    if missing:
        errors.append(f"matrix is missing {len(missing)} required rows")
    if extra:
        errors.append(f"matrix has {len(extra)} unexpected rows")
    return errors


def _load(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def self_test(
    path: Path,
    benchmark_binary: Path | None,
    source_only: bool,
) -> list[str]:
    baseline = _load(path)
    failures: list[str] = []
    if validate(baseline, benchmark_binary, source_only):
        return ["baseline evidence must validate before running negative controls"]
    mutations = []
    missing_row = copy.deepcopy(baseline)
    missing_row["rows"].pop()
    mutations.append(("missing row", missing_row))
    wrong_scope = copy.deepcopy(baseline)
    wrong_scope["scope"] = "whole-sampler"
    mutations.append(("dishonest scope", wrong_scope))
    over_budget = copy.deepcopy(baseline)
    over_budget["rows"][-1]["p95_ns_per_frame"] = 481.0
    mutations.append(("over budget", over_budget))
    future_dated = copy.deepcopy(baseline)
    future_dated["environment"]["generated_utc"] = "2999-01-01T00:00:00Z"
    mutations.append(("future timestamp", future_dated))
    wrong_source = copy.deepcopy(baseline)
    wrong_source["environment"]["source_bundle_sha256"] = "0" * 64
    mutations.append(("wrong source bundle", wrong_source))
    if not source_only:
        wrong_binary_hash = copy.deepcopy(baseline)
        wrong_binary_hash["environment"]["benchmark_binary_sha256"] = "0" * 64
        mutations.append(("wrong binary hash", wrong_binary_hash))
    for name, mutation in mutations:
        if not validate(mutation, benchmark_binary, source_only):
            failures.append(f"negative control was not detected: {name}")
    if benchmark_binary is not None and not source_only:
        try:
            with tempfile.NamedTemporaryFile() as altered:
                altered.write(benchmark_binary.read_bytes())
                altered.write(b"pulp-benchmark-negative-control")
                altered.flush()
                if not validate(baseline, Path(altered.name), False):
                    failures.append("negative control was not detected: altered binary")
        except OSError as exc:
            failures.append(f"cannot exercise altered-binary negative control: {exc}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", nargs="?", type=Path, default=DEFAULT_EVIDENCE)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--print-source-bundle-sha256", action="store_true")
    verification = parser.add_mutually_exclusive_group()
    verification.add_argument("--benchmark-binary", type=Path)
    verification.add_argument("--source-only", action="store_true")
    args = parser.parse_args()
    if args.print_source_bundle_sha256:
        try:
            print(source_bundle_sha256())
        except OSError as exc:
            print(f"cannot hash source bundle: {exc}", file=sys.stderr)
            return 1
        return 0
    try:
        errors = (
            self_test(args.evidence, args.benchmark_binary, args.source_only)
            if args.self_test
            else validate(_load(args.evidence), args.benchmark_binary, args.source_only)
        )
    except (OSError, json.JSONDecodeError) as exc:
        errors = [f"cannot load evidence: {exc}"]
    if errors:
        for error in errors:
            print(f"sampler interpolation evidence: {error}", file=sys.stderr)
        return 1
    print(f"sampler interpolation evidence verified: {args.evidence}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
