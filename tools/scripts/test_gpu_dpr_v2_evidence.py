#!/usr/bin/env python3
"""Adversarial retained-byte, runner, and Git-object tests for A4 DPR v2."""

from __future__ import annotations

import copy
import hashlib
import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from typing import Any
from unittest import mock

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))
import gpu_dpr_evidence as v1_evidence  # noqa: E402
import gpu_dpr_experiment as experiment  # noqa: E402
import gpu_dpr_v2_evidence as evidence  # noqa: E402
import gpu_dpr_v2_runner as runner  # noqa: E402
import gpu_dpr_v2_terminal as terminal  # noqa: E402


SHA_A = "a" * 40
SHA_B = "b" * 40
SHA_C = "c" * 40
SHA_D = "d" * 40
A3_DOCUMENT = {
    "schema": "pulp.gpu-vellum-package-terminal.v1",
    "version": 1,
    "package": "a3-pulp-dpr-product-policy",
}


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def png_bytes(width: int, height: int) -> bytes:
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(((x * 17 + y * 3) % 256, (x * 5 + y * 19) % 256, (x + y * 11) % 256))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)
        )

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b"")
    )


def small_manifest() -> tuple[dict[str, Any], dict[str, Any]]:
    scenario = {
        "id": "web-case",
        "policy_class": "web",
        "required_provider": "WebGL2",
        "required_host": "browser",
        "kind": "maintained_web_canary",
        "logical_size": {"width": 16, "height": 16},
        "logical_input_oracle": {"point": [2, 3], "target": "view:test"},
        "required_oracles": [
            "content_floor", "capture_similarity", "logical_input", "authentic_webgl"
        ],
    }
    manifest = {
        "configured_max_dpr": 2,
        "adaptive_profile": {
            "id": "test", "scale_ladder": [1, 1.5, 2, 3],
            "shipping": False,
        },
        "trial_contract": {
            "required_trace_categories": ["render", "gpu", "text", "js", "layout"],
            "gpu_timer_calibration_trials": 5,
            "gpu_timer_extra_work_multiplier": 8,
        },
        "v2_protocol": {
            "product_policy": {
                "a3_receipt_sha256": evidence.canonical_sha256(A3_DOCUMENT)
            },
        },
        "scenarios": [scenario],
    }
    return manifest, scenario


def cell_skeleton(
    scenario: dict[str, Any], manifest: dict[str, Any], campaign: str, ordinal: int,
    *, mode: str = "exact", requested_dpr: float = 1,
    adapter_sha256: str = "2" * 64, build_sha: str = SHA_A,
) -> dict[str, Any]:
    nonce = hashlib.sha256(f"nonce:{campaign}:{ordinal}".encode()).hexdigest()[:32]
    observed = (
        min(requested_dpr, manifest["configured_max_dpr"])
        if mode == "configured_max" else requested_dpr
    )
    pid = 1_000_000 + ordinal * 100
    trials = [{
        "trial_index": index,
        "mode_order": ["exact", "configured_max", "adaptive_simulation"],
        "frame_count": 240,
        "gpu_p95_ns": 1,
        "cpu_median_ns": 1,
        "cpu_p95_ns": 1,
        "first_frame_median_ns": 1,
        "first_frame_p95_ns": 1,
        "interaction_median_ns": 1,
        "interaction_p95_ns": 1,
        "render_target_p95_bytes": 1,
        "resident_p95_bytes": 1,
        "upload_p95_bytes": 1,
        "frame_misses": 0,
        "xruns": 0,
        "affected": False,
        "sequence_sha256": hashlib.sha256(f"trial:{nonce}:{index}".encode()).hexdigest(),
    } for index in range(30)]
    fresh = [{
        "trial_index": index,
        "pid": pid + index + 1,
        "first_nonblank_present_ns": 1,
        "started_at_highest_eligible_rung": True,
        "counters_zero": True,
        "sequence_sha256": hashlib.sha256(f"fresh:{nonce}:{index}".encode()).hexdigest(),
    } for index in range(20)]
    return {
        "campaign": campaign,
        "scenario_id": scenario["id"],
        "policy_class": scenario["policy_class"],
        "mode": mode,
        "requested_dpr": requested_dpr,
        "observed_dpr": observed,
        "attempt_nonce": nonce,
        "outcome": "pass",
        "identity": {
            "machine_id": "runner-machine",
            "provider": scenario["required_provider"],
            "adapter": "hardware-adapter",
            "adapter_sha256": adapter_sha256,
            "build_sha": build_sha,
            "host": scenario["required_host"],
            "format": "web" if scenario["kind"] == "maintained_web_canary" else "standalone",
            "app": scenario["id"],
            "producer_process_id": pid - 1,
            "process_id": pid,
            "instance_id": f"instance:{nonce}",
            "product_sha256": "0" * 64,
        },
        "logical_size": scenario["logical_size"],
        "physical_size": {
            "width": round(scenario["logical_size"]["width"] * observed),
            "height": round(scenario["logical_size"]["height"] * observed),
        },
        "physical_dimensions_verified": True,
        "trace": (
            {"complete": True, "kind": "browser-devtools", "process_pid": pid}
            if scenario["kind"] == "maintained_web_canary" else {"complete": True}
        ),
        "warmup_count": 5,
        "measured_trials": trials,
        "adaptive_summary": None,
        "fresh_process_trials": fresh,
        "fidelity": {},
        "daw_subreceipts": [],
        "artifacts": [],
    }


