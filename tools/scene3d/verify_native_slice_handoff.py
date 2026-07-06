#!/usr/bin/env python3
"""Verify native glTF/bake first-slice handoff artifacts stay wired."""

import argparse
import re
import sys
from pathlib import Path


REQUIRED_ARTIFACTS = [
    "core/scene/CMakeLists.txt",
    "core/scene/include/pulp/scene/scene_data.hpp",
    "core/scene/src/gltf_loader.cpp",
    "core/scene/src/bake_preflight.cpp",
    "core/render/include/pulp/render/renderer3d.hpp",
    "core/render/src/renderer3d.cpp",
    "core/render/src/renderer3d_probe.cpp",
    "test/fixtures/scene3d/BoxTextured/BoxTextured.glb",
    "test/fixtures/scene3d/renderer3d-goldens.json",
    "tools/scene3d/validate_renderer_golden_manifest.py",
    "tools/scene3d/verify_renderer_probe.py",
    "tools/scene3d/verify_renderer_probe_fake_surfaces.py",
    "tools/scene3d/verify_renderer_probe_final_eligibility.py",
    "tools/scene3d/verify_renderer_probe_manifest_schema.py",
    "tools/scene3d/verify_renderer_probe_public_surface.py",
    "tools/scene3d/verify_native_source_boundary.py",
    "tools/scene3d/verify_native_renderer_boundary.py",
    "tools/scene3d/verify_renderer_probe_route_inventory.py",
    "tools/scene3d/verify_bake_preflight_matrix.py",
    "tools/scene3d/verify_bake_artifact.py",
]

REQUIRED_TESTS = [
    "scene3d-cmake-boundary",
    "scene3d-public-boundary",
    "scene3d-native-source-boundary",
    "scene3d-native-renderer-link-boundary",
    "scene3d-renderer-probe-hardcoded",
    "scene3d-renderer-probe-boxtextured",
    "scene3d-renderer-probe-public-surface-contract",
    "scene3d-renderer-probe-public-surface-negative-contract",
    "scene3d-renderer-probe-fake-surface-contract",
    "scene3d-renderer-probe-fake-surface-negative-contract",
    "scene3d-renderer-probe-manifest-schema-contract",
    "scene3d-renderer-probe-manifest-schema-negative-contract",
    "scene3d-renderer-probe-cli-surface-contract",
    "scene3d-renderer-probe-route-inventory-contract",
    "scene3d-renderer-probe-final-eligibility-contract",
    "scene3d-renderer-probe-final-eligibility-negative-contract",
    "scene3d-renderer-probe-final-software-gate",
    "scene3d-bake-preflight-matrix-contract",
    "scene3d-verify-bake-artifact-clean",
    "scene3d-sidecar-schema-constants-contract",
    "scene3d-boxtextured-fixture-contract",
]

REQUIRED_DOC_TOKENS = [
    "https://github.com/danielraffel/pulp/issues/2738",
    "https://github.com/danielraffel/pulp/issues/3369",
    "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927",
    "Native hardcoded textured-cube renderer proof",
    "Native `BoxTextured.glb` rendering",
    "scene3d-native-source-boundary-negative-contract",
    "scene3d-renderer-probe-route-inventory-contract",
    "scene3d-renderer-probe-public-surface-contract",
    "scene3d-renderer-probe-manifest-schema-contract",
    "scene3d-renderer-probe-final-software-gate",
    "final_software_golden_eligible=false",
    "scene3d-bake-preflight-matrix-contract",
    "runtime_evidence_url_invalid",
    "scene3d-renderer-probe-hardcoded",
    "scene3d-renderer-probe-boxtextured",
]

REQUIRED_CTEST_TOKENS = [
    "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927",
]

REQUIRED_PLAN_TOKENS = [
    "Parallel Worktree Contract",
    "Live `GLTFLoader + BoxTextured.glb` is a Runtime-hardening P0 item.",
    "https://github.com/danielraffel/pulp/issues/3369",
    "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927",
    "Native `BoxTextured.glb` rendering does not wait on live `GLTFLoader`",
    "Phase 5 in this native branch is the bake/export design and preflight contract",
    "not live `GLTFExporter` or web-compat implementation",
]

FORBIDDEN_PLAN_TOKENS = [
    "Treat in-bridge GLB loading as a risk-reduction spike (Phase 4, first task)",
    '"in-bridge JS loaders are unverified; spike first"',
    "**Spike A — Live GLB loads at all",
    "Resolve `GLTFLoader` in the demo module loader",
    "a `gltf` demo mode in `examples/threejs-native-demo`",
    "Wire `GLTFExporter` in the Live lane to emit GLB",
    "confirm/enable texture encoding (`toDataURL`/`toBlob` native path)",
    "add a native PNG/JPEG encode path behind `toDataURL`/`toBlob`",
]

REQUIRED_FILE_TOKENS = {
    "test/fixtures/scene3d/sidecars/clean.pulp3d.json": [
        '"runtime_evidence": "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927"',
    ],
}


def read_text(path: Path):
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(f"{path}: {exc}") from exc


def main():
    parser = argparse.ArgumentParser(
        description="Verify the native glTF/bake first-slice handoff surface.")
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--ctest-file", type=Path, action="append", required=True)
    parser.add_argument("--doc-file", type=Path, required=True)
    parser.add_argument("--plan-file", type=Path, required=True)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    ctest_text = "\n".join(read_text(path) for path in args.ctest_file)
    doc_text = read_text(args.doc_file)
    plan_text = read_text(args.plan_file)
    errors = []

    for relative in REQUIRED_ARTIFACTS:
        path = repo_root / relative
        if not path.exists():
            errors.append(f"missing native slice artifact: {relative}")

    for test_name in REQUIRED_TESTS:
        test_pattern = re.compile(
            rf"add_test\s*\(\s*NAME\s+{re.escape(test_name)}(?=\s|\))")
        if not test_pattern.search(ctest_text):
            errors.append(f"missing native slice CTest registration: {test_name}")

    for token in REQUIRED_DOC_TOKENS:
        if token not in doc_text:
            errors.append(f"missing native slice doc token: {token}")

    for token in REQUIRED_CTEST_TOKENS:
        if token not in ctest_text:
            errors.append(f"missing native slice CTest token: {token}")

    for token in REQUIRED_PLAN_TOKENS:
        if token not in plan_text:
            errors.append(f"missing native slice plan token: {token}")

    for token in FORBIDDEN_PLAN_TOKENS:
        if token in plan_text:
            errors.append(f"forbidden native slice plan token: {token}")

    for relative, tokens in REQUIRED_FILE_TOKENS.items():
        try:
            text = read_text(repo_root / relative)
        except RuntimeError as exc:
            errors.append(str(exc))
            continue
        for token in tokens:
            if token not in text:
                errors.append(
                    f"missing native slice file token: {relative}: {token}")

    if "core/view" in "\n".join(REQUIRED_ARTIFACTS):
        errors.append("native slice handoff artifact list crossed into core/view")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(
        "native_slice_handoff_verified="
        f"{len(REQUIRED_ARTIFACTS)} artifacts "
        f"{len(REQUIRED_TESTS)} tests "
        f"{len(REQUIRED_DOC_TOKENS)} doc tokens "
        f"{len(REQUIRED_CTEST_TOKENS)} ctest tokens "
        f"{len(REQUIRED_PLAN_TOKENS)} plan tokens "
        f"{len(FORBIDDEN_PLAN_TOKENS)} forbidden plan tokens "
        f"{sum(len(tokens) for tokens in REQUIRED_FILE_TOKENS.values())} file tokens")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
