#!/usr/bin/env python3
"""Positive and planted-negative tests for the external A3 campaign runner."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
import py_compile
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any
from unittest import mock

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
RUNNER = SCRIPT_DIR / "gpu_first_visible_a3_campaign.py"
sys.path.insert(0, str(SCRIPT_DIR))
import gpu_first_visible_a3_campaign as campaign  # noqa: E402
import test_gpu_first_visible_a3_acceptance as fixture  # noqa: E402


def assert_runner_ignores_acceptance_bytecode() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-a3-campaign-pyc-") as temporary:
        root = Path(temporary)
        copied_runner = root / RUNNER.name
        copied_runner.write_bytes(RUNNER.read_bytes())
        (root / "gpu_contained_process.py").write_bytes(
            (SCRIPT_DIR / "gpu_contained_process.py").read_bytes()
        )
        safe = root / "gpu_first_visible_a3_acceptance.py"
        safe_source = b'CAMPAIGN_ROLES={"standalone"}\n'
        malicious = b'raise SystemExit("planted acceptance bytecode executed")\n'
        safe_source += b"#" * max(0, len(malicious) - len(safe_source))
        malicious += b"#" * max(0, len(safe_source) - len(malicious))
        safe.write_bytes(malicious)
        timestamp = int(safe.stat().st_mtime)
        cache = Path(importlib.util.cache_from_source(str(safe)))
        cache.parent.mkdir(parents=True)
        py_compile.compile(
            str(safe), cfile=str(cache), doraise=True,
            invalidation_mode=py_compile.PycInvalidationMode.TIMESTAMP,
        )
        safe.write_bytes(safe_source)
        os.utime(safe, (timestamp, timestamp))
        completed = subprocess.run(
            [sys.executable, str(copied_runner), "--help"],
            text=True, capture_output=True, check=False,
        )
        assert completed.returncode == 0, (completed.stdout, completed.stderr)


def assert_typed_process_boundary() -> None:
    planted = mock.Mock(pid=8181)
    failure = campaign.contained_process.ProcessTreeTerminationError(
        "adapter-termination-failed: planted"
    )
    with mock.patch.object(
        campaign.contained_process, "terminate_contained", side_effect=failure,
    ):
        try:
            campaign.terminate_adapter(planted)
        except campaign.AdapterTerminationError as error:
            assert error.code in str(error)
        else:
            raise AssertionError("campaign erased the typed termination failure")

    spawned = mock.Mock()
    with mock.patch.object(
        campaign.contained_process, "spawn_contained", return_value=spawned,
    ) as spawn:
        assert campaign.spawn_adapter(["adapter"], cwd=Path("/owned")) is spawned
    spawn.assert_called_once_with(
        ["adapter"], cwd=Path("/owned"), stdin=subprocess.DEVNULL,
    )


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_adapter(path: Path, evidence: Path, mutation: str) -> None:
    source = f'''#!/usr/bin/env python3
import argparse, hashlib, json, shutil, sys, time
from pathlib import Path

SOURCE = Path({str(evidence)!r})
MUTATION = {mutation!r}

parser = argparse.ArgumentParser()
parser.add_argument("--request", required=True, type=Path)
parser.add_argument("--receipt", required=True, type=Path)
args = parser.parse_args()
request = json.loads(args.request.read_text())

if MUTATION == "sleep":
    time.sleep(5)

artifacts = {{
    "health_result": None, "raw_cold": None, "raw_warm": None,
    "product_artifact": None, "host_artifact": None, "trace": None,
    "trace_analysis": None, "blank_negative": None,
    "audio_thread_exclusion": None, "measurement_producer": None,
    "blank_control_binary": None, "audio_control_binary": None,
    "blank_control_provenance": None, "audio_control_provenance": None,
}}
outcome = "inconclusive" if MUTATION == "inconclusive" else "pass"
reason = "external product is unavailable" if outcome != "pass" else None
dependencies = ["product:unavailable"] if outcome != "pass" else []

if outcome == "pass":
    role = request["role"]
    source_role = "standalone" if role == "pulp-standalone" else role
    root = Path(request["artifact_directory"])
    root.mkdir(parents=True, exist_ok=True)
    sources = {{
        "health_result": SOURCE / f"{{source_role}}-health.json",
        "raw_cold": SOURCE / f"{{source_role}}-cold.json",
        "raw_warm": SOURCE / f"{{source_role}}-warm.json",
        "product_artifact": SOURCE / f"{{source_role}}-product.bin",
        "host_artifact": SOURCE / f"{{source_role}}-host.bin",
        "trace": SOURCE / f"{{source_role}}-trace.pftrace",
        "trace_analysis": SOURCE / f"{{source_role}}-trace-analysis.json",
        "measurement_producer": SOURCE / f"{{source_role}}-product.bin",
    }}
    if request["require_controls"] and MUTATION != "missing-controls":
        sources["blank_negative"] = SOURCE / "blank.json"
        sources["audio_thread_exclusion"] = SOURCE / "audio.json"
        sources["blank_control_binary"] = SOURCE / "blank-negative-control.bin"
        sources["audio_control_binary"] = SOURCE / "audio-thread-exclusion-control.bin"
        sources["blank_control_provenance"] = SOURCE / "blank-negative-provenance.json"
        sources["audio_control_provenance"] = SOURCE / "audio-thread-exclusion-provenance.json"
    for name, source_path in sources.items():
        suffix = source_path.suffix or ".bin"
        destination = root / f"{{name}}{{suffix}}"
        shutil.copyfile(source_path, destination)
        artifacts[name] = {{
            "path": destination.relative_to(args.request.parent).as_posix(),
            "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
        }}

    def mutate_json(name, callback):
        ref = artifacts[name]
        path = args.request.parent / ref["path"]
        payload = json.loads(path.read_text())
        callback(payload)
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\\n")
        ref["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()

    if request["require_controls"] and MUTATION != "missing-controls":
        for provenance_name in ("blank_control_provenance", "audio_control_provenance"):
            mutate_json(provenance_name, lambda value: value.update({{
                "campaign_id": request["identity"]["campaign_id"],
                "product_build_id": request["identity"]["build_id"],
            }}))

    if MUTATION == "warm-provenance":
        mutate_json("raw_warm", lambda value: value["samples"][0].__setitem__(
            "cache_provenance", "fresh-process"))
    elif MUTATION == "duplicate-lifecycle":
        mutate_json("raw_cold", lambda value: value["samples"][1].__setitem__(
            "lifecycle_id", value["samples"][0]["lifecycle_id"]))
    elif MUTATION == "audio-event":
        mutate_json("audio_thread_exclusion", lambda value: value.__setitem__(
            "observed_audio_thread_events", 1))
    elif MUTATION == "blank-missed":
        mutate_json("blank_negative", lambda value: value.__setitem__("caught", False))
    elif MUTATION == "trace-evidence":
        mutate_json("trace_analysis", lambda value: value.__setitem__(
            "evidence_ids", ["f" * 32]))
    elif MUTATION == "empty-product":
        ref = artifacts["product_artifact"]
        product = args.request.parent / ref["path"]
        product.write_bytes(b"")
        ref["sha256"] = hashlib.sha256(b"").hexdigest()
    elif MUTATION == "request-mutation":
        args.request.write_text("{{}}\\n")

receipt = {{
    "schema": {campaign.ADAPTER_SCHEMA!r}, "version": 1,
    "attempt_nonce": request["attempt_nonce"], "role": request["role"],
    "outcome": outcome, "reason": reason, "dependencies": dependencies,
    "identity": request["identity"],
    "measurement_endpoint": request["measurement_endpoint"],
    "artifacts": artifacts,
}}
args.receipt.parent.mkdir(parents=True, exist_ok=True)
args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\\n")
if MUTATION == "exit-mismatch":
    raise SystemExit(1)
raise SystemExit({{"pass": 0, "inconclusive": 2}}[outcome])
'''
    path.write_text(source, encoding="utf-8")
    path.chmod(0o755)


def run_adapter(
    root: Path, evidence: Path, identity_path: Path, mutation: str,
    *, controls: bool = True, timeout: float = 10,
) -> tuple[subprocess.CompletedProcess[str], dict[str, Any]]:
    adapter = root / f"adapter-{mutation or 'pass'}.py"
    write_adapter(adapter, evidence, mutation)
    output = root / f"run-{mutation or 'pass'}-{'controls' if controls else 'plain'}"
    command = [
        sys.executable, str(RUNNER), "run-role", "--role", "pulp-standalone",
        "--identity", str(identity_path), "--budget-receipt", str(evidence / "budget.json"),
        "--budget-cold", str(evidence / "budget-cold.json"),
        "--budget-warm", str(evidence / "budget-warm.json"),
        "--adapter", str(adapter), "--output-dir", str(output),
        "--timeout-seconds", str(timeout),
    ]
    if controls:
        command.append("--require-controls")
    run = subprocess.run(command, text=True, capture_output=True, check=False)
    result = json.loads((output / "run.json").read_text()) if (output / "run.json").is_file() else {}
    return run, result


def main() -> int:
    assert_runner_ignores_acceptance_bytecode()
    assert_typed_process_boundary()
    with tempfile.TemporaryDirectory(prefix="pulp-a3-campaign-") as temporary:
        root = Path(temporary)
        evidence = root / "evidence"
        evidence.mkdir()
        receipt = fixture.make_fixture(evidence)
        identity = copy.deepcopy(receipt["campaigns"][0]["identity"])
        identity["forge_revision"] = None
        identity_path = root / "identity.json"
        write_json(identity_path, identity)

        run, result = run_adapter(root, evidence, identity_path, "", controls=True)
        assert run.returncode == 0, run.stderr
        assert result["status"] == "pass"
        assert result["campaign"]["role"] == "pulp-standalone"
        assert result["campaign"]["adapter"] == result["adapter"]
        assert result["campaign"]["measurement_producer"] == result["measurement_producer"]
        assert result["controls"]["blank_negative"] is not None
        assert result["controls"]["audio_thread_exclusion"] is not None
        assert result["controls"]["blank_control_binary"] is not None
        assert result["controls"]["audio_control_binary"] is not None
        assert result["controls"]["blank_control_provenance"] is not None
        assert result["controls"]["audio_control_provenance"] is not None
        assert result["measurement_producer"] is not None
        assert result["campaign"]["identity"] == identity

        run, result = run_adapter(root, evidence, identity_path, "", controls=False)
        assert run.returncode == 0, run.stderr
        assert result["status"] == "pass" and result["controls"] is None

        negatives = {
            "warm-provenance": "cache provenance does not prove warm",
            "duplicate-lifecycle": "lifecycle identities must be unique",
            "audio-event": "audio-thread exclusion",
            "blank-missed": "intended failure",
            "missing-controls": "passing adapter omitted",
            "trace-evidence": "does not exactly corroborate the campaign",
            "empty-product": "product artifact is empty",
            "exit-mismatch": "exit code disagrees",
            "request-mutation": "digest mismatch",
        }
        for mutation, needle in negatives.items():
            run, result = run_adapter(root, evidence, identity_path, mutation)
            assert run.returncode == 1, (mutation, run.stdout, run.stderr)
            assert result["status"] == "fail"
            assert needle in result["reason"], (mutation, result["reason"])

        run, result = run_adapter(root, evidence, identity_path, "inconclusive")
        assert run.returncode == 2, run.stderr
        assert result["status"] == "incomplete"
        assert result["dependencies"] == ["product:unavailable"]

        run, result = run_adapter(
            root, evidence, identity_path, "sleep", timeout=0.05,
        )
        assert run.returncode == 2, run.stderr
        assert result["status"] == "incomplete"
        assert result["dependencies"] == ["adapter:timeout"]

        wrong_forge = copy.deepcopy(identity)
        wrong_forge["forge_revision"] = "a" * 40
        wrong_forge["plugin_format"] = "vst3"
        try:
            campaign.validate_identity(wrong_forge, "forge-modular-standalone")
        except CampaignError as error:
            assert "wrong product/plugin format" in str(error)
        else:
            raise AssertionError("Forge campaign accepted a non-standalone product format")

        print(
            "gpu-first-visible-a3-campaign: positive=2 planted_negatives=12 "
            "typed_process_boundary=pass"
        )
    return 0


CampaignError = campaign.CampaignError


if __name__ == "__main__":
    raise SystemExit(main())
