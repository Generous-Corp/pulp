#!/usr/bin/env python3
"""Prove the GPU health v1 shape and its cross-field contract."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
FIXTURES = ROOT / "test" / "fixtures" / "gpu-ux"
sys.path.insert(0, str(SCRIPT_DIR))
import json_schema_lite  # noqa: E402

VERDICTS = {"pass", "fail", "unavailable", "unverified"}
STAGES = {
    "configuration", "adapter", "shader_compile", "pipeline_create", "render",
    "submit", "readback", "content", "compute", "device_state",
}
CLASSES = {"hardware", "software", "null", "unknown"}
PRECEDENCE = ("fail", "unavailable", "unverified", "pass")
SPECIFIC_EVIDENCE_CODE_BINDINGS = {
    "gpu_compute_adapter_acquired": ("adapter", "pass"),
    "gpu_compute_adapter_identity_unverified": ("adapter", "unverified"),
    "gpu_compute_device_lost": ("device_state", "fail"),
    "gpu_compute_execution_failed": ("compute", "fail"),
    "gpu_compute_initialization_unavailable": ("adapter", "unavailable"),
    "gpu_compute_not_built": ("configuration", "unavailable"),
    "gpu_compute_oracle_mismatch": ("compute", "fail"),
    "gpu_compute_oracle_passed": ("compute", "pass"),
    "render_not_requested": ("configuration", "unverified"),
    "renderer3d_adapter_unavailable": ("adapter", "unavailable"),
    "renderer3d_blank_output": ("content", "fail"),
    "renderer3d_content_floor_passed": ("content", "pass"),
    "renderer3d_not_compiled": ("configuration", "unavailable"),
    "renderer3d_setup_failed": ("pipeline_create", "fail"),
    "renderer3d_readback_completed": ("readback", "pass"),
    "renderer3d_readback_failed": ("readback", "fail"),
    "renderer3d_render_completed": ("render", "pass"),
    "renderer3d_submit_completed": ("submit", "pass"),
    "skia_graphite_content_floor_passed": ("content", "pass"),
    "skia_graphite_content_mismatch": ("content", "fail"),
    "skia_graphite_frame_failed": ("render", "fail"),
    "skia_graphite_readback_completed": ("readback", "pass"),
    "skia_graphite_render_completed": ("render", "pass"),
    "skia_graphite_unavailable": ("configuration", "unavailable"),
    "wgsl.async_uncaptured_error": ("shader_compile", "fail"),
}


def load(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def derived_verdict(verdicts: list[str]) -> str:
    return next(candidate for candidate in PRECEDENCE if candidate in verdicts)


def evidence_code_matches(code: str, stage: str, verdict: str) -> bool:
    if code.startswith("gpu."):
        if code == "gpu.adapter.null":
            return stage == "adapter" and verdict == "fail"
        return code == f"gpu.{stage}.{verdict}"
    return SPECIFIC_EVIDENCE_CODE_BINDINGS.get(code) == (stage, verdict)


def semantic_errors(document: dict[str, Any]) -> list[str]:
    """Mirror relationships deliberately outside the portable JSON Schema."""
    errors: list[str] = []
    probes = document["probes"]
    if len({probe["probe_id"] for probe in probes}) != len(probes):
        errors.append("probe ids are not unique")

    expected_sequence = 0
    lost = False
    pixel_proof = False
    authentic_identity = False
    probe_verdicts: list[str] = []
    for probe in probes:
        adapter = probe["adapter"]
        if adapter["class"] == "hardware" and adapter["status"] != "authentic":
            errors.append("hardware identity is not authentic")
        if adapter["status"] == "authentic" and not adapter["backend"]:
            errors.append("authentic identity lacks backend")
        if adapter["class"] == "null" and probe["verdict"] == "pass":
            errors.append("null adapter passed")

        seen: set[str] = set()
        event_verdicts: list[str] = []
        for event in probe["events"]:
            if event["sequence"] != expected_sequence:
                errors.append("event sequence is not globally contiguous")
            expected_sequence += 1
            if event["stage"] in seen:
                errors.append("stage is duplicated within a probe")
            seen.add(event["stage"])
            if not evidence_code_matches(event["code"], event["stage"], event["verdict"]):
                errors.append("event code disagrees with stage or verdict")
            event_verdicts.append(event["verdict"])
        if derived_verdict(event_verdicts) != probe["verdict"]:
            errors.append("probe verdict disagrees with events")
        if probe["required"]:
            probe_verdicts.append(probe["verdict"])
            authentic_identity |= adapter["status"] == "authentic"

        measurement = probe["measurements"]
        if measurement["readback_completed"] is False and measurement["pixel_output_produced"] is True:
            errors.append("pixels were claimed after failed readback")
        if measurement["content_floor_passed"] is True and measurement["pixel_output_produced"] is not True:
            errors.append("content floor lacks pixel proof")
        if any(measurement[key] is not None for key in (
            "non_transparent_pixel_count", "distinct_color_count", "rgba_fingerprint"
        )) and measurement["pixel_output_produced"] is not True:
            errors.append("pixel metrics lack pixel proof")
        render_stage = bool(seen & {"render", "submit", "readback", "content"})
        compute_stage = "compute" in seen
        if probe["verdict"] == "pass" and render_stage and not all(
            measurement[key] is True for key in (
                "command_submitted", "readback_completed",
                "pixel_output_produced", "content_floor_passed"
            )
        ):
            errors.append("passing render probe lacks stage-specific proof")
        if probe["verdict"] == "pass" and compute_stage and not all(
            measurement[key] is True for key in (
                "compute_initialized", "compute_oracle_passed"
            )
        ):
            errors.append("passing compute probe lacks stage-specific proof")
        pixel_proof |= probe["required"] and all(measurement[key] is True for key in (
            "readback_completed", "pixel_output_produced", "content_floor_passed"
        ))
        lost |= probe["required"] and measurement["device_lost"] is True

    if not probe_verdicts:
        errors.append("no required probe contributes to the verdict")
    if probe_verdicts and derived_verdict(probe_verdicts) != document["verdict"]:
        errors.append("top verdict disagrees with probes")
    if not document["render_requested"] and document["verdict"] != "unverified":
        errors.append("no-render result is not unverified")
    if document["verdict"] == "pass" and not pixel_proof:
        errors.append("pass lacks readback pixel content proof")
    if document["verdict"] == "pass" and not authentic_identity:
        errors.append("pass lacks authentic adapter identity")
    allowed_states = {
        "pass": {"healthy"}, "fail": {"failed", "lost"},
        "unavailable": {"unavailable"}, "unverified": {"unverified"},
    }
    if document["health_state"] not in allowed_states[document["verdict"]]:
        errors.append("health state disagrees with verdict")
    if (document["health_state"] == "lost") != lost:
        errors.append("lost state disagrees with device_lost evidence")
    return errors


def assert_invalid(document: dict[str, Any], schema: dict[str, Any], label: str) -> None:
    if not json_schema_lite.validate(document, schema) and not semantic_errors(document):
        raise AssertionError(f"mutation unexpectedly valid: {label}")


def mutate_required_fields(document: dict[str, Any], schema: dict[str, Any]) -> int:
    """Delete and rename every required member at every nested object level."""
    count = 0
    paths = [document, document["probes"][0], document["probes"][0]["adapter"],
             document["probes"][0]["measurements"], document["probes"][0]["events"][0]]
    for target_index, target in enumerate(paths):
        for key in tuple(target):
            deleted = copy.deepcopy(document)
            cursor = [deleted, deleted["probes"][0], deleted["probes"][0]["adapter"],
                      deleted["probes"][0]["measurements"], deleted["probes"][0]["events"][0]][target_index]
            del cursor[key]
            assert_invalid(deleted, schema, f"delete required {target_index}:{key}")
            renamed = copy.deepcopy(document)
            cursor = [renamed, renamed["probes"][0], renamed["probes"][0]["adapter"],
                      renamed["probes"][0]["measurements"], renamed["probes"][0]["events"][0]][target_index]
            cursor[key + "_renamed"] = cursor.pop(key)
            assert_invalid(renamed, schema, f"rename required {target_index}:{key}")
            count += 2
    return count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path,
                        help="also verify the real CLI no-render JSON/exit contract")
    args = parser.parse_args()
    schema = load(ROOT / "docs" / "contracts" / "gpu-health-result-v1.schema.json")
    manifest = load(FIXTURES / "manifest.json")
    schema_codes = set(schema["properties"]["probes"]["items"]["properties"]
                       ["events"]["items"]["properties"]["code"]["enum"])
    registry_header = (ROOT / "tools" / "cli" / "gpu_health" / "include" /
                       "pulp_tooling" / "gpu_health" / "health_result.hpp").read_text(
                           encoding="utf-8")
    registry_block = registry_header.split("inline constexpr std::array kEvidenceCodes{", 1)[1].split("};", 1)[0]
    native_codes = set(re.findall(r'std::string_view\{"([^"]+)"\}', registry_block))
    assert native_codes == schema_codes, "native diagnostic-code registry and schema enum drifted"
    native_source = (ROOT / "tools" / "cli" / "gpu_health" / "src" /
                     "health_result_json.cpp").read_text(encoding="utf-8")
    native_specific_bindings = {
        code: (stage, verdict)
        for code, stage, verdict in re.findall(
            r'EvidenceCodeBinding\{"([^"]+)", Stage::(\w+), Verdict::(\w+)\}',
            native_source)
    }
    assert native_specific_bindings == SPECIFIC_EVIDENCE_CODE_BINDINGS, (
        "native and Python specific diagnostic-code bindings drifted")

    for group in ("source_artifacts", "auxiliary_fixtures"):
        for name, artifact in manifest[group].items():
            path = FIXTURES / artifact["path"]
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            assert digest == artifact["sha256"], f"{name}: source hash drifted"
            if "expected" in artifact:
                assert artifact["expected"] in schema_codes, f"{name}: unregistered expected code"

    listed_files = {"manifest.json", *manifest["valid_fixtures"]}
    listed_files.update(item["path"] for group in
                        ("source_artifacts", "auxiliary_fixtures")
                        for item in manifest[group].values())
    actual_files = {path.name for path in FIXTURES.iterdir() if path.is_file()}
    assert listed_files == actual_files, (
        f"fixture manifest drift: missing={sorted(actual_files - listed_files)} "
        f"stale={sorted(listed_files - actual_files)}")

    stage_matrix = load(FIXTURES / manifest["coverage"]["stage_positive_negative"])
    assert {row["stage"] for row in stage_matrix["outcomes"]} == STAGES
    for row in stage_matrix["outcomes"]:
        assert row["positive_code"] in schema_codes
        assert row["negative_code"] in schema_codes
        assert row["positive_code"].endswith(".pass")
        assert row["negative_code"].endswith(".fail")
    documents = [load(FIXTURES / name) for name in manifest["valid_fixtures"]]
    for name, document in zip(manifest["valid_fixtures"], documents):
        problems = json_schema_lite.validate(document, schema)
        assert not problems, f"{name}: schema: {problems}"
        problems = semantic_errors(document)
        assert not problems, f"{name}: semantic: {problems}"
        assert all(event["code"] in schema_codes
                   for probe in document["probes"] for event in probe["events"])

    assert {doc["verdict"] for doc in documents} == VERDICTS
    assert {doc["probes"][0]["adapter"]["class"] for doc in documents} == CLASSES
    assert {event["stage"] for doc in documents for event in doc["probes"][0]["events"]} == STAGES

    base = documents[0]
    mutation_count = mutate_required_fields(base, schema)
    enum_locations = [
        ("verdict",), ("health_state",), ("probes", 0, "verdict"),
        ("probes", 0, "adapter", "status"), ("probes", 0, "adapter", "class"),
        ("probes", 0, "events", 0, "stage"), ("probes", 0, "events", 0, "verdict"),
    ]
    for location in enum_locations:
        mutated = copy.deepcopy(base)
        cursor: Any = mutated
        for part in location[:-1]:
            cursor = cursor[part]
        cursor[location[-1]] = "unsupported-value"
        assert_invalid(mutated, schema, f"enum {location}")
        mutation_count += 1

    unknown_code = copy.deepcopy(base)
    unknown_code["probes"][0]["events"][0]["code"] = "gpu.configuration.renamed"
    assert_invalid(unknown_code, schema, "renamed diagnostic code")
    mutation_count += 1

    # Every stage is independently fail-able and deterministically rolls up.
    negative_codes = {row["stage"]: row["negative_code"]
                      for row in stage_matrix["outcomes"]}
    for stage in STAGES:
        mutated = copy.deepcopy(base)
        mutated["verdict"] = mutated["probes"][0]["verdict"] = "fail"
        mutated["health_state"] = "failed"
        event = next(event for event in mutated["probes"][0]["events"]
                     if event["stage"] == stage)
        event["verdict"] = "fail"
        event["code"] = negative_codes[stage]
        assert not semantic_errors(mutated), f"stage mutation did not roll up: {stage}"
        mutation_count += 1

    code_stage_mismatch = copy.deepcopy(base)
    code_stage_mismatch["probes"][0]["events"][0]["code"] = "gpu.adapter.pass"
    assert_invalid(code_stage_mismatch, schema, "diagnostic code/stage mismatch")
    mutation_count += 1

    code_verdict_mismatch = copy.deepcopy(base)
    code_verdict_mismatch["probes"][0]["events"][0]["code"] = "gpu.configuration.fail"
    assert_invalid(code_verdict_mismatch, schema, "diagnostic code/verdict mismatch")
    mutation_count += 1

    semantic_mutations: list[tuple[str, Any]] = [
        ("sequence-gap", lambda d: d["probes"][0]["events"][1].update(sequence=9)),
        ("probe-verdict", lambda d: d["probes"][0].update(verdict="fail")),
        ("top-verdict", lambda d: d.update(verdict="fail", health_state="failed")),
        ("hardware-unverified", lambda d: d["probes"][0]["adapter"].update(status="unverified")),
        ("null-pass", lambda d: d["probes"][0]["adapter"].update(**{"class": "null"})),
        ("no-render-pass", lambda d: d.update(render_requested=False)),
        ("lost-mismatch", lambda d: d.update(health_state="lost")),
        ("pass-no-content", lambda d: d["probes"][0]["measurements"].update(content_floor_passed=False)),
        ("pass-no-authentic-identity", lambda d: d["probes"][0]["adapter"].update(status="unverified", **{"class": "unknown"})),
    ]
    for label, operation in semantic_mutations:
        mutated = copy.deepcopy(base)
        operation(mutated)
        assert_invalid(mutated, schema, label)
        mutation_count += 1

    optional_probe = copy.deepcopy(base)
    optional_probe["probes"].append({
        "probe_id": "optional-renderer",
        "required": False,
        "verdict": "unavailable",
        "adapter": {
            "status": "unavailable", "class": "unknown", "backend": None,
            "name": None, "vendor": None, "architecture": None, "device": None,
        },
        "measurements": {
            "command_submitted": None, "readback_completed": None,
            "pixel_output_produced": None, "content_floor_passed": None,
            "compute_initialized": None, "compute_oracle_passed": None,
            "device_lost": None, "non_transparent_pixel_count": None,
            "distinct_color_count": None, "rgba_fingerprint": None,
        },
        "events": [{
            "sequence": len(base["probes"][0]["events"]),
            "stage": "configuration", "verdict": "unavailable",
            "code": "renderer3d_not_compiled", "detail": "optional fixture probe",
        }],
    })
    assert not json_schema_lite.validate(optional_probe, schema)
    assert not semantic_errors(optional_probe)

    async_doc = load(FIXTURES / manifest["coverage"]["async_invalid_wgsl"])
    shader = next(event for event in async_doc["probes"][0]["events"] if event["stage"] == "shader_compile")
    assert async_doc["verdict"] == "fail" and shader["verdict"] == "fail"
    assert "asynchronous uncaptured-error callback" in shader["detail"]

    if args.cli:
        rendered = subprocess.run(
            [str(args.cli.resolve()), "doctor", "gpu", "--json"],
            cwd=ROOT, capture_output=True, text=True, check=False)
        assert rendered.returncode == 0, (
            f"healthy required probes must exit 0, got {rendered.returncode}: "
            f"{rendered.stderr}\n{rendered.stdout}")
        rendered_document = json.loads(rendered.stdout)
        problems = json_schema_lite.validate(rendered_document, schema)
        assert not problems, f"real CLI rendered schema: {problems}"
        problems = semantic_errors(rendered_document)
        assert not problems, f"real CLI rendered semantic: {problems}"
        assert rendered_document["render_requested"] is True
        assert rendered_document["verdict"] == "pass"

        completed = subprocess.run(
            [str(args.cli.resolve()), "doctor", "gpu", "--no-render", "--json"],
            cwd=ROOT, capture_output=True, text=True, check=False)
        assert completed.returncode == 2, (
            f"no-render must exit 2, got {completed.returncode}: {completed.stderr}")
        emitted = json.loads(completed.stdout)
        problems = json_schema_lite.validate(emitted, schema)
        assert not problems, f"real CLI no-render schema: {problems}"
        problems = semantic_errors(emitted)
        assert not problems, f"real CLI no-render semantic: {problems}"
        assert emitted["render_requested"] is False
        assert emitted["verdict"] == "unverified"

    cli_suffix = " cli=render,no-render" if args.cli else ""
    print(f"gpu_health_contract_verified fixtures={len(documents)} mutations={mutation_count} stages={len(STAGES)}{cli_suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
