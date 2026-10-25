#!/usr/bin/env python3
"""Fail-closed verifier for `pulp.gpu-audio.provider-conformance.v1` receipts.

The receipt is written by the shared-I/O provider conformance kit
(test/harness/shared_io_provider_conformance.hpp). This verifier recomputes
the verdict from the rows instead of trusting the writer's: every required
scenario appears exactly once, nothing was truncated, differential rows carry
a residual within the program tolerance, and a provider that is not a fake
carries an authenticated SharedIoProviderIdentity. The provider's kind is
derived from that identity's adapter backend, never from the writer's label,
and the program and counters are pinned to what the kit actually runs. A
receipt proves the
provider honoured the shared-I/O seam's contract under the kit's injected
faults; it is not evidence of a real device-loss trigger or of DAW behaviour.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


SCHEMA = "pulp.gpu-audio.provider-conformance.v1"
KIT_VERSION = 1
REQUIRED_SCENARIOS = (
    "nominal",
    "force_loss_before_submit",
    "force_loss_between_submit_and_registration",
    "force_loss_after_registration",
    "late_completion",
    "missing_completion",
    "refused_submission",
    "terminal_busy_retry",
    "teardown_quiescence",
    "drain_after_loss",
    "stale_generation_replay",
    "differential_cpu_reference",
)
# The kit's one program; a receipt for any other program is not this kit's.
PROGRAM = {"name": "affine-1.75x+0.25", "scale": 1.75, "bias": 0.25, "tolerance": 1.0e-5}
SKIP_EXIT = 77
OPTIONAL_SCENARIOS = ("differential_provider_pair",)
DIFFERENTIAL_SCENARIOS = ("differential_cpu_reference", "differential_provider_pair")
VERDICTS = ("passed", "failed", "skipped")
IDENTITY_FIELDS = {
    "authenticated": bool,
    "provider_revision": str,
    "adapter_name": str,
    "adapter_backend": str,
    "adapter_vendor_id": int,
    "adapter_device_id": int,
    "native_runtime_authenticated": bool,
    "native_runtime_name": str,
    "native_runtime_backend": str,
}
CAPABILITY_FIELDS = ("imported_host_pointer", "ordered_causal_state", "completion_service",
                     "device_loss_recovery", "gpu_timestamps")
ROW_COUNTERS = ("submissions", "retired_success", "retired_failure", "rejected_callbacks")
FAKE_BACKEND = "fake"


class ReceiptError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ReceiptError(message)


def _is_int(value: Any) -> bool:
    return type(value) is int


def _is_number(value: Any) -> bool:
    return type(value) in (int, float) and math.isfinite(value)


def load_receipt(path: Path) -> Any:
    def reject_constant(value: str) -> None:
        raise ValueError(f"non-finite JSON constant {value}")

    def unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key}")
            result[key] = value
        return result

    try:
        text = path.read_text(encoding="utf-8")
        return json.loads(text, parse_constant=reject_constant, object_pairs_hook=unique)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
        raise ReceiptError(f"cannot parse {path}: {exc}") from exc


def derive_kind(identity: dict[str, Any]) -> str:
    backend = identity["adapter_backend"]
    if backend == FAKE_BACKEND:
        return "fake"
    return "real" if backend else "unidentified"


def _verify_identity(provider: dict[str, Any]) -> str:
    identity = provider.get("identity")
    _require(isinstance(identity, dict), "provider.identity (SharedIoProviderIdentity) is required")
    _require(set(identity) == set(IDENTITY_FIELDS),
             "provider.identity must carry exactly the SharedIoProviderIdentity fields")
    for key, kind in IDENTITY_FIELDS.items():
        value = identity[key]
        _require(type(value) is kind and (kind is not int or value >= 0),
                 f"provider.identity.{key} has the wrong type")
    kind = derive_kind(identity)
    _require(kind != "unidentified",
             "provider identity names no adapter backend, so its kind cannot be derived")
    _require(provider["kind"] == kind,
             f"provider.kind {provider['kind']!r} disagrees with the kind its identity derives "
             f"({kind!r})")
    if kind == "real":
        _require(identity["authenticated"],
                 "a real provider must carry an authenticated SharedIoProviderIdentity")
        _require(identity["provider_revision"] and identity["adapter_name"],
                 "an authenticated identity must name its revision and adapter")
    if identity["native_runtime_authenticated"]:
        _require(identity["authenticated"],
                 "native runtime authentication requires an authenticated provider")
    return kind


def _verify_counters(name: str, row: dict[str, Any], program: dict[str, Any]) -> None:
    """A passed row must carry the counts its scenario necessarily produces."""
    submissions = row["submissions"]
    success = row["retired_success"]
    failure = row["retired_failure"]
    rejected = row["rejected_callbacks"]
    terminals = success + failure

    def need(condition: bool, what: str) -> None:
        _require(condition, f"scenario {name} passed with impossible counters: {what}")

    if name == "nominal":
        need(submissions >= 4 and success == submissions and failure == 0 and rejected == 0,
             "nominal must retire every submission successfully")
    elif name.startswith("force_loss_"):
        need(success == 0 and failure == submissions and submissions <= 1,
             "lost work must retire failed, never successfully")
    elif name == "drain_after_loss":
        need(submissions >= 1 and success == 0 and failure == submissions,
             "drained lost work must all retire failed")
    elif name in ("late_completion", "missing_completion"):
        need(submissions == 1 and terminals == 1 and rejected == 0,
             "withheld work must retire exactly once")
    elif name == "refused_submission":
        need(submissions == 0 and terminals == 0 and rejected == 0,
             "a refusal must publish nothing")
    elif name == "terminal_busy_retry":
        need(submissions == 1 and success == 1 and failure == 0,
             "the busy terminal must retire successfully once")
    elif name == "teardown_quiescence":
        need(submissions >= 1 and terminals == submissions,
             "drain must retire every in-flight submission")
    elif name == "stale_generation_replay":
        need(submissions == 2 and success == 2 and rejected == 1,
             "both epochs retire once and only the kit's replay is rejected")
    elif name == "differential_cpu_reference":
        need(submissions == program["blocks"] and success == submissions and failure == 0,
             "every program block must retire successfully")
    elif name == "differential_provider_pair":
        need(submissions == 2 * program["blocks"] and success == submissions,
             "both providers must run every program block")


def verify_receipt(receipt: Any, *, forbid_skips: bool = False, require_pair: bool = False,
                   require_kind: str | None = None) -> dict[str, Any]:
    _require(isinstance(receipt, dict), "receipt must be a JSON object")
    _require(receipt.get("schema") == SCHEMA, f"schema must be {SCHEMA}")
    _require(receipt.get("kit_version") == KIT_VERSION, f"kit_version must be {KIT_VERSION}")

    provider = receipt.get("provider")
    _require(isinstance(provider, dict), "provider must be an object")
    _require(isinstance(provider.get("name"), str) and provider["name"],
             "provider.name is required")
    _require(provider.get("kind") in ("fake", "real"), "provider.kind must be fake or real")
    kind = _verify_identity(provider)
    if require_kind is not None:
        _require(kind == require_kind, f"provider kind is {kind}, but {require_kind} is required")
    capabilities = provider.get("capabilities")
    _require(isinstance(capabilities, dict) and set(capabilities) == set(CAPABILITY_FIELDS)
             and all(type(capabilities[key]) is bool for key in CAPABILITY_FIELDS),
             "provider.capabilities must carry exactly the SharedIoProviderCapabilities flags")

    program = receipt.get("program")
    _require(isinstance(program, dict), "program must be an object")
    for key, expected in PROGRAM.items():
        _require(program.get(key) == expected,
                 f"program.{key} must be the kit's {expected!r}")
    tolerance = program["tolerance"]
    for key in ("frames", "slots", "blocks"):
        _require(_is_int(program.get(key)) and program[key] > 0, f"program.{key} must be positive")

    _require(receipt.get("required_scenarios") == list(REQUIRED_SCENARIOS),
             "required_scenarios does not match this kit version")

    _require(receipt.get("truncated") is False, "receipt is truncated")
    _require(receipt.get("dropped_rows") == 0, "receipt dropped rows")
    rows = receipt.get("scenarios")
    _require(isinstance(rows, list) and rows, "scenarios must be a non-empty list")
    _require(_is_int(receipt.get("row_count")) and receipt["row_count"] == len(rows),
             "row_count does not match the scenario rows")
    _require(_is_int(receipt.get("capacity")) and receipt["capacity"] >= len(rows),
             "capacity is smaller than the scenario rows")

    seen: dict[str, dict[str, Any]] = {}
    for index, row in enumerate(rows):
        _require(isinstance(row, dict), f"scenarios[{index}] must be an object")
        name = row.get("name")
        _require(name in REQUIRED_SCENARIOS or name in OPTIONAL_SCENARIOS,
                 f"scenarios[{index}] names an unknown scenario")
        _require(name not in seen, f"scenario {name} appears more than once")
        _require(row.get("verdict") in VERDICTS, f"scenario {name} has an invalid verdict")
        _require(isinstance(row.get("reason"), str) and row["reason"],
                 f"scenario {name} must carry a reason")
        for counter in ROW_COUNTERS:
            _require(_is_int(row.get(counter)) and row[counter] >= 0,
                     f"scenario {name}.{counter} must be a non-negative integer")
        residual = row.get("max_abs_error")
        _require(residual is None or (_is_number(residual) and residual >= 0),
                 f"scenario {name}.max_abs_error must be null or non-negative")
        if name in DIFFERENTIAL_SCENARIOS and row["verdict"] == "passed":
            _require(residual is not None and residual <= tolerance,
                     f"scenario {name} passed without a residual within tolerance")
        if row["verdict"] == "passed":
            _verify_counters(name, row, program)
        if row["verdict"] == "skipped":
            _require(not forbid_skips, f"scenario {name} was skipped")
        seen[name] = row

    missing = [name for name in REQUIRED_SCENARIOS if name not in seen]
    _require(not missing, "receipt is missing scenarios: " + ", ".join(missing))
    if require_pair:
        _require("differential_provider_pair" in seen, "receipt lacks a provider-pair differential")
    failed = sorted(name for name, row in seen.items() if row["verdict"] == "failed")
    _require(not failed, "scenarios failed: " + ", ".join(failed))
    _require(receipt.get("verdict") == "passed", "writer verdict disagrees with passing rows")
    skipped = sorted(name for name, row in seen.items() if row["verdict"] == "skipped")
    return {"verdict": "PASS", "schema": SCHEMA, "provider": provider["name"],
            "kind": kind, "scenarios": len(seen), "skipped": skipped}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--forbid-skips", action="store_true",
                        help="fail when any scenario was skipped")
    parser.add_argument("--require-pair", action="store_true",
                        help="fail unless a provider-pair differential is present")
    parser.add_argument("--require-kind", choices=("fake", "real"),
                        help="fail unless the identity derives this provider kind")
    parser.add_argument("--skip-if-missing", action="store_true",
                        help=f"exit {SKIP_EXIT} (skip) when the receipt file does not exist, "
                             "e.g. because the run skipped for lack of a device")
    args = parser.parse_args(argv)
    if args.skip_if_missing and not args.receipt.exists():
        print(json.dumps({"verdict": "SKIP", "reason": f"no receipt at {args.receipt}"},
                         sort_keys=True))
        return SKIP_EXIT
    try:
        result = verify_receipt(load_receipt(args.receipt), forbid_skips=args.forbid_skips,
                                require_pair=args.require_pair, require_kind=args.require_kind)
    except ReceiptError as exc:
        print(json.dumps({"verdict": "FAIL", "reason": str(exc)}, sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
