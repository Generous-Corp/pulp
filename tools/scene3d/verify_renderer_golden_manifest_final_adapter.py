#!/usr/bin/env python3
import argparse
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def final_manifest():
    return {
        "schema_version": 1,
        "golden_kind": "renderer3d_rgba_fingerprint",
        "fingerprint_algorithm": "fnv1a64",
        "status": "final_software_adapter",
        "entries": [
            {
                "id": "hardcoded_textured_cube",
                "renderer": "Renderer3D::render_hardcoded_textured_cube",
                "source": "hardcoded",
                "width": 128,
                "height": 128,
                "adapter_backend_type": "SwiftShader",
                "adapter_scope": "software_swiftshader",
                "scene_data_consumed": False,
                "primitive_count": 0,
                "depth_target_allocated": True,
                "color_target_allocated": True,
                "vertex_buffer_uploaded": True,
                "index_buffer_uploaded": True,
                "uniform_buffer_uploaded": True,
                "texture_uploaded": True,
                "pipeline_cache_entry_count": 0,
                "pipeline_cache_hit_count": 0,
                "command_submitted": True,
                "readback_completed": True,
                "pixel_output_produced": True,
                "min_distinct_color_count": 64,
                "min_non_transparent_pixel_count": 1024,
                "fingerprint": "1111111111111111111",
                "cpp_constant": "kFutureSoftwareHardcodedCubeFingerprint",
            }
        ],
        "software_adapter": {
            "status": "pinned",
            "required_for_final_phase7": True,
            "pixel_producing": True,
            "backend_type": "SwiftShader",
            "adapter_scope": "software_swiftshader",
            "golden_entry_ids": ["hardcoded_textured_cube"],
            "notes": "Synthetic final manifest used to verify validator behavior.",
        },
    }


def cpp_constants_source():
    return (
        "inline constexpr uint64_t "
        "kFutureSoftwareHardcodedCubeFingerprint = "
        "1111111111111111111ULL;\n"
    )


def run_validator(validator, manifest, cpp_test, require_pixel_software=True):
    command = [
        sys.executable,
        str(validator),
        str(manifest),
        "--cpp-test",
        str(cpp_test),
    ]
    if require_pixel_software:
        command.append("--require-pixel-software-adapter")
    return subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def expect_result(name, result, expected_code, expected_text, errors):
    sys.stdout.write(result.stdout)
    if result.returncode != expected_code:
        errors.append(
            f"{name}: expected exit {expected_code}, got {result.returncode}")
    if expected_text not in result.stdout:
        errors.append(
            f"{name}: expected diagnostic containing {expected_text!r}")
    print(f"renderer_golden_manifest_final_adapter_case={name}")


def main():
    parser = argparse.ArgumentParser(
        description="Verify final Renderer3D software-adapter manifest contract.")
    parser.add_argument("--validator", type=Path, required=True)
    args = parser.parse_args()

    if not args.validator.exists():
        print(f"validator_exists=false path={args.validator}")
        return 2

    errors = []
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        cpp_test = temp_path / "test_renderer3d_constants.cpp"
        cpp_test.write_text(cpp_constants_source(), encoding="utf-8")

        good = final_manifest()
        good_path = temp_path / "final-software-good.json"
        good_path.write_text(json.dumps(good), encoding="utf-8")
        result = run_validator(args.validator, good_path, cpp_test)
        expect_result(
            "valid-final-software-adapter",
            result,
            0,
            "Renderer3D golden manifest entries=1",
            errors,
        )

        null_backend = copy.deepcopy(good)
        null_backend["entries"][0]["adapter_backend_type"] = "Null"
        null_backend["software_adapter"]["backend_type"] = "Null"
        null_path = temp_path / "final-software-null.json"
        null_path.write_text(json.dumps(null_backend), encoding="utf-8")
        result = run_validator(args.validator, null_path, cpp_test)
        expect_result(
            "null-backend-rejected",
            result,
            1,
            "Dawn null backend is not a pixel-producing software adapter",
            errors,
        )

        missing_entry = copy.deepcopy(good)
        missing_entry["software_adapter"]["golden_entry_ids"] = [
            "missing_software_golden"
        ]
        missing_path = temp_path / "final-software-missing-entry.json"
        missing_path.write_text(json.dumps(missing_entry), encoding="utf-8")
        result = run_validator(args.validator, missing_path, cpp_test)
        expect_result(
            "missing-entry-rejected",
            result,
            1,
            "software_adapter.golden_entry_ids references missing entry missing_software_golden",
            errors,
        )

        scope_mismatch = copy.deepcopy(good)
        scope_mismatch["entries"][0]["adapter_scope"] = "software_other"
        scope_path = temp_path / "final-software-scope-mismatch.json"
        scope_path.write_text(json.dumps(scope_mismatch), encoding="utf-8")
        result = run_validator(args.validator, scope_path, cpp_test)
        expect_result(
            "scope-mismatch-rejected",
            result,
            1,
            "software golden hardcoded_textured_cube must use adapter_scope software_swiftshader",
            errors,
        )

        final_status_without_pixel_adapter = copy.deepcopy(good)
        final_status_without_pixel_adapter["software_adapter"]["pixel_producing"] = False
        final_status_path = temp_path / "final-status-without-pixel-adapter.json"
        final_status_path.write_text(
            json.dumps(final_status_without_pixel_adapter),
            encoding="utf-8")
        result = run_validator(
            args.validator,
            final_status_path,
            cpp_test,
            require_pixel_software=False)
        expect_result(
            "final-status-without-pixel-adapter-rejected",
            result,
            1,
            "final renderer goldens require a pixel-producing software adapter",
            errors,
        )

    if errors:
        for error in errors:
            print(f"error={error}")
        return 1
    print("renderer_golden_manifest_final_adapter_verified=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