def retain_cell(
    root: Path, cell: dict[str, Any], scenario: dict[str, Any], manifest: dict[str, Any],
    prefix: str,
) -> dict[str, Any]:
    product = Path(sys.executable).read_bytes()
    product_format = evidence._file_format(product[:65536])
    if product_format is None:
        raise AssertionError("test interpreter is not a recognized executable product")
    product_sha = hashlib.sha256(product).hexdigest()
    cell["identity"]["product_sha256"] = product_sha
    physical = cell["physical_size"]
    capture_payload = png_bytes(physical["width"], physical["height"])
    capture_path = root / prefix / "capture.png"
    reference_path = root / prefix / "reference.png"
    capture_path.parent.mkdir(parents=True, exist_ok=True)
    capture_path.write_bytes(capture_payload)
    reference_path.write_bytes(capture_payload)
    width, height, content, similarity, text, stroke = v1_evidence.recompute_fidelity(
        capture_path, reference_path, scenario, float(cell["observed_dpr"])
    )
    assert (width, height) == (physical["width"], physical["height"])
    required = set(scenario["required_oracles"])
    cell["fidelity"] = {
        "capture_similarity": similarity,
        "small_text_luminance_stddev": text if "small_text" in required else "not-applicable-by-manifest",
        "thin_stroke_coverage": stroke if "thin_strokes" in required else "not-applicable-by-manifest",
        "content_floor": content,
        "logical_input_exact": True,
        "identity": True,
    }
    retained_trials = []
    for trial in cell["measured_trials"]:
        count = trial["frame_count"]
        samples = {
            "gpu_ns": [trial["gpu_p95_ns"]] * count,
            "cpu_ns": [trial["cpu_median_ns"]] * count,
            "first_frame_ns": [trial["first_frame_median_ns"]] * count,
            "interaction_ns": [trial["interaction_median_ns"]] * count,
            "render_target_bytes": [trial["render_target_p95_bytes"]] * count,
            "resident_bytes": [trial["resident_p95_bytes"]] * count,
            "upload_bytes": [trial["upload_p95_bytes"]] * count,
            "frame_missed": [False] * count,
            "xruns": [0] * count,
        }
        trial["sequence_sha256"] = evidence.canonical_sha256(samples)
        retained_trials.append({
            "trial_index": trial["trial_index"],
            "mode_order": trial["mode_order"],
            "frame_samples": samples,
            "sequence_sha256": trial["sequence_sha256"],
        })
    binding = evidence.binding_sha256(cell)
    product_path = root / prefix / "product.bin"
    product_path.write_bytes(product)
    product_path.chmod(0o755)
    trace_events = [{
        "name": f"pulp.dpr.{cell['attempt_nonce']}.{category}",
        "pid": cell["identity"]["process_id"],
        "ph": "X",
        "dur": 1,
    } for category in manifest["trial_contract"]["required_trace_categories"]]
    trace = {"traceEvents": trace_events}
    daw_documents = []
    daw_projections = []
    for index, projection in enumerate(cell["daw_subreceipts"]):
        daw_root = root / prefix / "daw" / str(index)
        a3_path = daw_root / "a3-evidence.json"
        host_path = daw_root / "host.bin"
        lifecycle_path = daw_root / "lifecycle.json"
        write_json(a3_path, A3_DOCUMENT)
        host_path.write_bytes(product)
        host_path.chmod(0o755)
        def reference(path: Path) -> dict[str, Any]:
            payload = path.read_bytes()
            return {
                "path": path.relative_to(root).as_posix(),
                "sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload),
            }
        a3_reference = reference(a3_path)
        product_reference = reference(product_path)
        host_reference = reference(host_path)
        lifecycle = {
            "schema": "pulp.gpu-dpr-daw-lifecycle.v2", "version": 2,
            "binding_sha256": binding,
            "a3_receipt_sha256": manifest["v2_protocol"]["product_policy"]["a3_receipt_sha256"],
            "product_sha256": product_sha,
            "host_executable_sha256": host_reference["sha256"],
            "format": projection["format"], "host": projection["host"],
            "a3_role": projection["a3_role"],
            "lifecycle_id": f"lifecycle:{cell['attempt_nonce']}:{index}",
            "process_start_identity": f"start:{cell['attempt_nonce']}:{index}",
        }
        write_json(lifecycle_path, lifecycle)
        lifecycle_reference = reference(lifecycle_path)
        retained_identity = {
            "a3_receipt_sha256": manifest["v2_protocol"]["product_policy"]["a3_receipt_sha256"],
            "product_sha256": product_sha,
            "host_executable_sha256": host_reference["sha256"],
            "process_start_identity": lifecycle["process_start_identity"],
            "lifecycle_id": lifecycle["lifecycle_id"],
            "format": projection["format"],
            "host": projection["host"],
            "a3_role": projection["a3_role"],
        }
        document = {
            "schema": "pulp.gpu-dpr-daw-subreceipt.v2", "version": 2,
            "binding_sha256": binding,
            "format": projection["format"], "host": projection["host"],
            "a3_role": projection["a3_role"],
            "outcome": projection["outcome"],
            "gates_passed": projection["gates_passed"],
            "a3_evidence": a3_reference, "product": product_reference,
            "host_product": host_reference, "lifecycle": lifecycle_reference,
        }
        projection["identity_sha256"] = evidence.canonical_sha256(retained_identity)
        projection["receipt_sha256"] = evidence.canonical_sha256(document)
        daw_documents.append(document)
        daw_projections.append(projection)
    cell["daw_subreceipts"] = daw_projections
    json_documents = {
        "raw_trials": {
            "schema": "pulp.gpu-dpr-raw-trials.v2", "version": 2,
            "binding_sha256": binding, "cell": evidence.cell_payload(cell),
        },
        "frame_sequences": {
            "schema": "pulp.gpu-dpr-frame-sequences.v2", "version": 2,
            "binding_sha256": binding,
            "gpu_timer_calibration": {
                "schema": "pulp.gpu-dpr-timer-calibration.v2", "version": 2,
                "clock": {
                    "Dawn/WebGPU": "dawn-gpu-timestamp",
                    "WebGL2": "webgl2-timer-query",
                }[scenario["required_provider"]],
                "resolution_ns": 1,
                "baseline_samples_ns": [10] * 5,
                "extra_work_samples_ns": [20] * 5,
                "extra_work_multiplier": 8, "control_detected": True,
            },
            "measured": retained_trials,
            "fresh": [{
                "trial_index": item["trial_index"], "pid": item["pid"],
                "sequence_sha256": item["sequence_sha256"],
            } for item in cell["fresh_process_trials"]],
        },
        "input_receipt": {
            "schema": "pulp.gpu-dpr-input-receipt.v2", "version": 2,
            "binding_sha256": binding, "logical_size": cell["logical_size"],
            "physical_size": cell["physical_size"],
            "physical_dimensions_verified": True, "logical_input_exact": True,
            "event": {
                "requested_logical_point": scenario["logical_input_oracle"]["point"],
                "requested_physical_point": [
                    round(value * cell["observed_dpr"], 6)
                    for value in scenario["logical_input_oracle"]["point"]
                ],
                "observed_physical_point": [
                    round(value * cell["observed_dpr"])
                    for value in scenario["logical_input_oracle"]["point"]
                ],
                "observed_logical_point": [
                    round(value * cell["observed_dpr"]) / cell["observed_dpr"]
                    for value in scenario["logical_input_oracle"]["point"]
                ],
                "event_received": True,
                "expected_target": scenario["logical_input_oracle"]["target"],
                "observed_target": scenario["logical_input_oracle"]["target"],
                "process_id": cell["identity"]["process_id"],
            },
        },
        "identity_receipt": {
            "schema": "pulp.gpu-dpr-identity-receipt.v2", "version": 2,
            "binding_sha256": binding, "identity": cell["identity"],
            "process_ids": [
                cell["identity"]["producer_process_id"],
                cell["identity"]["process_id"],
                *[item["pid"] for item in cell["fresh_process_trials"]],
            ],
            "product": {
                "sha256": product_sha, "bytes": len(product), "file_format": product_format,
                "app": cell["identity"]["app"], "format": cell["identity"]["format"],
                "host": cell["identity"]["host"], "provider": cell["identity"]["provider"],
            },
            "daw_subreceipts": daw_projections,
            "daw_receipt_documents": daw_documents,
        },
    }
    sources: dict[str, Path] = {
        "capture": capture_path,
        "reference_capture": reference_path,
        "product": product_path,
    }
    trace_path = root / prefix / "trace.pftrace"
    write_json(trace_path, trace)
    sources["trace"] = trace_path
    for kind, document in json_documents.items():
        path = root / prefix / f"{kind}.json"
        write_json(path, document)
        sources[kind] = path
    artifacts = []
    for kind in sorted(evidence.ARTIFACT_KINDS):
        path = sources[kind]
        payload = path.read_bytes()
        artifacts.append({
            "kind": kind,
            "path": path.relative_to(root).as_posix(),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "bytes": len(payload),
            "binding_sha256": binding,
        })
    cell["artifacts"] = artifacts
    return cell


