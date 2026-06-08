#!/usr/bin/env python3
import argparse
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def load_manifest(path):
    return json.loads(path.read_text(encoding="utf-8"))


def malformed_cases(manifest):
    missing_root_key = copy.deepcopy(manifest)
    missing_root_key.pop("baseline_kind")

    invalid_build_type = copy.deepcopy(manifest)
    invalid_build_type["build"]["build_type"] = "Debug"

    missing_hotspot = copy.deepcopy(manifest)
    missing_hotspot["hotspots"] = missing_hotspot["hotspots"][:1]

    missing_ccache_disable = copy.deepcopy(manifest)
    missing_ccache_disable["hotspots"][0]["timing_command"] = (
        missing_ccache_disable["hotspots"][0]["timing_command"]
        .replace("CCACHE_DISABLE=1 ", ""))

    median_drift = copy.deepcopy(manifest)
    median_drift["hotspots"][0]["median_wall_seconds"] = 1.0

    missing_source = copy.deepcopy(manifest)
    missing_source["hotspots"][0]["source"] = "core/view/src/not_real.cpp"

    missing_ctest = copy.deepcopy(manifest)
    missing_ctest["renderer_golden_gate"]["required_ctests"][0] = (
        "scene3d-renderer-golden-missing")

    missing_renderer_manifest = copy.deepcopy(manifest)
    missing_renderer_manifest["renderer_golden_gate"]["manifest"] = (
        "test/fixtures/scene3d/not-real.json")

    interim_without_final_gate = copy.deepcopy(manifest)
    interim_without_final_gate[
        "renderer_golden_gate"][
            "interim_requires_final_software_adapter"] = False

    return [
        (
            "missing-root-key",
            missing_root_key,
            "baseline root keys must be exactly",
        ),
        (
            "invalid-build-type",
            invalid_build_type,
            "build.build_type must be Release",
        ),
        (
            "missing-hotspot",
            missing_hotspot,
            "hotspots must contain exactly",
        ),
        (
            "missing-ccache-disable",
            missing_ccache_disable,
            "hotspots[0].timing_command must contain CCACHE_DISABLE=1",
        ),
        (
            "median-drift",
            median_drift,
            "hotspots[0].median_wall_seconds must equal median",
        ),
        (
            "missing-source",
            missing_source,
            "hotspots[0].source must be core/view/src/widget_bridge.cpp",
        ),
        (
            "missing-ctest",
            missing_ctest,
            "required CTest scene3d-renderer-golden-missing not found",
        ),
        (
            "missing-renderer-manifest",
            missing_renderer_manifest,
            "renderer_golden_gate.manifest does not exist",
        ),
        (
            "interim-without-final-gate",
            interim_without_final_gate,
            "renderer_golden_gate.interim_requires_final_software_adapter "
            "must be true",
        ),
    ]


def main():
    parser = argparse.ArgumentParser(
        description="Verify refactor baseline manifest rejects schema drift.")
    parser.add_argument("--validator", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, required=True)
    args = parser.parse_args()

    if not args.validator.exists():
        print(f"validator_exists=false path={args.validator}")
        return 2
    if not args.manifest.exists():
        print(f"manifest_exists=false path={args.manifest}")
        return 2

    manifest = load_manifest(args.manifest)
    errors = []
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        for name, mutated, expected in malformed_cases(manifest):
            path = temp_path / f"{name}.json"
            path.write_text(json.dumps(mutated), encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    str(args.validator),
                    str(path),
                    "--repo-root",
                    str(args.repo_root),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            output = result.stdout + result.stderr
            if result.returncode == 0:
                errors.append(f"{name}: validator unexpectedly succeeded")
            if expected not in output:
                errors.append(
                    f"{name}: expected diagnostic {expected!r}; got {output!r}")
            else:
                print(f"refactor_baseline_contract_case={name}")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("refactor_baseline_contract_verified=true")
    return 0


if __name__ == "__main__":
    sys.exit(main())
