#!/usr/bin/env python3
"""Verify native slice handoff checker rejects durable-artifact drift."""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def read_text(path):
    return path.read_text(encoding="utf-8")


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def copy_required_repo_bits(source_root, target_root):
    for relative in (
            "core/scene",
            "core/render/include/pulp/render/renderer3d.hpp",
            "core/render/src/renderer3d.cpp",
            "core/render/src/renderer3d_probe.cpp",
            "test/fixtures/scene3d/BoxTextured",
            "test/fixtures/scene3d/renderer3d-goldens.json",
            "test/fixtures/scene3d/sidecars",
            "planning/threejs-webgpu-gltf-bake-plan.md",
            "tools/scene3d"):
        src = source_root / relative
        dst = target_root / relative
        if src.is_dir():
            shutil.copytree(src, dst)
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)


def run_verifier(verifier, repo_root, ctest_files, doc_file):
    cmd = [
        sys.executable,
        str(verifier),
        "--repo-root",
        str(repo_root),
    ]
    for ctest_file in ctest_files:
        cmd.extend(["--ctest-file", str(ctest_file)])
    cmd.extend([
        "--doc-file",
        str(doc_file),
        "--plan-file",
        str(repo_root / "planning/threejs-webgpu-gltf-bake-plan.md"),
    ])
    return subprocess.run(
        cmd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def remove_artifact(repo_root):
    (repo_root / "core/render/src/renderer3d_probe.cpp").unlink()


def remove_ctest(ctest_file, test_name):
    text = read_text(ctest_file)
    needle = f"add_test(NAME {test_name}"
    if needle not in text:
        raise ValueError(f"missing CTest token {needle}")
    write_text(ctest_file, text.replace(needle, f"add_test(NAME drift-{test_name}", 1))


def combine_ctest_files(source_files, ctest_file):
    text = "\n".join(read_text(path) for path in source_files)
    write_text(ctest_file, text)


def remove_doc_token(doc_file, token):
    text = read_text(doc_file)
    if token not in text:
        raise ValueError(f"missing doc token {token}")
    write_text(doc_file, text.replace(token, "drift-token"))


def add_plan_token(plan_file, token):
    text = read_text(plan_file)
    write_text(plan_file, text + "\n" + token + "\n")


def expect_case(name, result, expected_code, expected_text, errors):
    sys.stdout.write(result.stdout)
    if result.returncode != expected_code:
        errors.append(
            f"{name}: expected exit {expected_code}, got {result.returncode}")
    if expected_text not in result.stdout:
        errors.append(
            f"{name}: expected diagnostic containing {expected_text!r}")
    print(f"native_slice_handoff_contract_case={name}")


def main():
    parser = argparse.ArgumentParser(
        description="Verify native slice handoff drift is rejected.")
    parser.add_argument("--handoff-verifier", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--ctest-file", type=Path, action="append", required=True)
    parser.add_argument("--doc-file", type=Path, required=True)
    args = parser.parse_args()

    for label, path in (
            ("handoff_verifier", args.handoff_verifier),
            ("repo_root", args.repo_root),
            ("doc_file", args.doc_file)):
        if not path.exists():
            print(f"{label}_exists=false path={path}")
            return 2
    for path in args.ctest_file:
        if not path.exists():
            print(f"ctest_file_exists=false path={path}")
            return 2

    errors = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp_root = Path(tmp)

        cases = [
            ("valid-current-handoff", None, 0,
             "native_slice_handoff_verified=20 artifacts 21 tests 15 doc tokens 1 ctest tokens 7 plan tokens 8 forbidden plan tokens 1 file tokens"),
            ("missing-renderer-probe-artifact", "artifact", 1,
             "missing native slice artifact: core/render/src/renderer3d_probe.cpp"),
            ("missing-boxtextured-ctest", "ctest", 1,
             "missing native slice CTest registration: scene3d-renderer-probe-boxtextured"),
            ("missing-runtime-issue-link", "runtime-issue", 1,
             "missing native slice doc token: https://github.com/danielraffel/pulp/issues/3369"),
            ("missing-runtime-evidence-comment-link", "runtime-comment", 1,
             "missing native slice doc token: https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927"),
            ("missing-runtime-evidence-ctest-link", "runtime-ctest", 1,
             "missing native slice CTest token: https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927"),
            ("missing-clean-sidecar-runtime-evidence-link", "runtime-fixture", 1,
             "missing native slice file token: test/fixtures/scene3d/sidecars/clean.pulp3d.json"),
            ("missing-plan-runtime-boundary", "plan-boundary", 1,
             "missing native slice plan token: Live `GLTFLoader + BoxTextured.glb` is a Runtime-hardening P0 item."),
            ("stale-plan-live-gltf-spike", "plan-forbidden", 1,
             "forbidden native slice plan token: Treat in-bridge GLB loading as a risk-reduction spike"),
            ("stale-plan-live-exporter-implementation", "plan-exporter-forbidden", 1,
             "forbidden native slice plan token: Wire `GLTFExporter` in the Live lane to emit GLB"),
            ("missing-url-gate-doc", "url-gate", 1,
             "missing native slice doc token: runtime_evidence_url_invalid"),
            ("missing-final-gate-doc", "final-gate", 1,
             "missing native slice doc token: scene3d-renderer-probe-final-software-gate"),
        ]

        for name, drift, expected_code, expected_text in cases:
            case_root = tmp_root / name
            copy_required_repo_bits(args.repo_root, case_root)
            ctest_file = case_root / "test/CMakeLists.txt"
            doc_file = case_root / "docs/analysis/threejs-gltf-bake-native-slice.md"
            ctest_file.parent.mkdir(parents=True, exist_ok=True)
            doc_file.parent.mkdir(parents=True, exist_ok=True)
            combine_ctest_files(args.ctest_file, ctest_file)
            shutil.copy2(args.doc_file, doc_file)

            if drift == "artifact":
                remove_artifact(case_root)
            elif drift == "ctest":
                remove_ctest(ctest_file, "scene3d-renderer-probe-boxtextured")
            elif drift == "runtime-issue":
                remove_doc_token(
                    doc_file, "https://github.com/danielraffel/pulp/issues/3369")
            elif drift == "runtime-comment":
                remove_doc_token(
                    doc_file,
                    "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927")
            elif drift == "runtime-ctest":
                remove_doc_token(
                    ctest_file,
                    "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927")
            elif drift == "runtime-fixture":
                remove_doc_token(
                    case_root / "test/fixtures/scene3d/sidecars/clean.pulp3d.json",
                    "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927")
            elif drift == "plan-boundary":
                remove_doc_token(
                    case_root / "planning/threejs-webgpu-gltf-bake-plan.md",
                    "Live `GLTFLoader + BoxTextured.glb` is a Runtime-hardening P0 item.")
            elif drift == "plan-forbidden":
                add_plan_token(
                    case_root / "planning/threejs-webgpu-gltf-bake-plan.md",
                    "Treat in-bridge GLB loading as a risk-reduction spike (Phase 4, first task)")
            elif drift == "plan-exporter-forbidden":
                add_plan_token(
                    case_root / "planning/threejs-webgpu-gltf-bake-plan.md",
                    "Wire `GLTFExporter` in the Live lane to emit GLB")
            elif drift == "url-gate":
                remove_doc_token(doc_file, "runtime_evidence_url_invalid")
            elif drift == "final-gate":
                remove_doc_token(
                    doc_file, "scene3d-renderer-probe-final-software-gate")

            expect_case(
                name,
                run_verifier(args.handoff_verifier, case_root, [ctest_file], doc_file),
                expected_code,
                expected_text,
                errors,
            )

    if errors:
        for error in errors:
            print(f"error={error}", file=sys.stderr)
        return 1

    print("native_slice_handoff_contract_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
