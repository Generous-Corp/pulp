#!/usr/bin/env python3
"""Prove the GPU health-read envelope and its embedded v1 health contract."""

from __future__ import annotations

import copy
import json
import re
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))
import json_schema_lite  # noqa: E402


def load(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def startup_document(health: dict[str, Any]) -> dict[str, Any]:
    source = "a" * 64
    shader = "b" * 64
    target = "c" * 64
    return {
        "schema": "pulp.gpu-health-read-result.v1",
        "version": 1,
        "health": health,
        "startup": {
            "status": "complete",
            "verdict": "pass",
            "measurement_endpoint": "native-compositor-presentation",
            "budget": {
                "budget_id": "editor-first-visible-v1",
                "version": 1,
                "status": "ratified",
                "clock_origin": "editor_open_requested",
                "endpoint": "first_nonblank_presented_frame",
                "interaction_hitch_metric":
                    "max_present_interval_before_first_nonblank_ms",
                "trial_count": 2,
                "cold_trial_count": 1,
                "warm_trial_count": 1,
                "percentile": 95,
                "threshold_ms": 20,
                "threshold_source": "reference-host-study-1",
                "reference_hosts": [
                    {"host_id": "forge-shell-m5", "refresh_rate_hz": 60}
                ],
            },
            "correlation": {
                "gpu_evidence_id": health["run_id"],
                "trace_evidence_id": "trace-evidence-1",
            },
            "capture": {
                "event_capacity": 64,
                "event_count": 8,
                "dropped_event_count": 0,
                "truncated": False,
                "missing_trace_categories": [],
            },
            "identity": {
                "pulp_build_id": "pulp-build-1",
                "vellum_revision": "vellum-revision-1",
                "source_signature_sha256": source,
                "shader_signature_sha256": shader,
                "expected_target_signature_sha256": target,
                "adapter_class": "hardware",
            },
            "trials": [
                {
                    "sequence": sequence,
                    "cache_state": cache,
                    "lifecycle_id": f"lifecycle-{sequence}",
                    "cache_provenance": (
                        "fresh-process" if cache == "cold"
                        else "same-process-editor-reopen"
                    ),
                    "editor_open_to_first_nonblank_ms": duration,
                    "interaction_hitch_ms": hitch,
                    "shader_compile_ms": compile_ms,
                    "upload_ms": 1,
                    "hidden_frame_ms": 1,
                    "present_ms": 2,
                    "observed_target_signature_sha256": target,
                    "content_floor_passed": True,
                    "visible_state": "prepared",
                    "verdict": "pass",
                    "diagnostic_code": "gpu.startup.pass",
                }
                for sequence, cache, duration, hitch, compile_ms in (
                    (0, "cold", 10, 4, 2),
                    (1, "warm", 12, 5, 0),
                )
            ],
            "observed_percentile_ms": 12,
            "interaction_hitch_percentile_ms": 5,
            "pipeline_contribution": "not_material",
            "causal_attribution": "complete",
            "disposition": "no-change",
        },
    }


def strip_annotations(schema: dict[str, Any]) -> dict[str, Any]:
    return {
        key: value
        for key, value in schema.items()
        if key not in {"$schema", "$id", "title", "description"}
    }


def assert_invalid(document: dict[str, Any], schema: dict[str, Any], label: str) -> None:
    if not json_schema_lite.validate(document, schema):
        raise AssertionError(f"mutation unexpectedly valid: {label}")


def required_object_paths(schema: dict[str, Any], path: tuple[Any, ...] = ()):
    if schema.get("type") == "object":
        yield path, tuple(schema.get("required", ()))
        for key, child in schema.get("properties", {}).items():
            yield from required_object_paths(child, path + (key,))
    elif schema.get("type") == "array" and "items" in schema:
        yield from required_object_paths(schema["items"], path + (0,))


def at_path(document: Any, path: tuple[Any, ...]) -> Any:
    for part in path:
        document = document[part]
    return document


def main() -> int:
    schema = load(ROOT / "docs/contracts/gpu-health-read-result-v1.schema.json")
    health_schema = load(ROOT / "docs/contracts/gpu-health-result-v1.schema.json")
    embedded = schema["properties"]["health"]
    assert embedded == strip_annotations(health_schema), (
        "embedded health contract drifted from gpu-health-result-v1.schema.json"
    )

    health = load(ROOT / "test/fixtures/gpu-ux/pass-hardware.json")
    document = startup_document(health)
    problems = json_schema_lite.validate(document, schema)
    assert not problems, f"valid health-read fixture: {problems}"

    mutation_count = 0
    for path, required in required_object_paths(schema):
        target = at_path(document, path)
        for key in required:
            mutated = copy.deepcopy(document)
            del at_path(mutated, path)[key]
            assert_invalid(mutated, schema, f"delete required {path}:{key}")
            mutation_count += 1
        if target:
            mutated = copy.deepcopy(document)
            at_path(mutated, path)["unexpected"] = True
            assert_invalid(mutated, schema, f"unknown field {path}")
            mutation_count += 1

    nullable_paths = (
        ("startup", "correlation", "gpu_evidence_id"),
        ("startup", "correlation", "trace_evidence_id"),
        ("startup", "identity", "vellum_revision"),
        ("startup", "identity", "source_signature_sha256"),
        ("startup", "trials", 0, "shader_compile_ms"),
        ("startup", "disposition"),
    )
    for path in nullable_paths:
        mutated = copy.deepcopy(document)
        parent, leaf = path[:-1], path[-1]
        at_path(mutated, parent)[leaf] = None
        problems = json_schema_lite.validate(mutated, schema)
        assert not problems, f"nullable field rejected {path}: {problems}"

    source = (ROOT / "tools/cli/gpu_health/src/health_read_result_json.cpp").read_text(
        encoding="utf-8"
    )
    native = {
        code: verdict
        for code, verdict in re.findall(
            r'DiagnosticBinding\{"([^"]+)", Verdict::(\w+)\}', source
        )
    }
    schema_codes = set(
        schema["properties"]["startup"]["properties"]["trials"]["items"]
        ["properties"]["diagnostic_code"]["enum"]
    )
    assert set(native) == schema_codes, "native and schema startup codes drifted"
    assert native["gpu.startup.pass"] == "pass"
    assert native["gpu.startup.blank"] == "fail"
    assert native["gpu.startup.event_loss"] == "unverified"
    assert native["gpu.startup.timeout"] == "unavailable"

    print(
        "gpu-health-read-contract: "
        f"required_mutations={mutation_count} nullable={len(nullable_paths)} "
        f"codes={len(schema_codes)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
