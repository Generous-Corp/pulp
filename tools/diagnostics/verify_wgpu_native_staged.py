#!/usr/bin/env python3
"""Fail-closed identity and result receipt for the opt-in staged diagnostic."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import subprocess
import sys
from pathlib import Path


SHA256 = re.compile(r"[0-9a-f]{64}\Z")


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(chunk)
    return sha.hexdigest()


def tree_digest(root: Path) -> str:
    files = sorted((p for p in root.rglob("*") if p.is_file()),
                   key=lambda p: p.relative_to(root).as_posix())
    lines = "".join(f"{p.relative_to(root).as_posix()} {digest(p)}\n" for p in files)
    return hashlib.sha256(lines.encode("utf-8")).hexdigest()


def pinned_asset(manifest: dict, name: str) -> tuple[str, str]:
    rows = [row for row in manifest.get("dependencies", [])
            if isinstance(row, dict) and row.get("name") == "wgpu-native"]
    if len(rows) != 1 or rows[0].get("version") != "v24.0.3.1":
        raise ValueError("wgpu_native_pin_missing_or_changed")
    assets = [row for row in rows[0].get("artifacts", [])
              if isinstance(row, dict) and row.get("name") == name]
    if len(assets) != 1:
        raise ValueError("wgpu_native_asset_missing_or_duplicate")
    asset = assets[0]
    for field in ("sha256", "tree_sha256"):
        if not isinstance(asset.get(field), str) or SHA256.fullmatch(asset[field]) is None:
            raise ValueError(f"invalid_{field}")
    return rows[0]["version"], asset["tree_sha256"]


def parse_output(stdout: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in stdout.splitlines():
        if line.startswith("loaded_runtime_path="):
            result["loaded_runtime_path"] = line.removeprefix("loaded_runtime_path=")
        elif line.startswith("input_sha256="):
            for field in line.split():
                key, value = field.split("=", 1)
                result[key] = value
        elif line.startswith("staged_blocks="):
            for field in line.split():
                key, value = field.split("=", 1)
                result[key] = value
        elif line.startswith("device_loss_proof="):
            for field in line.split():
                key, value = field.split("=", 1)
                result[key] = value
        elif line.startswith("device_loss_callback="):
            for field in line.split():
                key, value = field.split("=", 1)
                result[key] = value
    return result


def validate_loss_evidence(observed: dict[str, str]) -> int:
    """Require typed loss evidence before emitting a diagnostic receipt."""
    if observed.get("device_loss_callback") != "registered":
        raise ValueError("device_loss_callback_not_registered")
    if observed.get("device_loss_scenario") != "triggered":
        raise ValueError("device_loss_scenario_not_triggered")
    if observed.get("device_loss_reason") != "destroyed":
        raise ValueError("device_loss_reason_not_destroyed")
    records = observed.get("device_loss_terminal_records", "")
    if not records.isdigit() or int(records) < 1:
        raise ValueError("device_loss_terminal_records_missing")
    if observed.get("device_loss_no_output_leases") != "true":
        raise ValueError("device_loss_output_lease_evidence_missing")
    if observed.get("device_loss_reprime") != "unavailable":
        raise ValueError("device_loss_reprime_must_remain_unavailable")
    if observed.get("device_loss_proof") != "callback_terminal_no_output_lease":
        raise ValueError("device_loss_proof_mismatch")
    if observed.get("callback_fallback_proof") != "unavailable":
        raise ValueError("callback_fallback_must_remain_unavailable")
    return int(records)


def verify(args: argparse.Namespace) -> dict:
    manifest = Path(args.manifest).resolve(strict=True)
    source_tree = Path(args.source_tree).resolve(strict=True)
    linked_runtime = Path(args.linked_runtime).resolve(strict=True)
    executable = Path(args.executable).resolve(strict=True)
    version, expected_tree = pinned_asset(json.loads(manifest.read_text(encoding="utf-8")),
                                          args.asset)
    if tree_digest(source_tree) != expected_tree:
        raise ValueError("pinned_runtime_tree_mismatch")
    source_matches = [path for path in source_tree.rglob(linked_runtime.name)
                      if path.is_file()]
    if len(source_matches) != 1 or digest(source_matches[0]) != digest(linked_runtime):
        raise ValueError("linked_runtime_not_pinned")
    manifest_before = digest(manifest)
    runtime_before = digest(linked_runtime)
    executable_before = digest(executable)
    run = subprocess.run([str(executable)], capture_output=True, text=True,
                         encoding="utf-8", timeout=30, check=False)
    if run.returncode == 77:
        raise RuntimeError("provider_unavailable")
    if run.returncode != 0:
        raise ValueError(f"diagnostic_failed_{run.returncode}: {run.stderr.strip()}")
    observed = parse_output(run.stdout)
    if Path(observed["loaded_runtime_path"]).resolve(strict=True) != linked_runtime:
        raise ValueError("loaded_runtime_differs_from_linked_runtime")
    if (digest(manifest) != manifest_before or digest(linked_runtime) != runtime_before or
            digest(executable) != executable_before or tree_digest(source_tree) != expected_tree):
        raise ValueError("identity_changed_during_diagnostic")
    for field in ("input_sha256", "weights_sha256", "shader_sha256"):
        if SHA256.fullmatch(observed.get(field, "")) is None:
            raise ValueError(f"invalid_{field}")
    if (observed.get("staged_blocks") != "5" or
            observed.get("planted_completion_failures") != "1" or
            observed.get("callback_fallback_proof") != "unavailable"):
        raise ValueError("diagnostic_disposition_mismatch")
    loss_terminal_records = validate_loss_evidence(observed)
    error = float(observed["cpu_oracle_max_error"])
    if not math.isfinite(error) or error < 0 or error > 1.0e-5:
        raise ValueError("cpu_oracle_parity_failed")
    config = {"blocks": 6, "samples_per_block": 64, "channels": 3,
              "planted_failure_sequence": 4}
    config_bytes = json.dumps(config, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return {
        "schema": "pulp.gpu-audio.wgpu-native-staged-diagnostic.v1",
        "status": "passed",
        "claim": "staged_architecture_only",
        "provider_identity_status": "runtime_image_bound_adapter_auth_unavailable",
        "gpu_receipt_authenticated": False,
        "provider_version": version,
        "manifest_sha256": manifest_before,
        "pinned_tree_sha256": expected_tree,
        "loaded_runtime_sha256": runtime_before,
        "executable_sha256": executable_before,
        "input_sha256": observed["input_sha256"],
        "weights_sha256": observed["weights_sha256"],
        "shader_sha256": observed["shader_sha256"],
        "config": config,
        "config_sha256": hashlib.sha256(config_bytes).hexdigest(),
        "successful_blocks": 5,
        "retired_failure_blocks": 1,
        "cpu_oracle_max_error": error,
        "device_loss_callback": "registered",
        "device_loss_scenario": "triggered",
        "device_loss_reason": "destroyed",
        "device_loss_proof": "callback_terminal_no_output_lease",
        "device_loss_terminal_records": loss_terminal_records,
        "device_loss_no_output_leases": True,
        "device_loss_reprime": "unavailable",
        "callback_fallback_proof": "unavailable",
        "gpu_timestamps": "unavailable",
        "product_proof": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    for name in ("manifest", "asset", "source-tree", "linked-runtime", "executable",
                 "receipt"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    try:
        Path(args.receipt).unlink(missing_ok=True)
        receipt = verify(args)
    except (OSError, KeyError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"wgpu_native_staged_diagnostic_failed: {error}", file=sys.stderr)
        return 77 if str(error) == "provider_unavailable" else 1
    encoded = json.dumps(receipt, sort_keys=True, separators=(",", ":")) + "\n"
    Path(args.receipt).write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
