#!/usr/bin/env python3
"""Verify a durable A2 GPU-probe acceptance receipt."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any

import json_schema_lite


SHA256 = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA = re.compile(r"^[0-9a-f]{40}$")
GPU_EVIDENCE_ID = re.compile(r"^[0-9a-f]{32}$")
SOURCE_STAMP = re.compile(r"^[0-9a-f]{7,40}$")
GROUPS = {
    "compute": "gpu-compute.magnitude.v1",
    "stft": "gpu-audio.stft.v1",
    "renderer": "renderer3d.hardcoded-cube.v1",
    "threejs": "threejs.multi-pass.v1",
}
HARDWARE_REQUIRED = {"compute", "stft", "threejs"}
EXPECTED_SAMPLE_COUNTS = {
    "compute": 256,
    "stft": 1024,
    "renderer": 0,
    "threejs": 5,
}
EXPECTED_DIMENSIONS = {
    "compute": {"width": 256, "height": 1, "work_items": 256},
    "stft": {"width": 1024, "height": 1, "work_items": 1024},
    "renderer": {"width": 128, "height": 128, "work_items": 16_384},
    "threejs": {"width": 96, "height": 96, "work_items": 9_216},
}
RENDERER_NEGATIVE_DIMENSIONS = {"width": 32, "height": 32, "work_items": 1_024}
RENDERER_NEGATIVE_MUTATION = "pre-submit-framebuffer-downscale"
EXPECTED_BINARY_ROLES = {
    "compute": "installed_rust_cli",
    "stft": "installed_rust_cli",
    "renderer": "scene3d_cpp_cli",
    "threejs": "v8_threejs_cpp_cli",
}
EXPECTED_BINARY_ROLES_V2 = {group: "installed_rust_cli" for group in GROUPS}
EXPECTED_SOURCE_BLOBS = {
    "core/render/include/pulp/render/gpu_compute.hpp",
    "core/render/include/pulp/render/renderer3d.hpp",
    "core/render/src/gpu_compute.cpp",
    "core/render/src/renderer3d.cpp",
    "tools/cli/gpu_probe/src/native_acceptance.cpp",
    "tools/cli/gpu_probe/src/native_recipes.cpp",
    "tools/cli/gpu_probe/src/probe_result.cpp",
    "tools/cli/gpu_probe/src/probe_result_json.cpp",
    "tools/cli/gpu_probe/src/stft_native_acceptance.cpp",
}
EXPECTED_BINARIES = {
    "installed_rust_cli",
    "installed_cpp_delegate",
    "installed_mcp",
    "scene3d_cpp_cli",
    "v8_threejs_cpp_cli",
}
EXPECTED_BINARIES_V2 = {
    "installed_rust_cli",
    "installed_cpp_delegate",
    "installed_mcp",
}
EXPECTED_SOURCE_BLOBS_V2 = EXPECTED_SOURCE_BLOBS | {
    "CMakeLists.txt",
    "core/runtime/include/pulp/runtime/build_info.hpp.in",
    "docs/contracts/gpu-probe-result-v1.schema.json",
    "docs/contracts/gpu-health-result-v1.schema.json",
    "docs/status/gpu-recipes.schema.json",
    "docs/status/gpu-recipes.yaml",
    "experimental/pulp-rs/CMakeLists.txt",
    "experimental/pulp-rs/src/fallthrough.rs",
    "experimental/pulp-rs/src/main.rs",
    "tools/cli/CMakeLists.txt",
    "tools/cli/cmd_gpu.cpp",
    "tools/cli/gpu_probe/CMakeLists.txt",
    "tools/cli/gpu_probe/include/pulp_tooling/gpu_probe/probe_result.hpp",
    "tools/cli/gpu_probe/include/pulp_tooling/gpu_probe/recipes.hpp",
    "tools/cli/gpu_recipe_catalog_data.h.in",
    "tools/cmake/PulpInstallRules.cmake",
    "tools/mcp/CMakeLists.txt",
    "tools/mcp/mcp_gpu_tools.cpp",
    "tools/mcp/pulp_mcp.cpp",
    "tools/scripts/gpu_probe_acceptance.py",
    "tools/scripts/json_schema_lite.py",
    "tools/scripts/sdk_capability_handoff.py",
    "tools/scripts/sdk_provenance.py",
    "tools/scripts/verify_gpu_probe_acceptance.py",
}
EXPECTED_PLAN_REVISION = "641649b7e7fece6baae34380b6e719904506af22"
EXPECTED_PLAN_SHA256 = "00bdb8bd55fb90fb42d98a09442d2b168505a23a4208cb5b9edb67b01de69f07"
EXPECTED_PLAN_BLOB = "2d1c461d3ea640f75786a72c312d074f68f59028"
EXPECTED_FORGE_REVISION = "0750a88dea3af7fca927a8c02887e071109407ae"
EXPECTED_FORGE_PULP_REF_BLOB = "3e54500140a1dc5de0dbefaab29612916f257ecd"
RESULT_SCHEMA = (
    Path(__file__).resolve().parents[2]
    / "docs/contracts/gpu-probe-result-v1.schema.json"
)
HEALTH_SCHEMA = RESULT_SCHEMA.with_name("gpu-health-result-v1.schema.json")


def _load(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _mapping(value: Any, name: str, errors: list[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.append(f"{name} must be an object")
        return {}
    return value


def _git_blobs(commit: str, paths: set[str]) -> dict[str, str]:
    completed = subprocess.run(
        ["git", "ls-tree", commit, "--", *sorted(paths)],
        cwd=Path(__file__).resolve().parents[2],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        return {}
    blobs: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        try:
            metadata, path = line.split("\t", 1)
            _mode, kind, value = metadata.split()
        except ValueError:
            continue
        if kind == "blob" and GIT_SHA.fullmatch(value):
            blobs[path] = value
    return blobs


def _checkout_blobs(paths: set[str]) -> dict[str, str]:
    completed = subprocess.run(
        ["git", "hash-object", "--stdin-paths"],
        cwd=Path(__file__).resolve().parents[2],
        input="".join(f"{path}\n" for path in sorted(paths)),
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        return {}
    values = completed.stdout.splitlines()
    if len(values) != len(paths):
        return {}
    return {
        path: value
        for path, value in zip(sorted(paths), values)
        if GIT_SHA.fullmatch(value)
    }


def _canonical_repeat(result: dict[str, Any]) -> dict[str, Any]:
    value = copy.deepcopy(result)
    value.pop("gpu_evidence_id", None)
    return value


def _png_metrics(path: Path) -> dict[str, int]:
    """Decode a bounded 8-bit RGB/RGBA PNG and prove nonblank content."""
    import binascii
    import struct
    import zlib

    if path.is_symlink():
        raise ValueError("Forge screenshot must be an in-receipt regular file")
    data = path.read_bytes()
    if len(data) > 8 * 1024 * 1024 or not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("Forge screenshot is not a bounded PNG")
    offset = 8
    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    while offset + 12 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        end = offset + 12 + length
        if end > len(data):
            raise ValueError("Forge screenshot PNG is truncated")
        payload = data[offset + 8:offset + 8 + length]
        declared_crc = struct.unpack(">I", data[end - 4:end])[0]
        if (binascii.crc32(kind + payload) & 0xffffffff) != declared_crc:
            raise ValueError("Forge screenshot PNG has an invalid chunk CRC")
        if kind == b"IHDR":
            if len(payload) != 13:
                raise ValueError("Forge screenshot PNG has a malformed IHDR")
            width, height, bit_depth, color_type, _compression, _filter, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
        offset = end
    if (
        not isinstance(width, int) or not isinstance(height, int)
        or width < 320 or height < 240 or width * height > 4_000_000
        or bit_depth != 8 or color_type not in {2, 6} or interlace != 0
    ):
        raise ValueError("Forge screenshot has unsupported or implausible PNG geometry")
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    expected_filtered_bytes = (stride + 1) * height
    try:
        inflater = zlib.decompressobj()
        filtered = inflater.decompress(bytes(compressed), expected_filtered_bytes + 1)
        if inflater.unconsumed_tail:
            raise ValueError("Forge screenshot PNG expands beyond its declared geometry")
    except zlib.error as error:
        raise ValueError(f"Forge screenshot PNG data is invalid: {error}") from error
    if len(filtered) != expected_filtered_bytes or not inflater.eof or inflater.unused_data:
        raise ValueError("Forge screenshot PNG scanline size is incoherent")
    previous = bytearray(stride)
    colors: set[bytes] = set()
    luminance_min = 255
    luminance_max = 0
    cursor = 0
    for _row_index in range(height):
        filter_type = filtered[cursor]
        cursor += 1
        encoded = filtered[cursor:cursor + stride]
        cursor += stride
        row = bytearray(stride)
        for index, value in enumerate(encoded):
            left = row[index - channels] if index >= channels else 0
            up = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = up
            elif filter_type == 3:
                predictor = (left + up) // 2
            elif filter_type == 4:
                p = left + up - upper_left
                distances = (abs(p - left), abs(p - up), abs(p - upper_left))
                predictor = (left, up, upper_left)[distances.index(min(distances))]
            else:
                raise ValueError("Forge screenshot uses an invalid PNG filter")
            row[index] = (value + predictor) & 0xff
        for index in range(0, stride, channels):
            rgb = bytes(row[index:index + 3])
            if len(colors) < 4096:
                colors.add(rgb)
            luminance = (int(rgb[0]) + int(rgb[1]) + int(rgb[2])) // 3
            luminance_min = min(luminance_min, luminance)
            luminance_max = max(luminance_max, luminance)
        previous = row
    if len(colors) < 16 or luminance_max - luminance_min < 20:
        raise ValueError("Forge screenshot is blank or lacks a credible UI content range")
    return {
        "width": width,
        "height": height,
        "decoded_pixel_count": width * height,
        "distinct_rgb_lower_bound": len(colors),
        "luminance_range": luminance_max - luminance_min,
    }


def _verify_v2_metadata(root: Path, receipt: dict[str, Any], errors: list[str]) -> None:
    source = _mapping(receipt.get("source_identity"), "source_identity", errors)
    if source.get("repository") != "Generous-Corp/pulp" or source.get("clean") is not True:
        errors.append("v2 source identity is not a clean canonical Pulp checkout")
    if source.get("revision") != receipt.get("integration_head"):
        errors.append("v2 source identity differs from integration_head")
    if not SHA256.fullmatch(str(source.get("status_sha256", ""))):
        errors.append("v2 source identity lacks the clean-status digest")
    plan = _mapping(receipt.get("accepted_plan"), "accepted_plan", errors)
    expected_plan = {
        "repository": "danielraffel/pulp-planning",
        "revision": EXPECTED_PLAN_REVISION,
        "path": "research/2026-08-27-vgpu-gpu-ux-inspiration-audit-and-plan.md",
        "blob": EXPECTED_PLAN_BLOB,
        "sha256": EXPECTED_PLAN_SHA256,
    }
    for key, value in expected_plan.items():
        if plan.get(key) != value:
            errors.append(f"v2 accepted plan {key} differs from the canonical plan")
    install = _mapping(receipt.get("install_provenance"), "install_provenance", errors)
    build_info = _mapping(install.get("build_info"), "install build_info", errors)
    revision = str(receipt.get("integration_head", ""))
    stamped = build_info.get("kGitSha")
    if (
        install.get("cmake_home_revision") != revision
        or install.get("cmake_build_type") != "Release"
        or install.get("rust_profile") != "release"
        or build_info.get("kBuildType") != "Release"
        or build_info.get("kGitDirty") is not False
        or not isinstance(stamped, str)
        or SOURCE_STAMP.fullmatch(stamped) is None
        or not revision.startswith(stamped)
    ):
        errors.append("v2 installed CLI provenance is not the exact clean Release source")
    if install.get("build_install_binary_identity") != "pass":
        errors.append("v2 installed binaries were not byte-identical to refreshed build outputs")
    for field in ("cmake_cache_sha256", "build_info_sha256"):
        if not SHA256.fullmatch(str(install.get(field, ""))):
            errors.append(f"v2 installed provenance lacks exact {field}")
    features = _mapping(install.get("feature_contract"), "install feature_contract", errors)
    expected_features = {
        "PULP_ENABLE_GPU": "ON", "PULP_ENABLE_SCENE3D": "ON",
        "PULP_ENABLE_THREEJS_RUNTIME": "ON", "PULP_ENABLE_JS": "ON",
        "PULP_JS_ENGINE": "v8", "PULP_BUILD_RUST_CLI": "ON",
        "PULP_HAS_THREEJS": "TRUE",
    }
    if features != expected_features:
        errors.append("v2 installed build lacks the exact all-four feature contract")
    binaries = receipt.get("binaries")
    if isinstance(binaries, dict):
        for role, binary in binaries.items():
            if isinstance(binary, dict) and binary.get("build_output_sha256") != binary.get("sha256"):
                errors.append(f"v2 binary {role} differs from its refreshed build output")
        rust = binaries.get("installed_rust_cli")
        cpp = binaries.get("installed_cpp_delegate")
        if (
            isinstance(rust, dict) and isinstance(cpp, dict)
            and rust.get("sha256") == cpp.get("sha256")
        ):
            errors.append("v2 installed pulp is a C++ copy rather than the Rust front")
    forge = _mapping(receipt.get("forge_downstream"), "forge_downstream", errors)
    if forge.get("repository") != "Generous-Corp/forge" or forge.get("revision") != EXPECTED_FORGE_REVISION:
        errors.append("v2 Forge proof does not name the exact accepted Forge revision")
    overlay = _mapping(forge.get("pulp_sdk_ref_overlay"), "Forge PULP_SDK_REF overlay", errors)
    if overlay.get("path") != "PULP_SDK_REF" or overlay.get("content") != revision:
        errors.append("v2 Forge proof lacks the exact one-file Pulp SDK overlay")
    if overlay.get("original_blob") != EXPECTED_FORGE_PULP_REF_BLOB:
        errors.append("v2 Forge proof lacks the accepted original PULP_SDK_REF blob")
    if forge.get("all_other_tracked_files_clean") is not True:
        errors.append("v2 Forge proof has source drift beyond PULP_SDK_REF")
    stamp = _mapping(forge.get("bundle_build_info"), "Forge bundle build info", errors)
    if (
        stamp.get("product") != "Forge Modular"
        or stamp.get("schema") != "1"
        or not str(stamp.get("version", "")).strip()
        or not str(stamp.get("product_id", "")).strip()
        or not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", str(stamp.get("packaged", "")))
        or stamp.get("role") != "Rack module and patch generator"
        or stamp.get("format") != "Standalone application"
        or "Release" not in str(stamp.get("build", ""))
        or revision[:12] not in str(stamp.get("pulp_sdk", ""))
        or stamp.get("expected_pulp_sdk_ref") not in {None, ""}
    ):
        errors.append("v2 Forge bundle stamp does not bind the Modular shell to this Pulp SDK")
    if forge.get("codesign_verify") != "pass" or forge.get("build_target") != "ForgeModular_Standalone":
        errors.append("v2 Forge standalone was not rebuilt and signature-verified")
    for field in ("cmake_cache_sha256", "bundle_build_info_sha256", "bundle_binary_sha256"):
        if not SHA256.fullmatch(str(forge.get(field, ""))):
            errors.append(f"v2 Forge proof lacks exact {field}")
    screenshot_path = root / "forge-modular-screenshot.png"
    try:
        metrics = _png_metrics(screenshot_path)
    except (OSError, ValueError) as error:
        errors.append(f"Forge screenshot content proof failed: {error}")
    else:
        if metrics != forge.get("screenshot_metrics"):
            errors.append("Forge screenshot metrics differ from decoded PNG content")
    try:
        doctor_path = root / "forge-gpu-doctor.json"
        if doctor_path.is_symlink():
            raise ValueError("Forge GPU doctor must be an in-receipt regular file")
        doctor_payload = _load(doctor_path)
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f"cannot parse Forge GPU doctor evidence: {error}")
    except ValueError as error:
        errors.append(str(error))
    else:
        doctor = _mapping(doctor_payload, "Forge GPU doctor evidence", errors)
        try:
            health_schema = _load(HEALTH_SCHEMA)
        except (OSError, json.JSONDecodeError) as error:
            errors.append(f"cannot read GPU health schema: {error}")
        else:
            for problem in json_schema_lite.validate(doctor_payload, health_schema):
                errors.append(f"Forge GPU doctor schema: {problem}")
        if (
            doctor.get("schema") != "pulp.gpu-health-result.v1"
            or doctor.get("verdict") != "pass"
            or doctor.get("health_state") != "healthy"
            or doctor.get("recommendations") != []
        ):
            errors.append("Forge-cwd GPU doctor did not return a healthy typed pass")
        probes = doctor.get("probes")
        if not isinstance(probes, list) or not probes or any(
            not isinstance(probe, dict) or probe.get("verdict") != "pass"
            for probe in probes
        ):
            errors.append("Forge-cwd GPU doctor lacks all-passing real probes")
        else:
            required_probes = [probe for probe in probes if probe.get("required") is True]
            if not required_probes:
                errors.append("Forge-cwd GPU doctor lacks a required real probe")
            elif any(
                not isinstance(probe.get("adapter"), dict)
                or probe["adapter"].get("status") != "authentic"
                or probe["adapter"].get("class") != "hardware"
                or "Metal" not in (
                    str(probe["adapter"].get("backend", ""))
                    + str(probe["adapter"].get("architecture", ""))
                )
                for probe in required_probes
            ):
                errors.append("Forge-cwd GPU doctor did not use authentic Metal hardware")
            else:
                measurements = [
                    value if isinstance(value, dict) else {}
                    for value in (probe.get("measurements") for probe in required_probes)
                ]
                for field in (
                    "command_submitted", "readback_completed", "pixel_output_produced",
                    "content_floor_passed", "compute_initialized", "compute_oracle_passed",
                ):
                    if not any(value.get(field) is True for value in measurements):
                        errors.append(f"Forge-cwd GPU doctor did not prove {field}")
    expected_canaries = {
        "gpu_audio": {
            "status": "pass", "recipe": GROUPS["stft"],
            "cli_positive_files": ["stft-run1.json", "stft-run2.json"],
            "cli_negative_file": "stft-negative.json",
            "mcp_positive_response_id": 4, "mcp_negative_response_id": 5,
        },
        "threejs": {
            "status": "pass", "recipe": GROUPS["threejs"],
            "cli_positive_files": ["threejs-run1.json", "threejs-run2.json"],
            "cli_negative_file": "threejs-negative.json",
            "mcp_positive_response_id": 8, "mcp_negative_response_id": 9,
        },
    }
    if "missing_path_canaries" in forge:
        errors.append("Pulp path canaries must not be claimed as Forge downstream evidence")
    if receipt.get("additional_pulp_path_canaries") != expected_canaries:
        errors.append(
            "A2 proof does not bind the additional Pulp Three.js/GPU-audio paths "
            "to their executed CLI/MCP evidence"
        )
    acceptance = _mapping(receipt.get("acceptance"), "acceptance", errors)
    expected_acceptance = {
        "terminal_status": "pass", "all_four_installed_cli": "pass",
        "all_four_installed_mcp": "pass", "seeded_negative_controls": "pass",
        "forge_modular_and_additional_pulp_path_canaries": "pass",
    }
    if acceptance != expected_acceptance:
        errors.append("v2 terminal acceptance fields are incomplete or non-passing")


def verify(root: Path) -> list[str]:
    errors: list[str] = []
    try:
        result_schema = _load(RESULT_SCHEMA)
    except (OSError, json.JSONDecodeError) as error:
        return [f"cannot read GPU probe result schema: {error}"]
    receipt_path = root / "receipt.json"
    try:
        receipt = _load(receipt_path)
    except (OSError, json.JSONDecodeError) as error:
        return [f"cannot read receipt.json: {error}"]
    if not isinstance(receipt, dict):
        return ["receipt.json must contain an object"]

    schema = receipt.get("schema")
    v2 = schema == "pulp.gpu-probe-acceptance-receipt.v2"
    if schema not in {
        "pulp.gpu-probe-acceptance-receipt.v1",
        "pulp.gpu-probe-acceptance-receipt.v2",
    }:
        errors.append("receipt schema mismatch")
    integration_head = str(receipt.get("integration_head", ""))
    if not GIT_SHA.fullmatch(integration_head):
        errors.append("integration_head must be an exact Git SHA")
    source_blobs = _mapping(receipt.get("source_blobs"), "source_blobs", errors)
    expected_source_blobs = EXPECTED_SOURCE_BLOBS_V2 if v2 else EXPECTED_SOURCE_BLOBS
    if set(source_blobs) != expected_source_blobs:
        errors.append("source_blobs does not bind the exact recipe source set")
    historical_blobs = (
        _git_blobs(integration_head, expected_source_blobs)
        if GIT_SHA.fullmatch(integration_head)
        else {}
    )
    head_blobs = _git_blobs("HEAD", expected_source_blobs)
    checkout_blobs = _checkout_blobs(expected_source_blobs)
    for path, declared_blob in source_blobs.items():
        if path not in expected_source_blobs:
            continue
        if not GIT_SHA.fullmatch(str(declared_blob)):
            errors.append(f"source blob {path} must be an exact Git blob SHA")
            continue
        observed_blob = historical_blobs.get(path)
        if observed_blob is None:
            errors.append(f"cannot resolve {path} at integration_head")
        elif observed_blob != declared_blob:
            errors.append(f"source blob mismatch for {path}")
        head_blob = head_blobs.get(path)
        if head_blob is None:
            errors.append(f"cannot resolve {path} at current HEAD")
        elif head_blob != declared_blob:
            errors.append(f"current HEAD source blob drift for {path}")
        checkout_blob = checkout_blobs.get(path)
        if checkout_blob is None:
            errors.append(f"cannot hash {path} in current checkout")
        elif checkout_blob != declared_blob:
            errors.append(f"current checkout source blob drift for {path}")
    context = _mapping(receipt.get("execution_context"), "execution_context", errors)
    if context.get("cwd_role") != "fresh-temporary-directory-outside-any-checkout":
        errors.append("installed fronts were not recorded outside every checkout")
    if context.get("path") != "/usr/bin:/bin:/usr/sbin:/sbin":
        errors.append("installed fronts did not use the bounded system-only PATH")
    binaries = _mapping(receipt.get("binaries"), "binaries", errors)
    expected_binaries = EXPECTED_BINARIES_V2 if v2 else EXPECTED_BINARIES
    if set(binaries) != expected_binaries:
        errors.append("binaries does not bind the exact installed executable set")
    for role, value in binaries.items():
        digest = value.get("sha256") if v2 and isinstance(value, dict) else value
        if not SHA256.fullmatch(str(digest)):
            errors.append(f"binary {role} lacks a SHA-256 digest")
        if v2 and (not isinstance(value, dict) or not isinstance(value.get("bytes"), int)
                   or value.get("bytes", 0) <= 0):
            errors.append(f"binary {role} lacks a positive byte count")

    declared = _mapping(receipt.get("raw_sha256"), "raw_sha256", errors)
    expected_files = {
        *(f"{group}-{suffix}.json" for group in GROUPS for suffix in ("run1", "run2", "negative")),
        "mcp-transcript.jsonl",
    }
    if v2:
        expected_files |= {"forge-modular-screenshot.png", "forge-gpu-doctor.json"}
    if set(declared) != expected_files:
        errors.append(
            f"raw_sha256 does not name the exact {len(expected_files)}-file evidence set"
        )
    for name in sorted(expected_files):
        digest = declared.get(name)
        path = root / name
        try:
            limit = 8 * 1024 * 1024 if name in {
                "mcp-transcript.jsonl", "forge-modular-screenshot.png"
            } else 1024 * 1024
            if path.is_symlink() or not path.is_file():
                errors.append(f"{name} must be a regular in-receipt evidence file")
                continue
            if path.stat().st_size > limit:
                errors.append(f"{name} exceeds its {limit}-byte evidence cap")
                continue
            observed = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError as error:
            errors.append(f"cannot read {name}: {error}")
            continue
        if observed != digest:
            errors.append(f"raw digest mismatch for {name}")

    run_groups = _mapping(receipt.get("run_groups"), "run_groups", errors)
    if set(run_groups) != set(GROUPS):
        errors.append("receipt does not bind the exact four run groups")
    seen_evidence_ids: set[str] = set()
    for group, recipe in GROUPS.items():
        run_group = run_groups.get(group, {})
        if not isinstance(run_group, dict):
            errors.append(f"receipt run group {group} must be an object")
            run_group = {}
        if run_group.get("recipe") != recipe:
            errors.append(f"receipt run group {group} does not bind recipe {recipe}")
        role = run_group.get("binary_role")
        expected_roles = EXPECTED_BINARY_ROLES_V2 if v2 else EXPECTED_BINARY_ROLES
        if role != expected_roles[group] or role not in binaries:
            errors.append(f"receipt run group {group} has the wrong executable role")
        runs: dict[str, dict[str, Any]] = {}
        for suffix in ("run1", "run2", "negative"):
            path = root / f"{group}-{suffix}.json"
            try:
                result_payload = _load(path)
            except (OSError, json.JSONDecodeError) as error:
                errors.append(f"cannot parse {path.name}: {error}")
                continue
            for problem in json_schema_lite.validate(result_payload, result_schema):
                errors.append(f"{path.name}: schema: {problem}")
            if not isinstance(result_payload, dict):
                errors.append(f"{path.name}: probe result must be an object")
                continue
            result = result_payload
            runs[suffix] = result
            evidence_id = result.get("gpu_evidence_id")
            if not GPU_EVIDENCE_ID.fullmatch(str(evidence_id)):
                errors.append(f"{path.name}: gpu_evidence_id must be exact 32-hex")
            elif evidence_id in seen_evidence_ids:
                errors.append(f"{path.name}: gpu_evidence_id was reused by another A2 run")
            else:
                seen_evidence_ids.add(evidence_id)
            if result.get("recipe_id") != recipe:
                errors.append(f"{path.name}: recipe mismatch")
            expected_dimensions = (
                RENDERER_NEGATIVE_DIMENSIONS
                if group == "renderer" and suffix == "negative"
                else EXPECTED_DIMENSIONS[group]
            )
            if result.get("dimensions") != expected_dimensions:
                errors.append(f"{path.name}: execution dimensions are not recipe-bound")
            passes = result.get("passes")
            if (
                not isinstance(passes, list) or not passes
                or not all(isinstance(item, dict) and item.get("work_completed") for item in passes)
            ):
                errors.append(f"{path.name}: work was not proven complete")
            artifacts_value = result.get("artifacts")
            artifacts = (
                artifacts_value
                if isinstance(artifacts_value, list)
                and all(isinstance(item, dict) for item in artifacts_value)
                else []
            )
            artifact_bytes = [item.get("bytes") for item in artifacts]
            if (
                not artifacts
                or any(type(value) is not int or value < 0 for value in artifact_bytes)
                or sum(artifact_bytes) > 512 * 1024
            ):
                errors.append(f"{path.name}: artifacts are absent or exceed 512 KiB")
            for artifact in artifacts:
                if not SHA256.fullmatch(str(artifact.get("sha256", ""))):
                    errors.append(f"{path.name}: artifact lacks SHA-256")
            if group == "renderer":
                rgba = [artifact for artifact in artifacts
                        if artifact.get("name") == "observed.rgba8"]
                expected_rgba_bytes = expected_dimensions["work_items"] * 4
                if len(rgba) != 1 or rgba[0].get("kind") != "image" or \
                        rgba[0].get("mime") != "application/octet-stream" or \
                        rgba[0].get("bytes") != expected_rgba_bytes:
                    errors.append(
                        f"{path.name}: observed RGBA artifact does not match declared dimensions"
                    )
            if result.get("numeric_sample_count") != EXPECTED_SAMPLE_COUNTS[group]:
                errors.append(f"{path.name}: numeric sample count changed")
            adapter_value = result.get("adapter")
            adapter = adapter_value if isinstance(adapter_value, dict) else {}
            if group in HARDWARE_REQUIRED and (
                adapter.get("status") != "authentic"
                or adapter.get("class") != "hardware"
                or adapter.get("backend") != "Metal"
            ):
                errors.append(f"{path.name}: hardware-required adapter claim is not authentic Metal")
            if group == "renderer" and (
                adapter.get("status") != "unverified"
                or adapter.get("class") != "unknown"
                or "Metal" not in str(adapter.get("name", ""))
            ):
                errors.append(f"{path.name}: renderer adapter truthfulness changed")

        if set(runs) != {"run1", "run2", "negative"}:
            continue
        if runs["run1"].get("verdict") != "pass" or runs["run2"].get("verdict") != "pass":
            errors.append(f"{group}: both positive runs must pass")
        for label in ("run1", "run2"):
            passes = runs[label].get("passes")
            if not isinstance(passes, list) or not all(
                isinstance(item, dict) and item.get("verdict") == "pass"
                for item in passes
            ):
                errors.append(f"{group}: {label} contains a non-passing semantic pass")
        if _canonical_repeat(runs["run1"]) != _canonical_repeat(runs["run2"]):
            errors.append(f"{group}: positive rerun is not deterministic")
        negative = runs["negative"]
        if negative.get("verdict") != "fail" or not negative.get("mutation"):
            errors.append(f"{group}: seeded negative control did not fail")
        if group == "renderer" and negative.get("mutation") != RENDERER_NEGATIVE_MUTATION:
            errors.append("renderer: negative mutation is not the exact pre-submit mutation")
        negative_passes = negative.get("passes")
        if not isinstance(negative_passes, list) or not any(
            isinstance(item, dict)
            and item.get("verdict") == "fail"
            and item.get("work_completed")
            for item in negative_passes
        ):
            errors.append(f"{group}: negative control lacks a completed causal failure")

    try:
        transcript_path = root / "mcp-transcript.jsonl"
        if transcript_path.is_symlink() or transcript_path.stat().st_size > 8 * 1024 * 1024:
            raise ValueError("MCP transcript is not a bounded regular evidence file")
        transcript = [json.loads(line) for line in transcript_path.read_text().splitlines()]
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f"cannot parse MCP transcript: {error}")
        transcript = []
    except ValueError as error:
        errors.append(str(error))
        transcript = []
    expected_transcript_length = 1 + (2 * len(GROUPS) if v2 else 2)
    if (
        len(transcript) != expected_transcript_length
        or not all(isinstance(row, dict) for row in transcript)
        or [row.get("id") for row in transcript] != list(
            range(1, expected_transcript_length + 1)
        )
    ):
        errors.append(
            "MCP transcript must contain initialize plus all-four positive/negative responses"
            if v2 else
            "MCP transcript must contain initialize, positive, and negative responses"
        )
    else:
        if any(row.get("jsonrpc") != "2.0" for row in transcript):
            errors.append("MCP transcript contains a non-JSON-RPC response")
        initialize_result = transcript[0].get("result")
        if (
            not isinstance(initialize_result, dict)
            or initialize_result.get("protocolVersion") != "2024-11-05"
        ):
            errors.append("MCP transcript lacks the accepted initialize response")
        mcp_evidence: dict[str, Any] = {}
        pairs = []
        if v2:
            for index, group in enumerate(GROUPS):
                pairs.extend(((f"{group}-positive", transcript[1 + 2 * index]),
                              (f"{group}-negative", transcript[2 + 2 * index])))
        else:
            pairs = [("positive", transcript[1]), ("negative", transcript[2])]
        for label, row in pairs:
            result_value = row.get("result")
            result = result_value if isinstance(result_value, dict) else {}
            positive = label.endswith("positive") or label == "positive"
            expected_exit = 0 if positive else 1
            if bool(result.get("isError", False)) != (not positive):
                if not v2 and positive:
                    errors.append("MCP positive result must omit isError or preserve isError=false")
                elif not v2:
                    errors.append("MCP completed failure did not preserve isError=true")
                else:
                    errors.append(f"MCP {label} did not preserve typed isError status")
            structured_value = result.get("structuredContent")
            structured = structured_value if isinstance(structured_value, dict) else {}
            if structured.get("exit_code") != expected_exit:
                errors.append(f"MCP {label} did not preserve exit {expected_exit}")
            evidence = structured.get("evidence")
            for problem in json_schema_lite.validate(evidence, result_schema):
                errors.append(f"MCP {label} evidence schema: {problem}")
            if isinstance(evidence, dict):
                mcp_evidence[label] = evidence
            content = result.get("content")
            try:
                if not isinstance(content, list):
                    raise TypeError("MCP content is not an array")
                text_evidence = json.loads(content[0]["text"])
            except (IndexError, KeyError, TypeError, json.JSONDecodeError):
                errors.append(f"MCP {label} text evidence is missing or malformed")
            else:
                if text_evidence != evidence:
                    errors.append(f"MCP {label} text and structured evidence disagree")
        comparisons = (
            [(f"{group}-positive", f"{group}-run1.json") for group in GROUPS]
            + [(f"{group}-negative", f"{group}-negative.json") for group in GROUPS]
            if v2 else
            [("positive", "compute-run1.json"), ("negative", "compute-negative.json")]
        )
        for label, raw_name in comparisons:
            if label not in mcp_evidence:
                continue
            try:
                raw = _load(root / raw_name)
            except (OSError, json.JSONDecodeError):
                continue
            if not isinstance(raw, dict):
                errors.append(f"{raw_name}: probe result must be an object")
                continue
            if _canonical_repeat(mcp_evidence[label]) != _canonical_repeat(raw):
                errors.append(f"MCP {label} evidence is not the installed CLI recipe result")
    if v2:
        _verify_v2_metadata(root, receipt, errors)

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt_dir", type=Path)
    args = parser.parse_args(argv)
    errors = verify(args.receipt_dir)
    if errors:
        for error in errors:
            print(f"gpu-probe-acceptance: FAIL: {error}", file=sys.stderr)
        return 1
    schema = _load(args.receipt_dir / "receipt.json").get("schema")
    if schema == "pulp.gpu-probe-acceptance-receipt.v2":
        print("gpu-probe-acceptance: ok (terminal v2; installed all-four CLI/MCP parity)")
    else:
        print("gpu-probe-acceptance: ok (historical v1 integrity; nonterminal)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
