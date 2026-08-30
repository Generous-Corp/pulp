#!/usr/bin/env python3
"""Deterministic tests and planted bad evidence for the A4 DPR runner."""

from __future__ import annotations

import hashlib
import json
import shutil
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path
from unittest import mock

import gpu_first_visible_a3_acceptance as a3_acceptance
import gpu_dpr_experiment as experiment
import gpu_dpr_evidence as evidence
import gpu_dpr_runner as runner
import test_gpu_first_visible_a3_acceptance as a3_fixture
from gpu_dpr_test_support import (
    exact_binary, forged_minimal_dependencies, malformed_adapter_script,
    no_receipt_adapter_script, test_adapter_script, noisy_adapter_script,
    timeout_adapter_script, trace_analyzer_script,
    structural_dependency_receipts, wrong_nonce_adapter_script,
)

SHA_A = "3" * 40
SHA_C = "2" * 40
PNG_CACHE: dict[tuple[int, int, bool], bytes] = {}


def plan(manifest: dict) -> dict:
    class Args:
        experiment_id = "a4-runner-selftest"
        plan_revision = SHA_A
        # The A3 fixture binds its causal campaign and source blobs to the
        # implementation under test. Keep A4's synthetic plan on that same
        # authority instead of retaining a legacy all-ones placeholder.
        pulp_sha = a3_fixture.PULP_REVISION
        forge_sha = SHA_C

    return experiment.planned_result(Args(), manifest)


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def fixture_png(width: int, height: int, *, flat: bool = False) -> bytes:
    """Make a valid, highly compressible RGBA fixture at the claimed dimensions."""
    key = (width, height, flat)
    if key in PNG_CACHE:
        return PNG_CACHE[key]
    palette = (
        (18, 24, 34, 255), (235, 239, 246, 255), (47, 111, 237, 255),
        (241, 160, 45, 255), (72, 189, 121, 255), (201, 70, 92, 255),
        (134, 94, 214, 255), (55, 181, 190, 255),
        (252, 205, 71, 255), (84, 94, 112, 255), (111, 207, 151, 255),
        (226, 106, 156, 255), (102, 146, 245, 255), (238, 127, 78, 255),
        (168, 211, 68, 255), (179, 130, 226, 255),
    )
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        if flat:
            rows.extend(bytes(palette[0]) * width)
        else:
            rows.extend(b"".join(
                bytes(palette[((x // 4) + (y // 4)) % len(palette)])
                for x in range(width)
            ))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)
        )

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b"")
    )
    PNG_CACHE[key] = png
    return png


def raw_samples(
    *,
    logical_input_ok: bool = True,
    fidelity_ok: bool = True,
    capture_similarity: float = 1.0,
) -> dict:
    expected = [320, 180]
    observed_physical = expected if logical_input_ok else [160, 90]
    semantics = {
        "cpu_frame_time": ("measured", "steady-frame CPU submit latency"),
        "gpu_frame_time": ("measured", "GPU timestamp elapsed time"),
        "first_frame_time": ("measured", "fresh-process first output latency"),
        "interaction_latency": ("measured", "input dispatch and render completion latency"),
        "render_target_bytes": ("derived", "RGBA8 physical dimensions times four"),
        "resident_bytes": ("derived", "render target plus observed resident allocations"),
        "upload_bytes": ("measured", "instrumented CPU-to-GPU upload bytes"),
    }
    samples = {
        "cpu_frame_time": [2.0 + index / 100 for index in range(30)],
        "gpu_frame_time": [1.0 + index / 100 for index in range(30)],
        "first_frame_time": [10.0 + index / 10 for index in range(20)],
        "interaction_latency": [4.0 + index / 100 for index in range(30)],
        "render_target_bytes": [1024 + index for index in range(30)],
        "resident_bytes": [4096 + index for index in range(30)],
        "upload_bytes": [512 + index for index in range(30)],
    }
    return {
        "schema": runner.RAW_SCHEMA,
        "version": 1,
        "metrics": {
            name: {"provenance": semantics[name][0], "definition": semantics[name][1],
                   "samples": values}
            for name, values in samples.items()
        },
        "gpu_timer_calibration": {
            "schema": "pulp.gpu-dpr-timer-calibration.v1", "version": 1,
            "clock": "dawn-gpu-timestamp", "resolution_ms": 0.01,
            "baseline_samples_ms": [1.0, 1.01, 1.02, 1.03, 1.04],
            "extra_work_samples_ms": [4.0, 4.01, 4.02, 4.03, 4.04],
            "extra_work_multiplier": 8, "control_detected": True,
        },
        "fidelity": {
            "content_floor_passed": fidelity_ok,
            "capture_similarity": capture_similarity,
            "small_text_luminance_stddev": 2.0 if fidelity_ok else 0.0,
            "thin_stroke_coverage": 0.1 if fidelity_ok else 0.0,
        },
        "logical_input_trials": [{
            "expected_logical": expected,
            "observed_logical": observed_physical,
            "requested_physical": expected,
            "observed_physical": observed_physical,
            "expected_target": "root-hit",
            "observed_target": "root-hit",
            "event_received": True,
        }],
        "trace": {
            "complete": True,
        },
    }


