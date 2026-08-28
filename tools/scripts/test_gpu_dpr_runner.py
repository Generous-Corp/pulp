#!/usr/bin/env python3
"""Deterministic tests and planted bad evidence for the A4 DPR runner."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path

import gpu_dpr_experiment as experiment
import gpu_dpr_runner as runner

SHA_A = "a" * 40
SHA_B = "b" * 40
SHA_C = "c" * 40


def plan(manifest: dict) -> dict:
    class Args:
        experiment_id = "a4-runner-selftest"
        plan_revision = SHA_A
        pulp_sha = SHA_B
        forge_sha = SHA_C

    return experiment.planned_result(Args(), manifest)


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def raw_samples(*, logical_input_ok: bool = True, fidelity_ok: bool = True) -> dict:
    expected = [123.25, 45.5]
    observed = expected if logical_input_ok else [61.625, 22.75]
    return {
        "schema": runner.RAW_SCHEMA,
        "version": 1,
        "metrics": {
            "cpu_frame_time": [2.0 + index / 100 for index in range(30)],
            "gpu_frame_time": [1.0 + index / 100 for index in range(30)],
            "first_frame_time": [10.0 + index / 10 for index in range(20)],
            "interaction_latency": [4.0 + index / 100 for index in range(30)],
            "render_target_bytes": [1024 + index for index in range(30)],
            "resident_bytes": [4096 + index for index in range(30)],
            "upload_bytes": [512 + index for index in range(30)],
        },
        "fidelity": {
            "content_floor_passed": fidelity_ok,
            "capture_similarity": 0.99,
            "small_text_legible": fidelity_ok,
            "thin_strokes_preserved": fidelity_ok,
        },
        "logical_input_trials": [{
            "expected_logical": expected,
            "observed_logical": observed,
            "expected_target": "gain-knob",
            "observed_target": "gain-knob",
        }],
        "trace": {
            "complete": True,
            "categories": ["render", "gpu", "text", "js", "layout"],
            "questions": [
                {"id": "gpu-startup", "status": "unverified", "evidence_id": "trace:startup"},
                {"id": "gpu-health", "status": "pass", "evidence_id": "trace:health"},
                {"id": "gpu-probe", "status": "pass", "evidence_id": "trace:probe"},
            ],
        },
    }


def make_receipt(
    run_dir: Path,
    state: dict,
    manifest: dict,
    key: str,
    *,
    outcome: str = "pass",
    logical_input_ok: bool = True,
    fidelity_ok: bool = True,
) -> Path:
    cell = state["cells"][key]
    scenario = runner.scenario_map(manifest)[cell["scenario_id"]]
    cell_dir = runner.cell_directory(run_dir, key)
    cell_dir.mkdir(parents=True, exist_ok=True)
    if outcome in runner.INCOMPLETE_OUTCOMES:
        receipt = {
            "schema": runner.RECEIPT_SCHEMA,
            "version": 1,
            "scenario_id": cell["scenario_id"],
            "scenario_kind": cell["scenario_kind"],
            "mode": cell["mode"],
            "requested_dpr": cell["requested_dpr"],
            "outcome": outcome,
            "reason": "adapter dependency intentionally absent",
            "dependencies": [f"adapter:{cell['scenario_id']}"],
        }
        path = cell_dir / "receipt.json"
        write_json(path, receipt)
        return path

    raw = raw_samples(logical_input_ok=logical_input_ok, fidelity_ok=fidelity_ok)
    files = {
        "capture": ("capture.png", b"real-capture-bytes"),
        "trace": ("trace.pftrace", b"real-trace-bytes"),
        "raw_samples": ("raw-samples.json", None),
        "input_receipt": ("input-receipt.json", b'{"logical":true}\n'),
    }
    write_json(cell_dir / "raw-samples.json", raw)
    artifacts = []
    for kind, (name, content) in files.items():
        path = cell_dir / name
        if content is not None:
            path.write_bytes(content)
        artifacts.append({"kind": kind, "path": name, "sha256": runner.sha256_file(path)})

    requested = float(cell["requested_dpr"])
    if cell["mode"] == "configured_max":
        observed = min(requested, float(manifest["configured_max_dpr"]))
    else:
        observed = requested
    logical = scenario["logical_size"]
    adapter = {
        "class": "hardware",
        "name": "Selftest GPU",
        "backend": "Metal",
        "driver": "selftest-1",
        "authentic_identity": True,
        "api": "webgl2" if "authentic_webgl" in scenario["required_oracles"] else "webgpu",
    }
    receipt = {
        "schema": runner.RECEIPT_SCHEMA,
        "version": 1,
        "scenario_id": cell["scenario_id"],
        "scenario_kind": cell["scenario_kind"],
        "mode": cell["mode"],
        "requested_dpr": cell["requested_dpr"],
        "observed_dpr": observed,
        "physical_size": {
            "width": round(logical["width"] * observed),
            "height": round(logical["height"] * observed),
        },
        "content_digest": runner.source_digest(
            scenario, Path(state["manifest_path"]), state["plan"]["forge_sha"]
        ),
        "outcome": outcome,
        "reason": None,
        "dependencies": [],
        "machine": {"id": "selftest-m3", "os": "macos", "architecture": "arm64"},
        "adapter": adapter,
        "build_identity": {
            "pulp_sha": state["plan"]["pulp_sha"],
            "forge_sha": state["plan"]["forge_sha"],
            "binary": {"path": "Forge Modular", "sha256": "d" * 64},
        },
        "plugin_format": "vst3",
        "format_qualified": True,
        "scan_cache_confirmed": True,
        "audio_thread_excluded": True,
        "artifacts": artifacts,
    }
    path = cell_dir / "receipt.json"
    write_json(path, receipt)
    return path


def expect_rejected(
    receipt: Path, state: dict, manifest: dict, run_dir: Path, label: str
) -> None:
    try:
        runner.receipt_observation(
            receipt, state, manifest, Path(state["manifest_path"]), run_dir
        )
    except runner.EvidenceError:
        return
    raise AssertionError(f"planted bad evidence unexpectedly passed: {label}")


def test_adapter_script(root: Path) -> Path:
    script = root / "skip-adapter.py"
    script.write_text(
        """#!/usr/bin/env python3