def analyzer_identity() -> dict[str, str]:
    return {"path": "/unused-for-web-traces", "sha256": "f" * 64}


def expect_artifact_failure(
    result: dict[str, Any], manifest: dict[str, Any], root: Path, label: str,
    *, contains: str | None = None,
) -> None:
    errors = evidence.result_artifact_errors(result, manifest, root, analyzer_identity())
    if not errors:
        raise AssertionError(f"planted A4 artifact negative passed: {label}")
    if contains is not None and not any(contains in error for error in errors):
        raise AssertionError(
            f"planted A4 artifact negative failed for the wrong reason: {label}: {errors[:3]}"
        )


def rewrite_json_artifact(root: Path, cell: dict[str, Any], kind: str, value: Any) -> None:
    descriptor = next(item for item in cell["artifacts"] if item["kind"] == kind)
    path = root / descriptor["path"]
    write_json(path, value)
    payload = path.read_bytes()
    descriptor["sha256"] = hashlib.sha256(payload).hexdigest()
    descriptor["bytes"] = len(payload)


def retained_documents(root: Path, cell: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        item["kind"]: json.loads((root / item["path"]).read_text(encoding="utf-8"))
        for item in cell["artifacts"]
        if item["kind"] in evidence.JSON_ARTIFACT_KINDS
    }