def make_receipt(
    run_dir: Path,
    state: dict,
    manifest: dict,
    key: str,
    *,
    analyzer: Path,
    binary: Path,
    outcome: str = "pass",
    logical_input_ok: bool = True,
    fidelity_ok: bool = True,
    capture_similarity: float | None = None,
    attempt_nonce: str | None = None,
    measurement_producer: Path | None = None,
) -> Path:
    cell = state["cells"][key]
    scenario = runner.scenario_map(manifest)[cell["scenario_id"]]
    cell_dir = runner.cell_directory(run_dir, key)
    cell_dir.mkdir(parents=True, exist_ok=True)
    nonce = attempt_nonce or runner.secrets.token_hex(runner.NONCE_HEX_LENGTH // 2)
    if outcome in runner.INCOMPLETE_OUTCOMES:
        receipt = {
            "schema": runner.RECEIPT_SCHEMA,
            "version": 1,
            "attempt_nonce": nonce,
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

    raw = raw_samples(
        logical_input_ok=logical_input_ok,
        fidelity_ok=fidelity_ok,
        capture_similarity=capture_similarity if capture_similarity is not None else 1.0,
    )
    requested = float(cell["requested_dpr"])
    observed = (
        min(requested, float(manifest["configured_max_dpr"]))
        if cell["mode"] == "configured_max" else requested
    )
    oracle = scenario["logical_input_oracle"]
    if "fidelity_oracle" in scenario:
        raw["fidelity"]["oracle_regions"] = scenario["fidelity_oracle"]
    logical_trial = raw["logical_input_trials"][0]
    expected_point = oracle["point"]
    logical_trial["expected_logical"] = expected_point
    logical_trial["requested_physical"] = [value * observed for value in expected_point]
    logical_trial["expected_target"] = oracle["target"]
    logical_trial["observed_target"] = oracle["target"]
    if logical_input_ok:
        logical_trial["observed_physical"] = logical_trial["requested_physical"]
        logical_trial["observed_logical"] = expected_point
    is_web = scenario["kind"] == "maintained_web_canary"
    if is_web:
        raw["trace"] = {
            "complete": True, "kind": "browser-devtools", "process_pid": 77,
        }
    if cell["mode"] == "adaptive_simulation":
        profile = manifest["adaptive_profile"]
        down_count = profile["downshift_after_over_budget_frames"]
        up_count = profile["upshift_after_under_budget_frames"]
        ladder = [float(value) for value in profile["scale_ladder"]]
        initial_scale = float(cell["requested_dpr"])
        initial_index = ladder.index(initial_scale)
        down_scale = ladder[max(0, initial_index - 1)]
        up_scale = ladder[min(len(ladder) - 1, max(0, initial_index - 1) + 1)]
        observations = []
        scale = initial_scale
        for index in range(down_count):
            after = down_scale if index == down_count - 1 else scale
            observations.append({
                "phase": "over-budget", "frame_index": index, "sample_ms": 12.0,
                "scale_before": scale, "scale_after": after,
                "transition": (
                    "downshift" if down_scale < initial_scale else "floor-hold"
                ) if index == down_count - 1 else False,
            })
            scale = after
        for index in range(up_count):
            after = up_scale if index == up_count - 1 else scale
            observations.append({
                "phase": "under-budget", "frame_index": down_count + index,
                "sample_ms": 4.0, "scale_before": scale, "scale_after": after,
                "transition": (
                    "upshift" if up_scale > scale else "ceiling-hold"
                ) if index == up_count - 1 else False,
            })
            scale = after
        raw["adaptive_policy_evidence"] = {
            "profile": profile,
            "budget_ms": 10.0,
            "initial_scale": initial_scale,
            "final_scale": up_scale,
            "observations": observations,
        }
    browser_trace = json.dumps({"traceEvents": [{
        "name": f"pulp.dpr.{nonce}.{category}", "ph": "X", "pid": 77, "dur": 1,
    } for category in manifest["trial_contract"]["required_trace_categories"]]}).encode()
    logical = scenario["logical_size"]
    physical_width = round(logical["width"] * observed)
    physical_height = round(logical["height"] * observed)
    reference_png = fixture_png(
        physical_width, physical_height, flat=not fidelity_ok,
    )
    capture_png = fixture_png(
        physical_width, physical_height, flat=not fidelity_ok,
    )
    files = {
        "capture": ("capture.png", capture_png),
        "reference_capture": ("reference.png", reference_png),
        "trace": ("trace.pftrace", browser_trace if is_web else b"real-trace-bytes:" + nonce.encode("ascii")),
        "raw_samples": ("raw-samples.json", None),
        "input_receipt": ("input-receipt.json", b'{"logical":true}\n'),
    }
    capture_digest = hashlib.sha256(files["capture"][1]).hexdigest()
    reference_digest = hashlib.sha256(files["reference_capture"][1]).hexdigest()
    raw["fidelity"]["comparison"] = {
        "method": "pulp-png-pixel-comparison",
        "reference_sha256": reference_digest,
        "capture_sha256": capture_digest,
        "same_content_token": nonce,
    }
    write_json(cell_dir / "raw-samples.json", raw)
    artifacts = []
    for kind, (name, content) in files.items():
        path = cell_dir / name
        if content is not None:
            path.write_bytes(content)
        artifacts.append({"kind": kind, "path": name, "sha256": runner.sha256_file(path)})

    (_, _, measured_floor, measured_similarity,
     measured_text, measured_strokes) = evidence.recompute_fidelity(
        cell_dir / "capture.png", cell_dir / "reference.png", scenario, observed,
    )
    raw["fidelity"].update({
        "content_floor_passed": measured_floor,
        "capture_similarity": (
            measured_similarity if capture_similarity is None else capture_similarity
        ),
        "small_text_luminance_stddev": measured_text,
        "thin_stroke_coverage": measured_strokes,
    })
    write_json(cell_dir / "raw-samples.json", raw)
    next(item for item in artifacts if item["kind"] == "raw_samples")["sha256"] = (
        runner.sha256_file(cell_dir / "raw-samples.json")
    )

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
        "attempt_nonce": nonce,
        "attempt_number": 1,
        "reason": None,
        "dependencies": [],
        "machine": {"id": "selftest-m3", "os": "macos", "architecture": "arm64"},
        "adapter": adapter,
        "build_identity": {
            "pulp_sha": state["plan"]["pulp_sha"],
            "forge_sha": state["plan"]["forge_sha"],
            "binary": {
                "path": str(binary.resolve()),
                "sha256": runner.sha256_file(binary),
            },
        },
        "plugin_format": "vst3",
        "format_qualified": True,
        "scan_cache_confirmed": True,
        "audio_thread_excluded": True,
        "artifacts": artifacts,
    }
    if measurement_producer is not None:
        producer_digest = runner.sha256_file(measurement_producer)
        receipt["build_identity"]["measurement_producer"] = {
            "path": str(measurement_producer.resolve()),
            "sha256": producer_digest,
        }
        receipt["measurement_attestation"] = {
            "schema": "pulp.gpu-dpr-native-measurement-attestation.v1",
            "producer_sha256": producer_digest,
            "same_process": {
                field: True for field in {
                    "adapter_identity", "capture", "frame_metrics",
                    "memory_metrics", "logical_input", "trace_correlation",
                }
            },
            "audio_device_opened": False,
        }
    elif is_web:
        browser_digest = runner.sha256_file(binary)
        script_digest = runner.sha256_file(analyzer)
        browser_product = cell_dir / "browser-product"
        shutil.copy2(binary, browser_product)
        browser_product.chmod(0o555)
        web_names = (
            "PulpSuperConvolverUi.data", "PulpSuperConvolverUi.js",
            "PulpSuperConvolverUi.wasm",
        )
        web_artifacts = {
            name: {"path": str(binary.resolve()), "sha256": browser_digest}
            for name in web_names
        }
        web_binding = "".join(
            f"{name}:{browser_digest}\n" for name in web_names
        )
        web_digest = hashlib.sha256(web_binding.encode()).hexdigest()
        receipt["build_identity"].update({
            "measurement_producer": {
                "path": str(binary.resolve()), "sha256": browser_digest,
            },
            "browser_product_executable": {
                "path": str(browser_product.resolve()), "sha256": browser_digest,
            },
            "measurement_script": {
                "path": str(analyzer.resolve()), "sha256": script_digest,
            },
            "browser_product": {
                "version": "Google Chrome selftest",
                "codesign_identifier": "com.google.Chrome",
                "team_identifier": "EQHXZ8M8AV",
            },
            "web_ui_artifacts": web_artifacts,
            "web_ui_bundle_sha256": web_digest,
        })
        receipt["measurement_attestation"] = {
            "schema": "pulp.gpu-dpr-browser-measurement-attestation.v1",
            "producer_sha256": browser_digest,
            "script_sha256": script_digest,
            "build_sha256": web_digest,
            "same_process": {
                field: True for field in {
                    "adapter_identity", "capture", "frame_metrics",
                    "memory_metrics", "logical_input", "trace_correlation",
                }
            },
            "audio_device_opened": False,
        }
    producer_digest = runner.sha256_file(measurement_producer or binary)
    raw["producer_pid"] = 9999
    raw["fresh_process_trials"] = [{
        "schema": "pulp.gpu-dpr-first-frame-trial.v1",
        "version": 1,
        "attempt_nonce": nonce,
        "attempt_number": 1,
        "pid": 10000 + index,
        "producer_sha256": producer_digest,
        "content_digest": receipt["content_digest"],
        "pulp_sha": state["plan"]["pulp_sha"],
        "first_frame_time_ms": raw["metrics"]["first_frame_time"]["samples"][index],
        "adapter": adapter,
        **({"build_sha256": receipt["build_identity"]["web_ui_bundle_sha256"]}
           if is_web else {}),
    } for index in range(20)]
    write_json(cell_dir / "raw-samples.json", raw)
    for artifact in artifacts:
        if artifact["kind"] == "raw_samples":
            artifact["sha256"] = runner.sha256_file(cell_dir / artifact["path"])
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


def ingest_receipt(run_dir: Path, receipt: Path) -> str:
    nonce = runner.regular_json(receipt, "selftest receipt")["attempt_nonce"]
    return runner.ingest_receipt(run_dir, receipt, nonce)


def main() -> int:
    # The matrix projection uses generated executable fixtures, not an installed
    # browser. Product-signature rejection has its own focused adapter test.
    evidence.validate_browser_product = lambda _path, _product: None
    with tempfile.TemporaryDirectory(prefix="pulp-dpr-runner-") as temporary:
        root = Path(temporary)
        manifest_path = root / "manifest.json"
        manifest = runner.load_json(experiment.DEFAULT_MANIFEST)
        write_json(manifest_path, manifest)
        for scenario in manifest["scenarios"]:
            if scenario.get("source_sha256"):
                shutil.copy2(
                    experiment.DEFAULT_MANIFEST.parent / scenario["source"],
                    root / scenario["source"],
                )
            elif scenario["kind"].startswith("maintained_"):
                maintained = root / scenario["source"]
                maintained.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(experiment.ROOT / scenario["source"], maintained)
        analyzer = trace_analyzer_script(root)
        pinned_analyzer = root / "runner-owned" / "trace-analyzer"
        analyzer_digest = runner.snapshot_file(
            analyzer, pinned_analyzer, "selftest analyzer", executable=True
        )
        analyzer_identity = {
            "path": str(pinned_analyzer.resolve()), "sha256": analyzer_digest
        }
        binary = exact_binary(root)
        run_dir = root / "run"
        planned = plan(manifest)
        unratified_manifest = json.loads(json.dumps(manifest))
        del unratified_manifest["trial_contract"]["capture_similarity_minimum"]
        try:
            runner.initial_state(
                planned, unratified_manifest, manifest_path, analyzer_identity
            )
        except runner.EvidenceError:
            pass
        else:
            raise AssertionError("manifest without a similarity threshold was accepted")
        state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(run_dir, state)
        assert len(state["cells"]) == 84
        assert runner.status_document(state)["incomplete_cells"] == 84

        request_run = root / "adaptive-request-run"
        request_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(request_run, request_state)
        adaptive_request_key = runner.cell_key(
            "dense-text-thin-strokes", "adaptive_simulation", 1
        )
        _, adaptive_request_path = runner.issue_attempt(
            request_run, request_state, manifest, adaptive_request_key
        )
        adaptive_request = runner.load_json(adaptive_request_path)
        assert adaptive_request["adaptive_profile"] == manifest["adaptive_profile"]
        assert adaptive_request["pulp_source_root"] == str(experiment.ROOT.resolve())

        # Only the newest issued generation for a cell remains authorized.
        # Request bytes and paths remain immutable so an overlapping adapter
        # cannot read a later attempt's nonce through a fixed filename.
        retry_run = root / "retry-generation-run"
        retry_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(retry_run, retry_state)
        nonce_a, request_a = runner.issue_attempt(
            retry_run, retry_state, manifest, adaptive_request_key
        )
        request_a_bytes = request_a.read_bytes()
        retry_state = runner.load_state(retry_run)
        nonce_b, request_b = runner.issue_attempt(
            retry_run, retry_state, manifest, adaptive_request_key
        )
        assert request_a != request_b
        assert request_a.read_bytes() == request_a_bytes
        assert runner.load_json(request_a)["attempt_nonce"] == nonce_a
        assert runner.load_json(request_b)["attempt_nonce"] == nonce_b
        retry_state = runner.load_state(retry_run)
        assert retry_state["issued_attempts"] == {nonce_b: adaptive_request_key}
        receipt_b = make_receipt(
            retry_run, retry_state, manifest, adaptive_request_key,
            analyzer=analyzer, binary=binary, outcome="skip", attempt_nonce=nonce_b,
        )
        ingest_receipt(retry_run, receipt_b)
        accepted_state = runner.load_state(retry_run)
        late_a = runner.cell_directory(retry_run, adaptive_request_key) / "late-a.json"
        late_document = runner.load_json(
            Path(accepted_state["cells"][adaptive_request_key]["attempts"][-1]["receipt"])
        )
        late_document["attempt_nonce"] = nonce_a
        write_json(late_a, late_document)
        try:
            runner.ingest_receipt(retry_run, late_a, nonce_a)
        except runner.EvidenceError:
            pass
        else:
            raise AssertionError("superseded retry nonce was accepted after the current attempt")
        assert runner.load_state(retry_run) == accepted_state

        initialized_run = root / "initialized-run"
        initialized_run.mkdir()
        initialized = runner.initialize_run(
            initialized_run, planned, manifest, manifest_path, analyzer
        )
        assert Path(initialized["trace_analyzer"]["path"]).suffix == ".py"
        initialized_digest = initialized["trace_analyzer"]["sha256"]
        analyzer_source_bytes = analyzer.read_bytes()
        analyzer.write_bytes(analyzer_source_bytes + b"# source mutation\n")
        assert runner.sha256_file(Path(initialized["trace_analyzer"]["path"])) == (
            initialized_digest
        )
        analyzer.write_bytes(analyzer_source_bytes)
        analyzer.chmod(0o755)

        dense = runner.cell_key("dense-text-thin-strokes", "exact", 1)
        good = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary
        )
        key, observation, dependencies = runner.receipt_observation(
            good, state, manifest, Path(state["manifest_path"]), run_dir
        )
        assert key == dense and observation is not None and not dependencies
        assert observation["metrics"]["first_frame_time"]["sample_count"] == 20

        zero_gpu = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary
        )
        zero_gpu_raw_path = runner.cell_directory(run_dir, dense) / "raw-samples.json"
        zero_gpu_raw = runner.load_json(zero_gpu_raw_path)
        zero_gpu_raw["metrics"]["gpu_frame_time"]["samples"][7] = 0.0
        write_json(zero_gpu_raw_path, zero_gpu_raw)
        zero_gpu_document = runner.load_json(zero_gpu)
        next(
            item for item in zero_gpu_document["artifacts"]
            if item["kind"] == "raw_samples"
        )["sha256"] = runner.sha256_file(zero_gpu_raw_path)
        write_json(zero_gpu, zero_gpu_document)
        expect_rejected(zero_gpu, state, manifest, run_dir, "missing GPU sample")

        planted = 0
        def reject_raw_mutation(label: str, mutate) -> None:
            receipt_path = make_receipt(
                run_dir, state, manifest, dense, analyzer=analyzer, binary=binary
            )
            raw_path = runner.cell_directory(run_dir, dense) / "raw-samples.json"
            raw_document = runner.load_json(raw_path)
            mutate(raw_document)
            write_json(raw_path, raw_document)
            receipt_document = runner.load_json(receipt_path)
            next(
                item for item in receipt_document["artifacts"]
                if item["kind"] == "raw_samples"
            )["sha256"] = runner.sha256_file(raw_path)
            write_json(receipt_path, receipt_document)
            expect_rejected(receipt_path, state, manifest, run_dir, label)

        bad_digest = runner.load_json(good)
        bad_digest["artifacts"][0]["sha256"] = "0" * 64
        write_json(good, bad_digest)
        expect_rejected(good, state, manifest, run_dir, "artifact digest mismatch")
        planted += 1

        good = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary,
            logical_input_ok=False,
        )
        expect_rejected(good, state, manifest, run_dir, "logical coordinate scaling")
        planted += 1

        reject_raw_mutation(
            "producer-authored logical expectation",
            lambda raw: raw["logical_input_trials"][0].update(
                expected_logical=[319, 180]
            ),
        )
        planted += 1
        reject_raw_mutation(
            "logical target substitution",
            lambda raw: raw["logical_input_trials"][0].update(
                observed_target="different-target"
            ),
        )
        planted += 1
        reject_raw_mutation(
            "metric provenance removal",
            lambda raw: raw["metrics"]["gpu_frame_time"].pop("provenance"),
        )
        planted += 1
        reject_raw_mutation(
            "undetectable GPU timer control",
            lambda raw: raw["gpu_timer_calibration"].update(
                extra_work_samples_ms=raw["gpu_timer_calibration"]["baseline_samples_ms"]
            ),
        )
        planted += 1
        reject_raw_mutation(
            "unbound fidelity reference",
            lambda raw: raw["fidelity"]["comparison"].update(
                reference_sha256="0" * 64
            ),
        )
        planted += 1
        reject_raw_mutation(
            "fidelity oracle region drift",
            lambda raw: raw["fidelity"]["oracle_regions"]["small_text_roi"].update(
                x=25
            ),
        )
        planted += 1
        reject_raw_mutation(
            "producer-authored fidelity metric forgery",
            lambda raw: raw["fidelity"].update(
                small_text_luminance_stddev=(
                    raw["fidelity"]["small_text_luminance_stddev"] + 1.0
                )
            ),
        )
        planted += 1

        mutated_capture_receipt = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary,
        )
        mutated_capture_path = runner.cell_directory(run_dir, dense) / "capture.png"
        mutated_capture_path.write_bytes(fixture_png(900, 600, flat=True))
        mutated_document = runner.load_json(mutated_capture_receipt)
        mutated_digest = runner.sha256_file(mutated_capture_path)
        next(
            item for item in mutated_document["artifacts"]
            if item["kind"] == "capture"
        )["sha256"] = mutated_digest
        mutated_raw_path = runner.cell_directory(run_dir, dense) / "raw-samples.json"
        mutated_raw = runner.load_json(mutated_raw_path)
        mutated_raw["fidelity"]["comparison"]["capture_sha256"] = mutated_digest
        write_json(mutated_raw_path, mutated_raw)
        next(
            item for item in mutated_document["artifacts"]
            if item["kind"] == "raw_samples"
        )["sha256"] = runner.sha256_file(mutated_raw_path)
        write_json(mutated_capture_receipt, mutated_document)
        expect_rejected(
            mutated_capture_receipt, state, manifest, run_dir,
            "committed capture mutation with refreshed digests",
        )
        planted += 1

        good = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary
        )
        changed_content = runner.load_json(good)
        changed_content["content_digest"] = "0" * 64
        write_json(good, changed_content)
        expect_rejected(good, state, manifest, run_dir, "content digest drift")
        planted += 1

        maintained_key = runner.cell_key("threejs-audio-reactive", "exact", 1)
        maintained_receipt = make_receipt(
            run_dir, state, manifest, maintained_key,
            analyzer=analyzer, binary=binary,
        )
        maintained_source = root / runner.scenario_map(manifest)[
            "threejs-audio-reactive"
        ]["source"]
        maintained_bytes = maintained_source.read_bytes()
        maintained_source.write_bytes(maintained_bytes + b"\n// planted drift\n")
        expect_rejected(
            maintained_receipt, state, manifest, run_dir,
            "maintained source byte drift",
        )
        maintained_source.write_bytes(maintained_bytes)
        planted += 1

        gpu_key = runner.cell_key("shader-heavy-controls", "exact", 1)
        gpu_receipt = make_receipt(
            run_dir, state, manifest, gpu_key, analyzer=analyzer, binary=binary
        )
        software = runner.load_json(gpu_receipt)
        software["adapter"]["class"] = "software"
        write_json(gpu_receipt, software)
        expect_rejected(gpu_receipt, state, manifest, run_dir, "software GPU substitution")
        planted += 1

        gpu_receipt = make_receipt(
            run_dir, state, manifest, gpu_key, analyzer=analyzer, binary=binary
        )
        raw_path = runner.cell_directory(run_dir, gpu_key) / "raw-samples.json"
        raw = runner.load_json(raw_path)
        raw["trace"]["categories"] = ["render", "gpu", "text", "js", "layout"]
        write_json(raw_path, raw)
        receipt = runner.load_json(gpu_receipt)
        next(item for item in receipt["artifacts"] if item["kind"] == "raw_samples")["sha256"] = runner.sha256_file(raw_path)
        write_json(gpu_receipt, receipt)
        expect_rejected(
            gpu_receipt, state, manifest, run_dir,
            "adapter-authored trace categories",
        )
        planted += 1

        similarity_receipt = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary,
            capture_similarity=0.98,
        )
        expect_rejected(
            similarity_receipt, state, manifest, run_dir,
            "capture similarity below ratified threshold",
        )
        planted += 1

        for label, mutate in [
            ("fresh-process mixed nonce",
             lambda trial: trial.__setitem__("attempt_nonce", "0" * 32)),
            ("fresh-process mixed attempt",
             lambda trial: trial.__setitem__("attempt_number", 2)),
            ("fresh-process producer digest drift",
             lambda trial: trial.__setitem__("producer_sha256", "0" * 64)),
            ("fresh-process mixed adapter",
             lambda trial: trial["adapter"].__setitem__("name", "Other GPU")),
        ]:
            ledger_receipt = make_receipt(
                run_dir, state, manifest, dense, analyzer=analyzer, binary=binary
            )
            ledger_path = runner.cell_directory(run_dir, dense) / "raw-samples.json"
            ledger_raw = runner.load_json(ledger_path)
            mutate(ledger_raw["fresh_process_trials"][1])
            write_json(ledger_path, ledger_raw)
            ledger_document = runner.load_json(ledger_receipt)
            next(
                item for item in ledger_document["artifacts"]
                if item["kind"] == "raw_samples"
            )["sha256"] = runner.sha256_file(ledger_path)
            write_json(ledger_receipt, ledger_document)
            expect_rejected(ledger_receipt, state, manifest, run_dir, label)
            planted += 1

        reused_pid_receipt = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary
        )
        reused_pid_path = runner.cell_directory(run_dir, dense) / "raw-samples.json"
        reused_pid_raw = runner.load_json(reused_pid_path)
        reused_pid_raw["fresh_process_trials"][1]["pid"] = (
            reused_pid_raw["fresh_process_trials"][0]["pid"]
        )
        write_json(reused_pid_path, reused_pid_raw)
        reused_pid_document = runner.load_json(reused_pid_receipt)
        next(
            item for item in reused_pid_document["artifacts"]
            if item["kind"] == "raw_samples"
        )["sha256"] = runner.sha256_file(reused_pid_path)
        write_json(reused_pid_receipt, reused_pid_document)
        expect_rejected(
            reused_pid_receipt, state, manifest, run_dir,
            "fresh-process reused pid",
        )
        planted += 1

        adaptive_key = runner.cell_key(
            "dense-text-thin-strokes", "adaptive_simulation", 1
        )
        adaptive_receipt = make_receipt(
            run_dir, state, manifest, adaptive_key,
            analyzer=analyzer, binary=binary,
        )
        adaptive_raw_path = runner.cell_directory(run_dir, adaptive_key) / "raw-samples.json"
        adaptive_raw = runner.load_json(adaptive_raw_path)
        del adaptive_raw["adaptive_policy_evidence"]
        write_json(adaptive_raw_path, adaptive_raw)
        adaptive_document = runner.load_json(adaptive_receipt)
        next(
            item for item in adaptive_document["artifacts"]
            if item["kind"] == "raw_samples"
        )["sha256"] = runner.sha256_file(adaptive_raw_path)
        write_json(adaptive_receipt, adaptive_document)
        expect_rejected(
            adaptive_receipt, state, manifest, run_dir,
            "adaptive cell without transition evidence",
        )
        planted += 1

        adaptive_receipt = make_receipt(
            run_dir, state, manifest, adaptive_key,
            analyzer=analyzer, binary=binary,
        )
        adaptive_raw = runner.load_json(adaptive_raw_path)
        adaptive_raw["adaptive_policy_evidence"]["observations"][-2]["transition"] = "upshift"
        write_json(adaptive_raw_path, adaptive_raw)
        adaptive_document = runner.load_json(adaptive_receipt)
        next(
            item for item in adaptive_document["artifacts"]
            if item["kind"] == "raw_samples"
        )["sha256"] = runner.sha256_file(adaptive_raw_path)
        write_json(adaptive_receipt, adaptive_document)
        expect_rejected(
            adaptive_receipt, state, manifest, run_dir,
            "adaptive hysteresis boundary drift",
        )
        planted += 1

        trace_receipt = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary
        )
        trace_path = runner.cell_directory(run_dir, dense) / "trace.pftrace"
        trace_path.write_bytes(b"dummy-self-attested-trace")
        trace_document = runner.load_json(trace_receipt)
        next(
            item for item in trace_document["artifacts"] if item["kind"] == "trace"
        )["sha256"] = runner.sha256_file(trace_path)
        write_json(trace_receipt, trace_document)
        expect_rejected(
            trace_receipt, state, manifest, run_dir,
            "self-attested trace outcomes over dummy bytes",
        )
        planted += 1

        wrong_cohort_receipt = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary
        )
        wrong_cohort_trace = runner.cell_directory(run_dir, dense) / "trace.pftrace"
        wrong_cohort_trace.write_bytes(b"real-trace-bytes:" + b"f" * 32)
        wrong_cohort_document = runner.load_json(wrong_cohort_receipt)
        next(
            item for item in wrong_cohort_document["artifacts"]
            if item["kind"] == "trace"
        )["sha256"] = runner.sha256_file(wrong_cohort_trace)
        write_json(wrong_cohort_receipt, wrong_cohort_document)
        expect_rejected(
            wrong_cohort_receipt, state, manifest, run_dir,
            "trace cohort not bound to the issued cell attempt nonce",
        )
        planted += 1

        bad_scope_analyzer = root / "bad-category-scope-analyzer"
        bad_scope_analyzer.write_text(
            analyzer.read_text(encoding="utf-8").replace(
                "'evidence_id':evidence[0]", "'evidence_id':'f'*32"
            ),
            encoding="utf-8",
        )
        bad_scope_analyzer.chmod(0o755)
        bad_scope_state = runner.initial_state(
            planned, manifest, manifest_path,
            {"path": str(bad_scope_analyzer.resolve()),
             "sha256": runner.sha256_file(bad_scope_analyzer)},
        )
        bad_scope_receipt = make_receipt(
            run_dir, bad_scope_state, manifest, dense,
            analyzer=bad_scope_analyzer, binary=binary,
        )
        expect_rejected(
            bad_scope_receipt, bad_scope_state, manifest, run_dir,
            "required categories from a different evidence/process scope",
        )
        planted += 1

        missing_categories_analyzer = root / "missing-correlated-categories-analyzer"
        missing_categories_analyzer.write_text(
            analyzer.read_text(encoding="utf-8").replace(
                "['gpu','js','layout','render','text']", "['gpu']"
            ),
            encoding="utf-8",
        )
        missing_categories_analyzer.chmod(0o755)
        missing_categories_state = runner.initial_state(
            planned, manifest, manifest_path,
            {"path": str(missing_categories_analyzer.resolve()),
             "sha256": runner.sha256_file(missing_categories_analyzer)},
        )
        missing_categories_receipt = make_receipt(
            run_dir, missing_categories_state, manifest, dense,
            analyzer=missing_categories_analyzer, binary=binary,
        )
        expect_rejected(
            missing_categories_receipt, missing_categories_state, manifest, run_dir,
            "required categories missing from the correlated evidence/process scope",
        )
        planted += 1

        analyzer_receipt = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary
        )
        analyzer_raw_path = runner.cell_directory(run_dir, dense) / "raw-samples.json"
        analyzer_raw = runner.load_json(analyzer_raw_path)
        analyzer_raw["trace"]["analyzer"] = {
            "path": str(analyzer), "sha256": "0" * 64
        }
        write_json(analyzer_raw_path, analyzer_raw)
        analyzer_document = runner.load_json(analyzer_receipt)
        next(
            item for item in analyzer_document["artifacts"]
            if item["kind"] == "raw_samples"
        )["sha256"] = runner.sha256_file(analyzer_raw_path)
        write_json(analyzer_receipt, analyzer_document)
        expect_rejected(
            analyzer_receipt, state, manifest, run_dir,
            "adapter-authored trace analyzer substitution",
        )
        planted += 1

        symlink_receipt = make_receipt(
            run_dir, state, manifest, dense, analyzer=analyzer, binary=binary
        )
        symlink_capture = runner.cell_directory(run_dir, dense) / "capture.png"
        symlink_capture.unlink()
        outside_capture = root / "outside-capture.png"
        outside_capture.write_bytes(b"real-capture-bytes")
        symlink_capture.symlink_to(outside_capture)
        expect_rejected(
            symlink_receipt, state, manifest, run_dir,
            "artifact symlink escape",
        )
        symlink_capture.unlink()
        planted += 1

        escaped_run = root / "escaped-run"
        escaped_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(escaped_run, escaped_state)
        escaped_cells = escaped_run / "cells"
        escaped_cells.mkdir()
        outside_cell = root / "outside-cell"
        outside_cell.mkdir()
        (escaped_cells / dense).symlink_to(outside_cell, target_is_directory=True)
        try:
            runner.checked_cell_directory(escaped_run, dense)
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("cell-directory symlink escape was accepted")

        frozen_escape_run = root / "frozen-escape-run"
        frozen_escape_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(frozen_escape_run, frozen_escape_state)
        frozen_nonce, _ = runner.issue_attempt(
            frozen_escape_run, frozen_escape_state, manifest, dense
        )
        frozen_receipt = make_receipt(
            frozen_escape_run, frozen_escape_state, manifest, dense,
            analyzer=analyzer, binary=binary, attempt_nonce=frozen_nonce,
        )
        outside_frozen = root / "outside-frozen"
        outside_frozen.mkdir()
        (frozen_escape_run / "frozen-evidence").symlink_to(
            outside_frozen, target_is_directory=True
        )
        try:
            runner.ingest_receipt(
                frozen_escape_run, frozen_receipt, frozen_nonce
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("frozen-evidence symlink escape was accepted")

        forge_key = runner.cell_key("forge-modular-native", "exact", 1)
        forge_receipt = make_receipt(
            run_dir, state, manifest, forge_key, analyzer=analyzer, binary=binary
        )
        missing_binary = runner.load_json(forge_receipt)
        missing_binary["build_identity"]["binary"]["path"] = str(
            root / "not-an-executable"
        )
        write_json(forge_receipt, missing_binary)
        expect_rejected(
            forge_receipt, state, manifest, run_dir,
            "nonexistent exact Forge binary",
        )
        planted += 1

        # Native measurement producers are copied into runner-owned evidence.
        # The adapter's source path can disappear or change after ingest, while
        # any mutation of the pinned producer invalidates the receipt.
        producer = root / "native-measurement-producer"
        shutil.copy2(binary, producer)
        producer.chmod(0o755)
        producer_run = root / "native-producer-run"
        producer_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(producer_run, producer_state)
        producer_nonce, _ = runner.issue_attempt(
            producer_run, producer_state, manifest, dense
        )
        producer_receipt = make_receipt(
            producer_run, producer_state, manifest, dense,
            analyzer=analyzer, binary=binary, attempt_nonce=producer_nonce,
            measurement_producer=producer,
        )
        ingest_receipt(producer_run, producer_receipt)
        producer_state = runner.load_state(producer_run)
        frozen_producer_receipt = Path(
            producer_state["cells"][dense]["attempts"][-1]["receipt"]
        )
        frozen_producer_document = runner.regular_json(
            frozen_producer_receipt, "native producer receipt"
        )
        pinned_producer = Path(
            frozen_producer_document["build_identity"]["measurement_producer"]["path"]
        )
        assert pinned_producer != producer.resolve()
        assert "frozen-evidence" in pinned_producer.parts
        original_producer_bytes = producer.read_bytes()
        producer.write_bytes(original_producer_bytes + b"# source mutation\n")
        runner.receipt_observation(
            frozen_producer_receipt, producer_state, manifest,
            Path(producer_state["manifest_path"]), producer_run,
        )
        pinned_producer_bytes = pinned_producer.read_bytes()
        pinned_producer.chmod(0o755)
        pinned_producer.write_bytes(pinned_producer_bytes + b"# pinned mutation\n")
        expect_rejected(
            frozen_producer_receipt, producer_state, manifest, producer_run,
            "runner-owned native measurement producer mutation",
        )
        pinned_producer.write_bytes(pinned_producer_bytes)
        pinned_producer.chmod(0o555)
        producer.write_bytes(original_producer_bytes)
        producer.chmod(0o755)
        planted += 1

        # The fixed executable protocol calls a scenario adapter without a shell.
        adapter_run = root / "adapter-run"
        adapter_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(adapter_run, adapter_state)
        adapter = test_adapter_script(root)
        adapter_exe = root / "test-adapter.exe"
        shutil.copy2(adapter, adapter_exe)
        adapter_exe.chmod(0o755)
        runner.run_cells(
            adapter_run, {"dense-text-thin-strokes": adapter_exe}, {dense}, None
        )
        pinned_adapters = list((adapter_run / "tooling" / "adapters").rglob("*.exe"))
        assert len(pinned_adapters) == 1 and pinned_adapters[0].is_file()
        adapter_state = runner.load_state(adapter_run)
        assert adapter_state["cells"][dense]["status"] == "skip"
        assert runner.project_result(adapter_state)["status"] == "incomplete"
        assert runner.status_document(adapter_state)["incomplete_cells"] == 84

        # The maintained native adapter echoes the runner-issued nonce even
        # when it can only report an inconclusive dependency receipt.
        native_run = root / "native-adapter-run"
        native_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(native_run, native_state)
        runner.run_cells(
            native_run,
            {"dense-text-thin-strokes": experiment.SCRIPT_DIR / "gpu_dpr_pulp_native_adapter.py"},
            {dense},
            None,
        )
        native_state = runner.load_state(native_run)
        native_cell = native_state["cells"][dense]
        assert native_cell["status"] == "inconclusive"
        assert "native-capture:dense-text-thin-strokes" in native_cell["dependencies"]
        native_attempt = native_cell["attempts"][-1]
        native_receipt = runner.load_json(Path(native_attempt["receipt"]))
        assert native_attempt["reason"] == native_receipt["reason"]
        assert native_attempt["dependencies"] == native_receipt["dependencies"]
        assert native_attempt["nonce"] == native_receipt["attempt_nonce"]

        # Missing adapters are explicit resumable dependencies, not fake samples.
        missing_run = root / "missing-run"
        missing_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(missing_run, missing_state)
        runner.run_cells(missing_run, {}, {dense}, None)
        missing_state = runner.load_state(missing_run)
        assert missing_state["cells"][dense]["status"] == "inconclusive"
        assert missing_state["cells"][dense]["dependencies"] == [
            "adapter:dense-text-thin-strokes"
        ]

        # Timeout is a durable incomplete attempt, so resumption never loses it.
        timeout_run = root / "timeout-run"
        timeout_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
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

        noisy_run = root / "noisy-run"
        noisy_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(noisy_run, noisy_state)
        runner.run_cells(
            noisy_run,
            {"dense-text-thin-strokes": noisy_adapter_script(root)},
            {dense},
            None,
            timeout_seconds=10,
        )
        noisy_state = runner.load_state(noisy_run)
        noisy_cell = noisy_state["cells"][dense]
        assert noisy_cell["status"] == "inconclusive"
        assert noisy_cell["dependencies"] == [
            "adapter:dense-text-thin-strokes:output-limit"
        ]
        noisy_attempt = noisy_cell["attempts"][-1]
        noisy_log = runner.cell_directory(noisy_run, dense) / (
            f"adapter-{noisy_attempt['nonce']}.stdout.log"
        )
        assert noisy_log.stat().st_size == runner.OUTPUT_CAP_BYTES

        malformed_run = root / "malformed-run"
        malformed_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
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

        nonce_run = root / "nonce-run"
        nonce_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(nonce_run, nonce_state)
        runner.run_cells(
            nonce_run,
            {"dense-text-thin-strokes": wrong_nonce_adapter_script(root)},
            {dense},
            None,
        )
        nonce_state = runner.load_state(nonce_run)
        assert nonce_state["cells"][dense]["status"] == "inconclusive"
        assert nonce_state["cells"][dense]["dependencies"] == [
            "valid-cell-receipt"
        ]
        planted += 1

        analyzer_timeout_run = root / "analyzer-timeout-run"
        analyzer_timeout_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(analyzer_timeout_run, analyzer_timeout_state)
        second_dense = runner.cell_key("dense-text-thin-strokes", "exact", 1.5)
        real_ingest = runner.ingest_receipt
        def planted_analyzer_timeout(*_args: object, **_kwargs: object) -> str:
            raise subprocess.TimeoutExpired("trace-analyzer", 0.05)
        runner.ingest_receipt = planted_analyzer_timeout
        try:
            runner.run_cells(
                analyzer_timeout_run,
                {"dense-text-thin-strokes": test_adapter_script(root)},
                {dense, second_dense},
                None,
            )
        finally:
            runner.ingest_receipt = real_ingest
        analyzer_timeout_state = runner.load_state(analyzer_timeout_run)
        assert all(
            analyzer_timeout_state["cells"][key]["status"] == "inconclusive"
            for key in (dense, second_dense)
        )
        assert all(
            analyzer_timeout_state["cells"][key]["dependencies"]
            == ["valid-cell-receipt"]
            for key in (dense, second_dense)
        )
        planted += 1

        replay_run = root / "nonce-replay-run"
        replay_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(replay_run, replay_state)
        replay_nonce, _ = runner.issue_attempt(
            replay_run, replay_state, manifest, dense
        )
        replay_receipt = make_receipt(
            replay_run, replay_state, manifest, dense,
            analyzer=analyzer, binary=binary, attempt_nonce=replay_nonce,
        )
        ingest_receipt(replay_run, replay_receipt)
        try:
            ingest_receipt(replay_run, replay_receipt)
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("replayed attempt nonce was accepted")

        # A fixed-name receipt left by an earlier attempt cannot be replayed.
        stale_run = root / "stale-run"
        stale_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(stale_run, stale_state)
        stale_path = runner.cell_directory(stale_run, dense) / "receipt.json"
        make_receipt(
            stale_run, stale_state, manifest, dense,
            analyzer=analyzer, binary=binary, outcome="skip",
        )
        assert stale_path.is_file()
        runner.run_cells(
            stale_run,
            {"dense-text-thin-strokes": no_receipt_adapter_script(root)},
            {dense},
            None,
        )
        stale_state = runner.load_state(stale_run)
        assert stale_state["cells"][dense]["status"] == "inconclusive"
        assert not stale_path.exists()
        planted += 1

        # Fill all 84 cells with full synthetic receipts to exercise projection.
        complete_run = root / "complete-run"
        complete_state = runner.initial_state(
            planned, manifest, manifest_path, analyzer_identity
        )
        runner.save_state(complete_run, complete_state)
        for cell in complete_state["cells"]:
            current = runner.load_state(complete_run)
            attempt_nonce, _ = runner.issue_attempt(
                complete_run, current, manifest, cell
            )
            receipt_path = make_receipt(
                complete_run, current, manifest, cell,
                analyzer=analyzer, binary=binary,
                attempt_nonce=attempt_nonce,
            )
            ingest_receipt(complete_run, receipt_path)
        completed = runner.load_state(complete_run)
        assert runner.status_document(completed)["complete_cells"] == 84
        readiness_observations = runner.project_result(completed)["observations"]
        assert runner.policy_readiness(
            readiness_observations,
            manifest["trial_contract"]["capture_similarity_minimum"],
        )
        readiness_observations[0]["metrics"]["gpu_frame_time"].update(
            provenance="unavailable", median=None, p95=None, sample_count=0
        )
        assert not runner.policy_readiness(
            readiness_observations,
            manifest["trial_contract"]["capture_similarity_minimum"],
        )
        # A checked-in self-test must not manufacture the live collector and
        # independent review evidence required for terminal A3 acceptance.
        # First prove production finalization rejects that synthetic shape;
        # then mock only the terminal verdict while exercising A4's own
        # dependency bindings, projections, and planted negatives below.
        a2t_receipt, budget_id, a3_receipt = structural_dependency_receipts(root)
        assert "machine_id" not in runner.load_json(a2t_receipt)["machine"]
        _, _, unratified_a3 = structural_dependency_receipts(root, ratified=False)
        try:
            runner.finalize(
                complete_run, "adaptive-candidate", str(a2t_receipt),
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("synthetic terminal-shaped A3 receipt was accepted")

        def validate_structural_a3_fixture(
            receipt: dict[str, object], evidence_root: Path,
        ) -> bool:
            # A4 still needs a terminal-shaped dependency to exercise its own
            # digest, machine, budget, and plan bindings.  Validate the full
            # historical shape explicitly, then override only its deliberately
            # retired terminal verdict.  Production validation above continues
            # to prove that the same v1 receipt is nonterminal under A3 v2.
            a3_acceptance._validate_v1_receipt(
                receipt, evidence_root, allow_fixture_overhead=True,
            )
            return True

        def finalize_structural(
            disposition: str, a2t: str, budget: str, a3: str,
        ) -> dict[str, object]:
            with mock.patch.object(
                a3_acceptance, "validate_receipt",
                side_effect=validate_structural_a3_fixture,
            ):
                return runner.finalize(
                    complete_run, disposition, a2t, budget, a3,
                )

        try:
            finalize_structural(
                "adaptive-candidate", "a2t:selftest",
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("synthetic A2T dependency string was accepted")

        forged_a2t, forged_a3 = forged_minimal_dependencies(root)
        try:
            runner.finalize(
                complete_run, "adaptive-candidate", str(forged_a2t),
                budget_id, str(forged_a3),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("forged minimal A2T/A3 receipts were accepted")
        try:
            finalize_structural(
                "adaptive-candidate", str(a2t_receipt),
                budget_id, str(unratified_a3),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("unratified A3 budget was accepted")
        try:
            finalize_structural(
                "adaptive-candidate", str(a2t_receipt),
                "different-budget", str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("A3 receipt with a mismatched budget id was accepted")

        a2t_original = runner.load_json(a2t_receipt)
        a3_original = runner.load_json(a3_receipt)
        wrong_plan_a2t = json.loads(json.dumps(a2t_original))
        wrong_plan_a2t["producer_overhead_disposition"]["formal_plan_revision"] = "0" * 40
        write_json(a2t_receipt, wrong_plan_a2t)
        try:
            finalize_structural(
                "adaptive-candidate", str(a2t_receipt),
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("cross-plan A2T receipt was accepted")
        write_json(a2t_receipt, a2t_original)

        cross_machine_a3 = json.loads(json.dumps(a3_original))
        causal_id = cross_machine_a3["same_instance_a2t"]["campaign_id"]
        causal_campaign = next(
            campaign for campaign in cross_machine_a3["campaigns"]
            if campaign["identity"]["campaign_id"] == causal_id
        )
        causal_campaign["identity"]["machine_id"] = "different-machine"
        write_json(a3_receipt, cross_machine_a3)
        try:
            finalize_structural(
                "adaptive-candidate", str(a2t_receipt),
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("cross-machine A3 receipt was accepted")

        mismatched_a2t = root / "digest-mismatched-a2t.json"
        mismatched_a2t.write_bytes(a2t_receipt.read_bytes() + b"\n")
        write_json(a3_receipt, a3_original)
        try:
            finalize_structural(
                "adaptive-candidate", str(mismatched_a2t),
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("A3 receipt accepted different supplied A2T bytes")

        # Finalization consumes only runner-owned snapshots. Mutating the
        # adapter's original path is harmless; mutating the snapshot fails.
        capture_path = runner.cell_directory(complete_run, dense) / "capture.png"
        capture_bytes = capture_path.read_bytes()
        capture_path.write_bytes(capture_bytes + b"mutated")
        finalize_structural(
            "adaptive-candidate", str(a2t_receipt),
            budget_id, str(a3_receipt),
        )
        snapshot_capture = complete_run / next(
            item["path"]
            for item in runner.load_state(complete_run)["cells"][dense]["observation"]["artifacts"]
            if item["kind"] == "capture"
        )
        snapshot_bytes = snapshot_capture.read_bytes()
        snapshot_capture.chmod(0o644)
        snapshot_capture.write_bytes(snapshot_bytes + b"mutated")
        try:
            finalize_structural(
                "adaptive-candidate", str(a2t_receipt),
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("runner-owned artifact snapshot mutation was accepted")
        snapshot_capture.write_bytes(snapshot_bytes)
        snapshot_capture.chmod(0o444)
        capture_path.write_bytes(capture_bytes)

        complete_snapshot = runner.load_state(complete_run)
        projected_mutation = runner.load_state(complete_run)
        projected_mutation["cells"][dense]["observation"]["physical_size"]["width"] += 1
        runner.save_state(complete_run, projected_mutation)
        try:
            finalize_structural(
                "adaptive-candidate", str(a2t_receipt),
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("post-ingest projected observation mutation was accepted")
        runner.save_state(complete_run, complete_snapshot)

        plan_mutation = runner.load_state(complete_run)
        plan_mutation["plan"]["pulp_sha"] = "0" * 40
        runner.save_state(complete_run, plan_mutation)
        try:
            finalize_structural(
                "adaptive-candidate", str(a2t_receipt),
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("embedded plan mutation was accepted")
        runner.save_state(complete_run, complete_snapshot)

        mutated_manifest = runner.load_json(manifest_path)
        mutated_manifest["trial_contract"]["capture_similarity_minimum"] = 0.98
        write_json(manifest_path, mutated_manifest)
        try:
            finalize_structural(
                "adaptive-candidate", str(a2t_receipt),
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("post-init manifest mutation was accepted")
        write_json(manifest_path, manifest)

        binary_bytes = binary.read_bytes()
        binary.write_bytes(binary_bytes + b"# mutation\n")
        finalize_structural(
            "adaptive-candidate", str(a2t_receipt),
            budget_id, str(a3_receipt),
        )
        web_key = runner.cell_key("super-convolver-web", "exact", 1)
        web_snapshot_receipt = runner.regular_json(
            Path(runner.load_state(complete_run)["cells"][web_key]["attempts"][-1]["receipt"]),
            "web receipt",
        )
        pinned_web_data = Path(
            web_snapshot_receipt["build_identity"]["web_ui_artifacts"]
            ["PulpSuperConvolverUi.data"]["path"]
        )
        pinned_web_bytes = pinned_web_data.read_bytes()
        pinned_web_data.chmod(0o644)
        pinned_web_data.write_bytes(pinned_web_bytes + b"# mutation\n")
        try:
            finalize_structural(
                "adaptive-candidate", str(a2t_receipt),
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("pinned browser build mutation was accepted")
        pinned_web_data.write_bytes(pinned_web_bytes)
        pinned_web_data.chmod(0o444)
        forge_snapshot_receipt = runner.regular_json(
            Path(runner.load_state(complete_run)["cells"][forge_key]["attempts"][-1]["receipt"]),
            "Forge receipt",
        )
        pinned_binary = Path(
            forge_snapshot_receipt["build_identity"]["binary"]["path"]
        )
        pinned_binary_bytes = pinned_binary.read_bytes()
        pinned_binary.chmod(0o755)
        pinned_binary.write_bytes(pinned_binary_bytes + b"# mutation\n")
        try:
            finalize_structural(
                "adaptive-candidate", str(a2t_receipt),
                budget_id, str(a3_receipt),
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("runner-owned exact binary mutation was accepted")
        pinned_binary.write_bytes(pinned_binary_bytes)
        pinned_binary.chmod(0o555)
        binary.write_bytes(binary_bytes)
        binary.chmod(0o755)

        b5 = finalize_structural(
            "adaptive-candidate", str(a2t_receipt),
            budget_id, str(a3_receipt)
        )
        assert b5["status"] == "waiting-trigger"
        assert b5["requires"] == ["B0-adopted-vellum-api-refresh"]
        assert b5["authorizes_policy_change"] is False
        result = runner.load_json(complete_run / "result.json")
        assert result["status"] == "complete" and len(result["observations"]) == 84
        assert not experiment.result_semantic_errors(
            result, manifest, experiment.canonical_sha256(manifest)
        )

        # An authoritative failing receipt can only close B5 no-change.
        failed_receipt = make_receipt(
            complete_run, runner.load_state(complete_run), manifest, dense,
            analyzer=analyzer, binary=binary, outcome="fail", fidelity_ok=False,
            attempt_nonce=runner.issue_attempt(
                complete_run, runner.load_state(complete_run), manifest, dense
            )[0],
        )
        ingest_receipt(complete_run, failed_receipt)
        try:
            finalize_structural(
                "configured-max-candidate", str(a2t_receipt),
                budget_id, str(a3_receipt)
            )
        except runner.EvidenceError:
            planted += 1
        else:
            raise AssertionError("candidate crossed a planted fidelity failure")
        b5 = finalize_structural(
            "no-change", str(a2t_receipt),
            budget_id, str(a3_receipt)
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
