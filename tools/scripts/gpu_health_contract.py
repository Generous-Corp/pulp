#!/usr/bin/env python3
"""Cross-field validation shared by GPU-health producers and verifiers."""

from __future__ import annotations

from typing import Any

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


def derived_verdict(verdicts: list[str]) -> str:
    return next(candidate for candidate in PRECEDENCE if candidate in verdicts)


def evidence_code_matches(code: str, stage: str, verdict: str) -> bool:
    if code.startswith("gpu."):
        if code == "gpu.adapter.null":
            return stage == "adapter" and verdict == "fail"
        return code == f"gpu.{stage}.{verdict}"
    return SPECIFIC_EVIDENCE_CODE_BINDINGS.get(code) == (stage, verdict)


def semantic_errors(document: dict[str, Any]) -> list[str]:
    """Validate relationships deliberately outside the portable JSON Schema."""
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
        if (measurement["readback_completed"] is False
                and measurement["pixel_output_produced"] is True):
            errors.append("pixels were claimed after failed readback")
        if (measurement["content_floor_passed"] is True
                and measurement["pixel_output_produced"] is not True):
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