import argparse, json
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--request'); p.add_argument('--receipt'); a=p.parse_args()
r=json.loads(Path(a.request).read_text())
Path(a.receipt).write_text(json.dumps({
  'schema':'pulp.gpu-dpr-cell-receipt.v1','version':1,
  'scenario_id':r['scenario']['id'],'scenario_kind':r['scenario']['kind'],
  'mode':r['mode'],'requested_dpr':r['requested_dpr'],'outcome':'skip',
  'reason':'real product adapter unavailable in selftest',
  'dependencies':['adapter:test-real-product']})+'\\n')
raise SystemExit(2)
""",
        encoding="utf-8",
    )
    script.chmod(0o755)
    return script


def timeout_adapter_script(root: Path) -> Path:
    script = root / "timeout-adapter.py"
    script.write_text(
        """#!/usr/bin/env python3
import time
time.sleep(60)
""",
        encoding="utf-8",
    )
    script.chmod(0o755)
    return script


def malformed_adapter_script(root: Path) -> Path:
    script = root / "malformed-adapter.py"
    script.write_text(
        """#!/usr/bin/env python3
import argparse
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--request'); p.add_argument('--receipt'); a=p.parse_args()
Path(a.receipt).write_text('{not-json')
""",
        encoding="utf-8",
    )
    script.chmod(0o755)
    return script


def main() -> int:
    manifest = runner.load_json(experiment.DEFAULT_MANIFEST)
    with tempfile.TemporaryDirectory(prefix="pulp-dpr-runner-") as temporary:
        root = Path(temporary)
        run_dir = root / "run"
        planned = plan(manifest)
        state = runner.initial_state(planned, manifest, experiment.DEFAULT_MANIFEST)
        runner.save_state(run_dir, state)
        assert len(state["cells"]) == 84
        assert runner.status_document(state)["incomplete_cells"] == 84

        dense = runner.cell_key("dense-text-thin-strokes", "exact", 1)
        good = make_receipt(run_dir, state, manifest, dense)
        key, observation, dependencies = runner.receipt_observation(
            good, state, manifest, Path(state["manifest_path"]), run_dir
        )
        assert key == dense and observation is not None and not dependencies
        assert observation["metrics"]["first_frame_time"]["sample_count"] == 20

        planted = 0
        bad_digest = runner.load_json(good)
        bad_digest["artifacts"][0]["sha256"] = "0" * 64
        write_json(good, bad_digest)
        expect_rejected(good, state, manifest, run_dir, "artifact digest mismatch")
        planted += 1

        good = make_receipt(run_dir, state, manifest, dense, logical_input_ok=False)
        expect_rejected(good, state, manifest, run_dir, "logical coordinate scaling")
        planted += 1

        good = make_receipt(run_dir, state, manifest, dense)
        changed_content = runner.load_json(good)
        changed_content["content_digest"] = "0" * 64
        write_json(good, changed_content)
        expect_rejected(good, state, manifest, run_dir, "content digest drift")
        planted += 1

        gpu_key = runner.cell_key("shader-heavy-controls", "exact", 1)
        gpu_receipt = make_receipt(run_dir, state, manifest, gpu_key)
        software = runner.load_json(gpu_receipt)
        software["adapter"]["class"] = "software"
        write_json(gpu_receipt, software)
        expect_rejected(gpu_receipt, state, manifest, run_dir, "software GPU substitution")
        planted += 1

        gpu_receipt = make_receipt(run_dir, state, manifest, gpu_key)
        raw_path = runner.cell_directory(run_dir, gpu_key) / "raw-samples.json"
        raw = runner.load_json(raw_path)
        raw["trace"]["categories"].remove("gpu")
        write_json(raw_path, raw)
        receipt = runner.load_json(gpu_receipt)
        next(item for item in receipt["artifacts"] if item["kind"] == "raw_samples")["sha256"] = runner.sha256_file(raw_path)
        write_json(gpu_receipt, receipt)
        expect_rejected(gpu_receipt, state, manifest, run_dir, "missing trace category")
        planted += 1

        # The fixed executable protocol calls a scenario adapter without a shell.
        adapter_run = root / "adapter-run"
        adapter_state = runner.initial_state(planned, manifest, experiment.DEFAULT_MANIFEST)
        runner.save_state(adapter_run, adapter_state)
        adapter = test_adapter_script(root)
        runner.run_cells(
            adapter_run, {"dense-text-thin-strokes": adapter}, {dense}, None
        )
        adapter_state = runner.load_state(adapter_run)
        assert adapter_state["cells"][dense]["status"] == "skip"
        assert runner.project_result(adapter_state)["status"] == "incomplete"
        assert runner.status_document(adapter_state)["incomplete_cells"] == 84

        # Missing adapters are explicit resumable dependencies, not fake samples.
        missing_run = root / "missing-run"
        missing_state = runner.initial_state(planned, manifest, experiment.DEFAULT_MANIFEST)
        runner.save_state(missing_run, missing_state)
        runner.run_cells(missing_run, {}, {dense}, None)
        missing_state = runner.load_state(missing_run)
        assert missing_state["cells"][dense]["status"] == "inconclusive"
        assert missing_state["cells"][dense]["dependencies"] == [
            "adapter:dense-text-thin-strokes"
        ]

        # Timeout is a durable incomplete attempt, so resumption never loses it.
        timeout_run = root / "timeout-run"
        timeout_state = runner.initial_state(planned, manifest, experiment.DEFAULT_MANIFEST)
        runner.save_state(timeout_run, timeout_state)
        runner.run_cells(
            timeout_run,
            {"dense-text-thin-strokes": timeout_adapter_script(root)},
            {dense},
            None,
            timeout_seconds=0.05,
        )
        timeout_state = runner.load_state(timeout_run)
        assert timeout_state["cells"][dense]["status"] == "inconclusive"
        assert timeout_state["cells"][dense]["dependencies"] == [
            "adapter:dense-text-thin-strokes:timeout"
        ]
        assert runner.project_result(timeout_state)["status"] == "incomplete"

        malformed_run = root / "malformed-run"
        malformed_state = runner.initial_state(planned, manifest, experiment.DEFAULT_MANIFEST)
        runner.save_state(malformed_run, malformed_state)
        runner.run_cells(
            malformed_run,
            {"dense-text-thin-strokes": malformed_adapter_script(root)},
            {dense},
            None,
        )
        malformed_state = runner.load_state(malformed_run)
        assert malformed_state["cells"][dense]["status"] == "inconclusive"
        assert malformed_state["cells"][dense]["dependencies"] == [
            "valid-cell-receipt"
        ]

        # Fill all 84 cells with full synthetic receipts to exercise projection.
        complete_run = root / "complete-run"
        complete_state = runner.initial_state(planned, manifest, experiment.DEFAULT_MANIFEST)
        runner.save_state(complete_run, complete_state)
        for cell in complete_state["cells"]:
            current = runner.load_state(complete_run)
            receipt_path = make_receipt(complete_run, current, manifest, cell)
            runner.ingest_receipt(complete_run, receipt_path)
        completed = runner.load_state(complete_run)
        assert runner.status_document(completed)["complete_cells"] == 84
        b5 = runner.finalize(
            complete_run, "adaptive-candidate", "a2t:selftest",
            "a3-budget:selftest", "a3:selftest"
        )
        assert b5["status"] == "waiting-trigger"
        assert b5["requires"] == ["B0-adopted-vellum-api-refresh"]
        assert b5["authorizes_policy_change"] is False
        result = runner.load_json(complete_run / "result.json")
        assert result["status"] == "complete" and len(result["observations"]) == 84
        assert not experiment.result_semantic_errors(
            result, manifest, experiment.canonical_sha256(manifest)
        )

        # A planted fidelity failure can only close B5 no-change.
        failed = runner.load_state(complete_run)
        failed["cells"][dense]["observation"]["fidelity"]["small_text_legible"] = False
        runner.save_state(complete_run, failed)
        try:
            runner.finalize(
                complete_run, "configured-max-candidate", "a2t:selftest",
                "a3-budget:selftest", "a3:selftest"
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("candidate crossed a planted fidelity failure")
        b5 = runner.finalize(
            complete_run, "no-change", "a2t:selftest",
            "a3-budget:selftest", "a3:selftest"
        )
        assert b5["status"] == "cancelled-no-change"
        assert b5["authorizes_policy_change"] is False

    print(
        "gpu_dpr_runner_selftest=true matrix_cells=84 "
        f"planted_bad_evidence={planted} adapter_protocol=pass "
        "skip_inconclusive_incomplete=pass timeout_incomplete=pass "
        "malformed_receipt_incomplete=pass b5_gate=pass"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
