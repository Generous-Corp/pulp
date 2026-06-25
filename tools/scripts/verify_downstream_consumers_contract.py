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
    missing_root_key.pop("schema_boundary")

    invalid_sdk_recipe = copy.deepcopy(manifest)
    invalid_sdk_recipe["sdk_recipe"]["required_options"].remove(
        "-DCMAKE_BUILD_TYPE=Release")

    duplicate_consumer = copy.deepcopy(manifest)
    duplicate_consumer["consumers"][1]["repo"] = duplicate_consumer[
        "consumers"][0]["repo"]

    invalid_sha = copy.deepcopy(manifest)
    invalid_sha["consumers"][0]["live_head"] = "210836ce"

    missing_command = copy.deepcopy(manifest)
    missing_command["consumers"][1]["required_commands"] = missing_command[
        "consumers"][1]["required_commands"][:1]

    missing_expected = copy.deepcopy(manifest)
    missing_expected["consumers"][0]["required_commands"][0]["expected"].pop(
        "value")

    project_design_merge = copy.deepcopy(manifest)
    project_design_merge["schema_boundary"]["project_ir_is_not_design_ir"] = False

    adapter_without_sdk = copy.deepcopy(manifest)
    adapter_without_sdk["consumers"][2]["dependency_surfaces"] = [
        item for item in adapter_without_sdk["consumers"][2]["dependency_surfaces"]
        if item != "installed Pulp SDK CMake package"
    ]

    return [
        (
            "missing-root-key",
            missing_root_key,
            "manifest root keys must be exactly",
        ),
        (
            "invalid-sdk-recipe",
            invalid_sdk_recipe,
            "sdk_recipe.required_options must include -DCMAKE_BUILD_TYPE=Release",
        ),
        (
            "duplicate-consumer",
            duplicate_consumer,
            "consumers[1].repo duplicates",
        ),
        (
            "invalid-sha",
            invalid_sha,
            "consumers[0].live_head must be a 40-character lowercase hex commit",
        ),
        (
            "missing-command",
            missing_command,
            "consumers[1].required_commands must contain at least 2 commands",
        ),
        (
            "missing-expected",
            missing_expected,
            "consumers[0].required_commands[0].expected keys must be exactly",
        ),
        (
            "project-design-merge",
            project_design_merge,
            "schema_boundary.project_ir_is_not_design_ir must be true",
        ),
        (
            "adapter-without-sdk",
            adapter_without_sdk,
            "consumers[2].dependency_surfaces must include installed Pulp SDK",
        ),
    ]


def main():
    parser = argparse.ArgumentParser(
        description="Verify downstream manifest rejects schema drift.")
    parser.add_argument("--validator", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()

    if not args.validator.exists():
        print(f"validator_exists=false path={args.validator}")
        return 2
    if not args.manifest.exists():
        print(f"manifest_exists=false path={args.manifest}")
        return 2

    manifest = load_manifest(args.manifest)
    valid_result = subprocess.run(
        [
            sys.executable,
            str(args.validator),
            str(args.manifest),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if valid_result.returncode != 0:
        print(
            "valid manifest unexpectedly failed: " +
            (valid_result.stdout + valid_result.stderr),
            file=sys.stderr)
        return 1
    print("downstream_manifest_contract_case=valid-current")

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
                print(f"downstream_manifest_contract_case={name}")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("downstream_manifest_contract_verified=true")
    return 0


if __name__ == "__main__":
    sys.exit(main())