def local_runner_state(
    run_dir: Path, manifest: dict[str, Any], analyzer: Path,
) -> dict[str, Any]:
    run_dir.mkdir(mode=0o700)
    evidence.initialize_run_key(run_dir)
    analyzer_payload = analyzer.read_bytes()
    analyzer_sha = hashlib.sha256(analyzer_payload).hexdigest()
    analyzer_target = run_dir / "tooling" / "trace-analyzer.sh"
    evidence.snapshot_regular(
        analyzer.parent, analyzer.name, analyzer_target, "test trace analyzer",
        max_bytes=1024 * 1024, expected_sha256=analyzer_sha,
        expected_bytes=len(analyzer_payload), executable=True,
    )
    analyzer_descriptor = {
        "path": analyzer_target.relative_to(run_dir).as_posix(),
        "sha256": analyzer_sha,
        "bytes": len(analyzer_payload),
    }
    runner._write_new_json(
        run_dir / evidence.TRACE_ANALYZER_DESCRIPTOR, analyzer_descriptor
    )
    runner._write_new_json(run_dir / runner.AUTHORIZED_MANIFEST, manifest)
    state = {
        "schema": evidence.RUN_STATE_SCHEMA,
        "version": 2,
        "run_id": "1" * 64,
        "root_identity": runner.run_identity(run_dir),
        "initialized_protected_head": SHA_A,
        "experiment_id": "test-real-runner",
        "plan_revision": SHA_A,
        "pulp_sha": SHA_A,
        "forge_sha": SHA_B,
        "installed_revisions": {
            "pulp": SHA_A, "forge": SHA_B, "vellum": SHA_C, "provider": SHA_D,
        },
        "trace_analyzer": analyzer_descriptor,
        "manifest": {
            "path": runner.AUTHORIZED_MANIFEST,
            "sha256": experiment.canonical_sha256(manifest),
        },
        "dependencies": {},
        "cells": runner._expected_cells(manifest),
    }
    runner.save_state(run_dir, state)
    return state


def producer_main(arguments: list[str]) -> int:
    request_path = Path(arguments[arguments.index("--request") + 1])
    output = Path(arguments[arguments.index("--output") + 1])
    request = json.loads(request_path.read_text(encoding="utf-8"))
    manifest = json.loads(
        (request_path.parents[3] / runner.AUTHORIZED_MANIFEST).read_text(encoding="utf-8")
    )
    expected = request["expected_cell"]
    scenario = next(item for item in manifest["scenarios"] if item["id"] == expected["scenario_id"])
    cell = cell_skeleton(
        scenario, manifest, expected["campaign"], 777,
        mode=expected["mode"], requested_dpr=expected["requested_dpr"],
        adapter_sha256=request["adapter"]["sha256"],
        build_sha=request["installed_revisions"]["pulp"],
    )
    cell["identity"]["producer_process_id"] = os.getpid() + (
        1 if os.environ.get("PULP_A4_TEST_BAD_PRODUCER_PID") == "1" else 0
    )
    cell["attempt_nonce"] = request["attempt_nonce"]
    retain_cell(output, cell, scenario, manifest, "artifacts")
    receipt = {
        "schema": runner.PRODUCER_RECEIPT_SCHEMA,
        "version": 2,
        "run_id": request["run_id"],
        "cell_key": request["cell_key"],
        "attempt_nonce": request["attempt_nonce"],
        "request_sha256": hashlib.sha256(request_path.read_bytes()).hexdigest(),
        "cell": cell,
    }
    write_json(output / "receipt.json", receipt)
    return 0


def git_object_tests(root: Path) -> None:
    repository = root / "repo"
    repository.mkdir()
    subprocess.run(["git", "init", "-q"], cwd=repository, check=True)
    subprocess.run(["git", "config", "user.email", "a4@example.invalid"], cwd=repository, check=True)
    subprocess.run(["git", "config", "user.name", "A4 Test"], cwd=repository, check=True)
    relative = Path("docs/candidate.json")
    write_json(repository / relative, {"candidate": True})
    subprocess.run(["git", "add", relative.as_posix()], cwd=repository, check=True)
    subprocess.run(["git", "commit", "-qm", "candidate"], cwd=repository, check=True)
    head = terminal.git_head(repository)
    blob = terminal.canonical_blob(repository, relative, head)
    assert blob["type"] == "blob" and blob["mode"] == "100644"
    try:
        terminal.canonical_blob(repository, relative, "0" * 40)
    except (ValueError, subprocess.SubprocessError):
        pass
    else:
        raise AssertionError("wrong protected head passed canonical blob validation")
    (repository / relative).write_text("substituted\n", encoding="utf-8")
    try:
        terminal.canonical_blob(repository, relative, head)
    except ValueError:
        pass
    else:
        raise AssertionError("dirty/substituted canonical bytes passed")
    subprocess.run(["git", "restore", relative.as_posix()], cwd=repository, check=True)
    target = repository / "outside.json"
    write_json(target, {"outside": True})
    (repository / relative).unlink()
    (repository / relative).symlink_to(target)
    try:
        terminal.canonical_blob(repository, relative, head)
    except ValueError:
        pass
    else:
        raise AssertionError("canonical symlink passed")
    try:
        terminal.canonical_blob(repository, Path("../outside.json"), head)
    except ValueError:
        pass
    else:
        raise AssertionError("outside canonical path passed")
    try:
        terminal.canonical_blob(repository, Path("docs/missing.json"), head)
    except ValueError:
        pass
    else:
        raise AssertionError("wrong-head/missing Git blob passed")
    (repository / relative).unlink()
    subprocess.run(["git", "rm", "-q", relative.as_posix()], cwd=repository, check=True)
    (repository / relative).mkdir(parents=True)
    write_json(repository / relative / "nested.json", {"tree": True})
    subprocess.run(["git", "add", relative.as_posix()], cwd=repository, check=True)
    subprocess.run(["git", "commit", "-qm", "tree"], cwd=repository, check=True)
    try:
        terminal.canonical_blob(repository, relative, terminal.git_head(repository))
    except ValueError:
        pass
    else:
        raise AssertionError("wrong Git object type passed")


