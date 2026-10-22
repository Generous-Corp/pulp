#!/usr/bin/env python3
"""Fail-closed verifier for `pulp.gpu-audio.provider-conformance.v1` receipts.

The receipt is written by the shared-I/O provider conformance kit
(test/harness/shared_io_provider_conformance.hpp). This verifier recomputes
the verdict from the rows instead of trusting the writer's: every required
scenario appears exactly once, nothing was truncated, differential rows carry
a residual within the program tolerance, and a provider that is not a fake
carries an authenticated SharedIoProviderIdentity. A receipt proves the
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
    "receipt_truncation",
    "differential_cpu_reference",
)
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
FAKE_BACKENDS = ("", "fake")


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


def _verify_identity(provider: dict[str, Any]) -> None:
    identity = provider.get("identity")
    _require(isinstance(identity, dict), "provider.identity (SharedIoProviderIdentity) is required")
    _require(set(identity) == set(IDENTITY_FIELDS),
             "provider.identity must carry exactly the SharedIoProviderIdentity fields")
    for key, kind in IDENTITY_FIELDS.items():
        value = identity[key]
        _require(type(value) is kind and (kind is not int or value >= 0),
                 f"provider.identity.{key} has the wrong type")
    if provider["kind"] == "real":
        _require(identity["authenticated"],
                 "a real provider must carry an authenticated SharedIoProviderIdentity")
        _require(identity["provider_revision"] and identity["adapter_name"]
                 and identity["adapter_backend"],
                 "an authenticated identity must name its revision, adapter and backend")
        _require(identity["adapter_backend"] not in FAKE_BACKENDS,
                 "a real provider cannot report a fake backend")
    else:
        _require(identity["adapter_backend"] in FAKE_BACKENDS,
                 "a fake provider cannot claim a real backend")
    if identity["native_runtime_authenticated"]:
        _require(identity["authenticated"],
                 "native runtime authentication requires an authenticated provider")


def verify_receipt(receipt: Any, *, forbid_skips: bool = False,
                   require_pair: bool = False) -> dict[str, Any]:
    _require(isinstance(receipt, dict), "receipt must be a JSON object")
    _require(receipt.get("schema") == SCHEMA, f"schema must be {SCHEMA}")
    _require(receipt.get("kit_version") == KIT_VERSION, f"kit_version must be {KIT_VERSION}")

    provider = receipt.get("provider")
    _require(isinstance(provider, dict), "provider must be an object")
    _require(isinstance(provider.get("name"), str) and provider["name"],
             "provider.name is required")
    _require(provider.get("kind") in ("fake", "real"), "provider.kind must be fake or real")
    _verify_identity(provider)
    capabilities = provider.get("capabilities")
    _require(isinstance(capabilities, dict) and set(capabilities) == set(CAPABILITY_FIELDS)
             and all(type(capabilities[key]) is bool for key in CAPABILITY_FIELDS),
             "provider.capabilities must carry exactly the SharedIoProviderCapabilities flags")

    program = receipt.get("program")
    _require(isinstance(program, dict), "program must be an object")
    tolerance = program.get("tolerance")
    _require(_is_number(tolerance) and 0 <= tolerance <= 1.0e-3,
             "program.tolerance must be a finite number in [0, 1e-3]")
    for key in ("frames", "slots", "blocks"):
        _require(_is_int(program.get(key)) and program[key] > 0, f"program.{key} must be positive")
    _require(isinstance(program.get("name"), str) and program["name"], "program.name is required")

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
            "kind": provider["kind"], "scenarios": len(seen), "skipped": skipped}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--forbid-skips", action="store_true",
                        help="fail when any scenario was skipped")
    parser.add_argument("--require-pair", action="store_true",
                        help="fail unless a provider-pair differential is present")
    args = parser.parse_args(argv)
    try:
        result = verify_receipt(load_receipt(args.receipt), forbid_skips=args.forbid_skips,
                                require_pair=args.require_pair)
    except ReceiptError as exc:
        print(json.dumps({"verdict": "FAIL", "reason": str(exc)}, sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