def main() -> int:
    manifest, scenario = small_manifest()
    with tempfile.TemporaryDirectory(prefix="pulp-a4-v2-files-") as directory:
        root = Path(directory).resolve()
        cells = [
            retain_cell(
                root,
                cell_skeleton(scenario, manifest, "original" if index < 84 else "repeat", index),
                scenario, manifest, f"cell-{index}",
            )
            for index in range(168)
        ]
        result = {"cells": cells[:84], "repeat_cells": cells[84:]}
        problems = evidence.result_artifact_errors(
            result, manifest, root, analyzer_identity()
        )
        if problems:
            raise AssertionError(f"real 168-cell retained corpus failed: {problems[:3]}")
        assert len(list(root.rglob("*"))) >= 168 * len(evidence.ARTIFACT_KINDS)

        missing = copy.deepcopy(result)
        missing_path = root / missing["cells"][0]["artifacts"][0]["path"]
        saved_missing = missing_path.read_bytes()
        missing_path.unlink()
        expect_artifact_failure(missing, manifest, root, "missing file")
        missing_path.write_bytes(saved_missing)

        wrong_digest = copy.deepcopy(result)
        wrong_digest["cells"][0]["artifacts"][0]["sha256"] = "0" * 64
        expect_artifact_failure(wrong_digest, manifest, root, "wrong digest")
        for kind, replacement in (
            ("capture", b"not-a-png"),
            ("trace", b'{"traceEvents":[]}\n'),
            ("input_receipt", b'{"event":"invented"}\n'),
            ("product", b"#!/bin/sh\nexit 0\n"),
        ):
            wrong_content = copy.deepcopy(result)
            descriptor = next(
                item for item in wrong_content["cells"][0]["artifacts"]
                if item["kind"] == kind
            )
            content_path = root / descriptor["path"]
            original_content = content_path.read_bytes()
            original_mode = stat.S_IMODE(content_path.stat().st_mode)
            content_path.chmod(0o755)
            content_path.write_bytes(replacement)
            descriptor["sha256"] = hashlib.sha256(replacement).hexdigest()
            descriptor["bytes"] = len(replacement)
            expect_artifact_failure(wrong_content, manifest, root, f"wrong {kind} content")
            content_path.write_bytes(original_content)
            content_path.chmod(original_mode)
        traversal = copy.deepcopy(result)
        traversal["cells"][0]["artifacts"][0]["path"] = "../outside"
        expect_artifact_failure(traversal, manifest, root, "outside path")
        cross_cell = {"cells": [cells[0], copy.deepcopy(cells[0])], "repeat_cells": []}
        expect_artifact_failure(cross_cell, manifest, root, "cross-cell reuse")
        cross_binding = copy.deepcopy(result)
        cross_binding["cells"][0]["artifacts"][0] = copy.deepcopy(
            cross_binding["cells"][1]["artifacts"][0]
        )
        expect_artifact_failure(cross_binding, manifest, root, "cross-cell binding")

        invented_summary = copy.deepcopy(result)
        invented_cell = invented_summary["cells"][0]
        invented_cell["measured_trials"][0]["gpu_p95_ns"] += 10_000
        raw_descriptor = next(
            item for item in invented_cell["artifacts"] if item["kind"] == "raw_trials"
        )
        raw_document = json.loads(
            (root / raw_descriptor["path"]).read_text(encoding="utf-8")
        )
        raw_document["cell"] = evidence.cell_payload(invented_cell)
        rewrite_json_artifact(root, invented_cell, "raw_trials", raw_document)
        expect_artifact_failure(
            invented_summary, manifest, root,
            "producer summary not derived from retained frame samples",
            contains="gpu_p95_ns differs from retained frames",
        )
        rewrite_json_artifact(root, cells[0], "raw_trials", {
            "schema": "pulp.gpu-dpr-raw-trials.v2", "version": 2,
            "binding_sha256": evidence.binding_sha256(cells[0]),
            "cell": evidence.cell_payload(cells[0]),
        })

        fake_calibration = copy.deepcopy(result)
        calibration_cell = fake_calibration["cells"][0]
        sequence_descriptor = next(
            item for item in calibration_cell["artifacts"]
            if item["kind"] == "frame_sequences"
        )
        sequence_document = json.loads(
            (root / sequence_descriptor["path"]).read_text(encoding="utf-8")
        )
        sequence_document["gpu_timer_calibration"]["extra_work_samples_ns"] = [10] * 5
        rewrite_json_artifact(root, calibration_cell, "frame_sequences", sequence_document)
        expect_artifact_failure(
            fake_calibration, manifest, root, "GPU known-extra-work control not detected",
            contains="known-extra-work control was not detected",
        )
        # Restore the valid calibration bytes before exercising later controls.
        cell_zero_sequence = retained_documents(root, cells[0])["frame_sequences"]
        cell_zero_sequence["gpu_timer_calibration"]["extra_work_samples_ns"] = [20] * 5
        rewrite_json_artifact(root, cells[0], "frame_sequences", cell_zero_sequence)

        foreign_clock = copy.deepcopy(result)
        calibration_cell = foreign_clock["cells"][0]
        sequence_document = retained_documents(root, calibration_cell)["frame_sequences"]
        sequence_document["gpu_timer_calibration"]["clock"] = "dawn-gpu-timestamp"
        rewrite_json_artifact(root, calibration_cell, "frame_sequences", sequence_document)
        expect_artifact_failure(
            foreign_clock, manifest, root, "foreign GPU timer clock",
            contains="clock does not match scenario provider",
        )
        sequence_document["gpu_timer_calibration"]["clock"] = "webgl2-timer-query"
        rewrite_json_artifact(root, cells[0], "frame_sequences", sequence_document)

        false_input = copy.deepcopy(result)
        input_cell = false_input["cells"][0]
        input_document = retained_documents(root, input_cell)["input_receipt"]
        input_document["event"]["observed_target"] = "invented-target"
        input_document["logical_input_exact"] = True
        rewrite_json_artifact(root, input_cell, "input_receipt", input_document)
        expect_artifact_failure(
            false_input, manifest, root, "self-attested input success",
            contains="not derived from runtime coordinates/event/target",
        )
        valid_input = retained_documents(root, cells[1])["input_receipt"]
        valid_input["binding_sha256"] = evidence.binding_sha256(cells[0])
        valid_input["logical_size"] = cells[0]["logical_size"]
        valid_input["physical_size"] = cells[0]["physical_size"]
        valid_input["event"]["process_id"] = cells[0]["identity"]["process_id"]
        rewrite_json_artifact(root, cells[0], "input_receipt", valid_input)

        near_miss_input = copy.deepcopy(result)
        input_cell = near_miss_input["cells"][0]
        input_document = retained_documents(root, input_cell)["input_receipt"]
        input_document["event"]["observed_physical_point"] = [2.49, 3]
        input_document["event"]["observed_logical_point"] = [2.49, 3]
        rewrite_json_artifact(root, input_cell, "input_receipt", input_document)
        expect_artifact_failure(
            near_miss_input, manifest, root, "near-miss logical input coordinates",
            contains="not derived from runtime coordinates/event/target",
        )
        rewrite_json_artifact(root, cells[0], "input_receipt", valid_input)

        missing_event_input = copy.deepcopy(result)
        input_cell = missing_event_input["cells"][0]
        input_document = retained_documents(root, input_cell)["input_receipt"]
        input_document["event"]["event_received"] = False
        rewrite_json_artifact(root, input_cell, "input_receipt", input_document)
        expect_artifact_failure(
            missing_event_input, manifest, root, "logical input event not received",
            contains="not derived from runtime coordinates/event/target",
        )
        rewrite_json_artifact(root, cells[0], "input_receipt", valid_input)

        daw_scenario = copy.deepcopy(scenario)
        daw_scenario.update({
            "id": "forge-modular-daw", "kind": "external_forge_daw_canary",
            "required_provider": "Dawn/WebGPU", "required_host": "logic-and-reaper-aggregate",
        })
        daw_manifest = copy.deepcopy(manifest)
        daw_manifest["scenarios"] = [daw_scenario]
        daw_cell = cell_skeleton(daw_scenario, daw_manifest, "original", 999)
        daw_cell["daw_subreceipts"] = [{
            "format": "auv2", "host": "logic", "a3_role": "forge-modular-auv2-logic",
            "outcome": "pass", "gates_passed": True,
            "identity_sha256": "0" * 64, "receipt_sha256": "0" * 64,
        }]
        retain_cell(root, daw_cell, daw_scenario, daw_manifest, "daw-cell")
        daw_artifacts = evidence._artifact_map(daw_cell)
        daw_documents = retained_documents(root, daw_cell)
        evidence._validate_json_artifacts(
            daw_cell, daw_documents, daw_artifacts, daw_manifest,
            evidence.binding_sha256(daw_cell),
        )
        evidence._validate_daw_subreceipts(
            daw_cell, daw_documents["identity_receipt"], daw_artifacts,
            daw_manifest, evidence.binding_sha256(daw_cell), root,
        )
        all_failed_cell = copy.deepcopy(daw_cell)
        all_failed_documents = copy.deepcopy(daw_documents)
        all_failed_projection = all_failed_cell["daw_subreceipts"][0]
        all_failed_document = all_failed_documents["identity_receipt"][
            "daw_receipt_documents"
        ][0]
        all_failed_projection["outcome"] = "fail"
        all_failed_projection["gates_passed"] = False
        all_failed_document["outcome"] = "fail"
        all_failed_document["gates_passed"] = False
        all_failed_projection["receipt_sha256"] = evidence.canonical_sha256(
            all_failed_document
        )
        try:
            evidence._validate_daw_subreceipts(
                all_failed_cell, all_failed_documents["identity_receipt"], daw_artifacts,
                daw_manifest, evidence.binding_sha256(all_failed_cell), root,
            )
        except evidence.V2EvidenceError as error:
            if "required DAW subreceipt did not pass" not in str(error):
                raise AssertionError(
                    f"all-failed DAW control failed incorrectly: {error}"
                ) from error
        else:
            raise AssertionError("all-failed DAW subreceipt passed retained validation")
        substituted_a3 = copy.deepcopy(daw_documents)
        substituted_a3_document = {**A3_DOCUMENT, "package": "substituted"}
        a3_reference = substituted_a3["identity_receipt"]["daw_receipt_documents"][0][
            "a3_evidence"
        ]
        a3_path = root / a3_reference["path"]
        write_json(a3_path, substituted_a3_document)
        a3_payload = a3_path.read_bytes()
        a3_reference["sha256"] = hashlib.sha256(a3_payload).hexdigest()
        a3_reference["bytes"] = len(a3_payload)
        try:
            evidence._validate_daw_subreceipts(
                daw_cell, substituted_a3["identity_receipt"], daw_artifacts,
                daw_manifest, evidence.binding_sha256(daw_cell), root,
            )
        except evidence.V2EvidenceError as error:
            if "fixed A3 authority" not in str(error):
                raise AssertionError(f"substituted A3 control failed incorrectly: {error}") from error
        else:
            raise AssertionError("DAW subreceipt with substituted fixed A3 evidence passed")
        write_json(a3_path, A3_DOCUMENT)
        substituted_lifecycle = copy.deepcopy(daw_documents)
        lifecycle_reference = substituted_lifecycle["identity_receipt"][
            "daw_receipt_documents"
        ][0]["lifecycle"]
        lifecycle_path = root / lifecycle_reference["path"]
        lifecycle_document = json.loads(lifecycle_path.read_text(encoding="utf-8"))
        valid_lifecycle = copy.deepcopy(lifecycle_document)
        lifecycle_document["lifecycle_id"] = ""
        write_json(lifecycle_path, lifecycle_document)
        lifecycle_payload = lifecycle_path.read_bytes()
        lifecycle_reference["sha256"] = hashlib.sha256(lifecycle_payload).hexdigest()
        lifecycle_reference["bytes"] = len(lifecycle_payload)
        try:
            evidence._validate_daw_subreceipts(
                daw_cell, substituted_lifecycle["identity_receipt"], daw_artifacts,
                daw_manifest, evidence.binding_sha256(daw_cell), root,
            )
        except evidence.V2EvidenceError as error:
            if "fixed A3/product/host/lifecycle" not in str(error):
                raise AssertionError(
                    f"missing lifecycle control failed incorrectly: {error}"
                ) from error
        else:
            raise AssertionError("DAW subreceipt without retained lifecycle identity passed")
        write_json(lifecycle_path, valid_lifecycle)

        symlink_result = copy.deepcopy(result)
        symlink_artifact = root / symlink_result["cells"][0]["artifacts"][1]["path"]
        symlink_payload = symlink_artifact.read_bytes()
        outside = root.parent / f"{root.name}-outside"
        outside.write_bytes(symlink_payload)
        symlink_artifact.unlink()
        symlink_artifact.symlink_to(outside)
        expect_artifact_failure(symlink_result, manifest, root, "artifact symlink")
        symlink_artifact.unlink()
        symlink_artifact.write_bytes(symlink_payload)
        outside.unlink()

    with tempfile.TemporaryDirectory(prefix="pulp-a4-v2-runner-") as directory:
        workspace = Path(directory).resolve()
        analyzer = workspace / "analyzer.sh"
        analyzer.write_text("#!/bin/sh\nexit 99\n", encoding="utf-8")
        analyzer.chmod(0o755)
        run_dir = workspace / "run"
        canonical = experiment.load_json(experiment.DEFAULT_MANIFEST)
        canonical["v2_protocol"]["status"] = "authorized"
        canonical["v2_protocol"]["product_policy"] = {
            "id": "test", "path": "policy.json", "blob": SHA_A,
            "a3_receipt_sha256": "3" * 64,
        }
        canonical["trial_contract"]["timer_noise_p95_ns"] = 1
        canonical["trial_contract"]["memory_sampler_resolution_bytes"] = 1
        for item in canonical["scenarios"]:
            item["frame_budget_ns"] = 16_666_667
        state = local_runner_state(run_dir, canonical, analyzer)
        key = runner.run_cell_key("original", "super-convolver-web", "exact", 1)
        adapter = workspace / "adapter.sh"
        adapter.write_text(
            f"#!/bin/sh\nexec {sys.executable} {Path(__file__).resolve()} --producer \"$@\"\n",
            encoding="utf-8",
        )
        adapter.chmod(0o755)
        accepted = runner.run_one(run_dir, key, adapter, 30)
        assert accepted["cell_key"] == key
        loaded, _ = runner.load_state(run_dir)
        rederived, _ = runner.rederive_cell(run_dir, loaded, key, canonical)
        assert rederived == accepted["cell"]

        bad_pid_key = runner.run_cell_key(
            "original", "super-convolver-web", "exact", 1.5
        )
        bad_pid_adapter = workspace / "bad-pid-adapter.sh"
        bad_pid_adapter.write_text(
            "#!/bin/sh\n"
            "export PULP_A4_TEST_BAD_PRODUCER_PID=1\n"
            f"exec {sys.executable} {Path(__file__).resolve()} --producer \"$@\"\n",
            encoding="utf-8",
        )
        bad_pid_adapter.chmod(0o755)
        try:
            runner.run_one(run_dir, bad_pid_key, bad_pid_adapter, 30)
        except ValueError:
            pass
        else:
            raise AssertionError("self-attested producer process id passed")
        loaded, _ = runner.load_state(run_dir)
        rejected = loaded["cells"][bad_pid_key]
        assert rejected["issued"] is None
        assert rejected["attempts"][-1]["outcome"] == "inconclusive"
        retried = runner.run_one(run_dir, bad_pid_key, adapter, 30)
        assert retried["cell_key"] == bad_pid_key

        output_key = runner.run_cell_key(
            "original", "super-convolver-web", "exact", 2
        )
        output_adapter = workspace / "unbounded-output-adapter.sh"
        output_adapter.write_text(
            "#!/bin/sh\n"
            f"exec {sys.executable} -c 'import sys; "
            f"sys.stdout.buffer.write(b\"x\" * {runner.OUTPUT_CAP_BYTES + 1})'\n",
            encoding="utf-8",
        )
        output_adapter.chmod(0o755)
        try:
            runner.run_one(run_dir, output_key, output_adapter, 30)
        except ValueError:
            pass
        else:
            raise AssertionError("adapter exceeded the bounded output cap")
        loaded, _ = runner.load_state(run_dir)
        assert loaded["cells"][output_key]["issued"] is None

        # A same-user attacker can replace the local integrity key and reseal
        # projected state.  That is not terminal authority: missing nonce-bound
        # accepted receipts still fail rederivation.
        forged = copy.deepcopy(state)
        forged_cell = next(iter(forged["cells"].values()))
        forged_cell["status"] = "complete"
        forged_cell["accepted_receipt"] = {
            "path": "caller/draft.json", "sha256": "4" * 64,
        }
        key_path = run_dir / evidence.RUN_KEY
        key_path.chmod(0o600)
        key_path.unlink()
        evidence.initialize_run_key(run_dir)
        evidence.write_integrity_state(run_dir, forged)
        forged_loaded, _ = runner.load_state(run_dir)
        forged_key = next(iter(forged_loaded["cells"]))
        try:
            runner.rederive_cell(run_dir, forged_loaded, forged_key, canonical)
        except ValueError:
            pass
        else:
            raise AssertionError("forged state/key replaced nonce-bound runner receipts")

        caller_owned = workspace / "caller-owned"
        caller_owned.mkdir()
        try:
            runner.initialize(caller_owned, "caller-owned", analyzer)
        except ValueError:
            pass
        else:
            raise AssertionError("caller-owned precreated run directory passed")

        draft = subprocess.run(
            [
                sys.executable, str(SCRIPT_DIR / "gpu_dpr_runner.py"),
                "finalize-v2", "--draft", str(workspace / "draft.json"),
            ],
            capture_output=True, text=True,
        )
        if draft.returncode == 0:
            raise AssertionError("caller draft remained a finalize-v2 authority")

        git_object_tests(workspace)
        live_document = {
            "schema": "pulp.gpu-dpr-live-verification.v1",
            "version": 1,
            "repository": "Generous-Corp/pulp",
            "head": SHA_A,
            "manifest": {
                "path": terminal.CANONICAL_MANIFEST.as_posix(), "type": "blob",
                "mode": "100644", "blob": SHA_B, "sha256": "5" * 64,
                "bytes": 10,
            },
            "result": {
                "path": terminal.CANONICAL_RESULT.as_posix(), "type": "blob",
                "mode": "100644", "blob": SHA_C, "sha256": "6" * 64,
                "bytes": 20,
            },
            "required_checks": [{
                "context": "contract", "integration_id": 1, "source_id": 2,
                "completed_at": "2026-08-29T00:00:00+00:00",
            }],
            "verified_at": "2026-08-29T00:00:01+00:00",
        }
        with mock.patch.object(
            experiment, "live_verification_document",
            return_value=(copy.deepcopy(live_document), []),
        ):
            experiment.emit_live_verification(run_dir)
            assert not experiment.live_receipt_errors(run_dir)
            receipt_path = run_dir / experiment.LIVE_RECEIPT_NAME
            receipt_path.chmod(0o600)
            changed = json.loads(receipt_path.read_text(encoding="utf-8"))
            changed["head"] = SHA_D
            write_json(receipt_path, changed)
            if not experiment.live_receipt_errors(run_dir):
                raise AssertionError("substituted durable live receipt passed")

    print(
        "gpu_dpr_v2_evidence_selftest=true corpus=84+84 artifacts=8 "
        "no_file=pass digest=pass content=pass path=pass symlink=pass outside=pass "
        "cross_cell=pass runner_process=pass producer_pid=pass retry=pass "
        "output_bound=pass forged_state_key=pass "
        "frame_rederive=pass gpu_calibration=pass runtime_input=pass "
        "fixed_a3_daw_identity=pass caller_draft=pass "
        "git_blob_type_head=pass live_receipt=pass"
    )
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--producer":
        raise SystemExit(producer_main(sys.argv[2:]))
    raise SystemExit(main())
