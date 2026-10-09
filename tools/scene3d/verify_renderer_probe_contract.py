#!/usr/bin/env python3
import argparse
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path


PROBE_LINES = [
    "scene=boxtextured",
    "width=128",
    "height=128",
    "success=true",
    "gpu_available=true",
    "adapter_info_available=true",
    "adapter_backend_type=Metal",
    "adapter_backend=Metal",
    "adapter_name=Apple M-series",
    "adapter_vendor=Apple",
    "adapter_architecture=arm64",
    "adapter_scope=macos_default_metal",
    "adapter_backend_preference=default",
    "fallback_adapter_requested=false",
    "null_backend_requested=false",
    "scene_data_consumed=true",
    "depth_target_allocated=true",
    "color_target_allocated=true",
    "vertex_buffer_uploaded=true",
    "index_buffer_uploaded=true",
    "uniform_buffer_uploaded=true",
    "texture_uploaded=true",
    "texture_decoded=true",
    "fallback_texture_used=false",
    "base_color_texture_srgb_applied=true",
    "texture_sampler_applied=true",
    "texture_sampler_clamp_s=false",
    "texture_sampler_clamp_t=false",
    "texture_sampler_linear=true",
    "texture_mipmap_filter_downgraded=true",
    "base_color_transform_applied=false",
    "base_color_texcoord1_used=false",
    "base_color_factor_applied=true",
    "unlit_material_applied=false",
    "alpha_mask_applied=false",
    "alpha_blend_applied=false",
    "alpha_blend_depth_write_disabled=false",
    "alpha_blend_sorted=false",
    "vertex_color_applied=false",
    "metallic_roughness_factor_applied=true",
    "metallic_roughness_texture_applied=false",
    "double_sided_material_applied=false",
    "emissive_factor_applied=false",
    "emissive_strength_applied=false",
    "emissive_texture_applied=false",
    "geometry_normals_applied=true",
    "directional_light_applied=false",
    "directional_light_transform_applied=false",
    "point_light_applied=false",
    "point_light_deferred=false",
    "spot_light_applied=false",
    "spot_light_deferred=false",
    "punctual_light_range_applied=false",
    "punctual_light_range_deferred=false",
    "spot_light_cone_deferred=false",
    "perspective_camera_applied=false",
    "orthographic_camera_applied=false",
    "camera_node_translation_applied=false",
    "camera_node_rotation_applied=false",
    "camera_aspect_ratio_applied=false",
    "camera_aspect_ratio_deferred=false",
    "camera_depth_range_applied=false",
    "camera_depth_range_deferred=false",
    "light_node_transform_deferred=false",
    "camera_node_transform_deferred=false",
    "transform_animation_initial_pose_applied=false",
    "transform_animation_deferred=false",
    "tangent_attributes_available=false",
    "tangent_attributes_derived=false",
    "normal_texture_applied=false",
    "normal_scale_applied=false",
    "metallic_roughness_texture_deferred=false",
    "normal_texture_deferred=false",
    "normal_scale_deferred=false",
    "occlusion_texture_applied=false",
    "occlusion_strength_applied=false",
    "occlusion_texture_deferred=false",
    "occlusion_strength_deferred=false",
    "emissive_texture_deferred=false",
    "non_base_color_texture_transform_applied=false",
    "non_base_color_texcoord1_used=false",
    "non_base_color_texture_transform_deferred=false",
    "non_base_color_texcoord1_deferred=false",
    "advanced_material_extension_deferred=false",
    "skinning_deferred=false",
    "morph_target_deferred=false",
    "gpu_instancing_deferred=false",
    "command_submitted=true",
    "readback_completed=true",
    "primitive_count=1",
    "pipeline_cache_entry_count=1",
    "pipeline_cache_hit_count=0",
    "distinct_color_count=64",
    "non_transparent_pixel_count=1024",
    "rgba_fingerprint=5845745157752120258",
    "pixel_output_produced=true",
    "final_software_golden_eligible=false",
]


def fake_probe_source(lines):
    return "print(" + repr("\n".join(lines) + "\n") + ", end='')\n"


def run_probe_verifier(verifier, manifest, probe):
    return subprocess.run(
        [
            sys.executable,
            str(verifier),
            "--manifest",
            str(manifest),
            "--entry-id",
            "official_boxtextured_fixture",
            "--",
            sys.executable,
            str(probe),
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def load_manifest(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_manifest(path, manifest):
    path.write_text(json.dumps(manifest), encoding="utf-8")


def replace_line(lines, key, replacement):
    prefix = key + "="
    return [
        replacement if line.startswith(prefix) else line
        for line in lines
    ]


def remove_line(lines, key):
    prefix = key + "="
    return [line for line in lines if not line.startswith(prefix)]


def manifest_without_entry_key(base_manifest, key):
    manifest = json.loads(json.dumps(base_manifest))
    for entry in manifest["entries"]:
        if entry.get("id") == "official_boxtextured_fixture":
            entry.pop(key)
            break
    return manifest


def manifest_with_extra_entry_key(base_manifest):
    manifest = json.loads(json.dumps(base_manifest))
    for entry in manifest["entries"]:
        if entry.get("id") == "official_boxtextured_fixture":
            entry["unexpected"] = "drift"
            break
    return manifest


def manifest_with_bad_software_adapter(base_manifest):
    manifest = json.loads(json.dumps(base_manifest))
    manifest["software_adapter"]["golden_entry_ids"] = (
        "official_boxtextured_fixture")
    return manifest


def expect_result(name, result, expected_code, expected_text, errors):
    sys.stdout.write(result.stdout)
    if result.returncode != expected_code:
        errors.append(
            f"{name}: expected exit {expected_code}, got {result.returncode}")
    if expected_text not in result.stdout:
        errors.append(
            f"{name}: expected diagnostic containing {expected_text!r}")
    print(f"renderer_probe_contract_case={name}")


def golden_scope_rule_errors(verifier_path):
    """The macOS-scoped golden steps aside off macOS for a different backend,
    and only there: on macOS, or for the scope's own backend, it judges."""
    spec = importlib.util.spec_from_file_location("verify_renderer_probe", verifier_path)
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    entry = {"adapter_scope": "macos_default_metal", "adapter_backend_type": "Metal"}
    vulkan = {"adapter_backend_type": "Vulkan", "adapter_name": "llvmpipe"}
    errors = []
    if not verifier.golden_not_applicable(entry, vulkan, "linux"):
        errors.append("golden-scope-rule: a Vulkan adapter off macOS must not judge the Metal golden")
    if verifier.golden_not_applicable(entry, vulkan, "darwin"):
        errors.append("golden-scope-rule: on macOS a non-Metal adapter is drift, not a skip")
    if verifier.golden_not_applicable(entry, {"adapter_backend_type": "Metal"}, "linux"):
        errors.append("golden-scope-rule: the scope's own backend always judges")
    if verifier.golden_not_applicable({"adapter_scope": "software_lavapipe", "adapter_backend_type": "Vulkan"},
                                      {"adapter_backend_type": "Metal"}, "linux"):
        errors.append("golden-scope-rule: only macOS-scoped goldens step aside")
    return errors


def main():
    parser = argparse.ArgumentParser(
        description="Verify Renderer3D probe verifier rejects malformed probe output.")
    parser.add_argument("--probe-verifier", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()

    if not args.probe_verifier.exists():
        print(f"probe_verifier_exists=false path={args.probe_verifier}")
        return 2
    if not args.manifest.exists():
        print(f"manifest_exists=false path={args.manifest}")
        return 2

    base_manifest = load_manifest(args.manifest)
    cases = [
        (
            "valid-fake-probe",
            PROBE_LINES,
            0,
            "renderer_probe_verified=official_boxtextured_fixture",
        ),
        (
            "missing-resource-field",
            remove_line(PROBE_LINES, "texture_uploaded"),
            1,
            "probe fields: expected",
        ),
        (
            "scene-data-consumption-drift",
            replace_line(PROBE_LINES,
                         "scene_data_consumed",
                         "scene_data_consumed=false"),
            1,
            "scene_data_consumed: expected 'true', got 'false'",
        ),
        (
            "pixel-output-drift",
            replace_line(PROBE_LINES,
                         "pixel_output_produced",
                         "pixel_output_produced=false"),
            1,
            "pixel_output_produced: expected 'true', got 'false'",
        ),
        (
            "coverage-floor-drift",
            replace_line(PROBE_LINES,
                         "non_transparent_pixel_count",
                         "non_transparent_pixel_count=1"),
            1,
            "non_transparent_pixel_count: expected at least 1024, got 1",
        ),
        # Another adapter's bytes cannot judge a macOS-scoped golden: off
        # macOS the verifier says so and steps aside, on macOS it is drift.
        (
            "other-adapter-backend",
            replace_line(PROBE_LINES,
                         "adapter_backend_type",
                         "adapter_backend_type=Vulkan"),
            0 if sys.platform != "darwin" else 1,
            "renderer_probe_golden_not_applicable=official_boxtextured_fixture"
            if sys.platform != "darwin" else
            "adapter_backend_type: expected 'Metal', got 'Vulkan'",
        ),
    ]
    manifest_cases = [
        (
            "manifest-missing-entry-field",
            manifest_without_entry_key(base_manifest, "texture_uploaded"),
            2,
            "manifest entry fields: expected",
        ),
        (
            "manifest-extra-entry-field",
            manifest_with_extra_entry_key(base_manifest),
            2,
            "manifest entry fields: expected",
        ),
        (
            "manifest-software-adapter-drift",
            manifest_with_bad_software_adapter(base_manifest),
            2,
            "manifest software_adapter.golden_entry_ids must be an array",
        ),
    ]

    errors = []
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        for name, lines, expected_code, expected_text in cases:
            probe = temp_path / f"{name}.py"
            probe.write_text(fake_probe_source(lines), encoding="utf-8")
            result = run_probe_verifier(
                args.probe_verifier,
                args.manifest,
                probe)
            expect_result(name,
                          result,
                          expected_code,
                          expected_text,
                          errors)
        valid_probe = temp_path / "valid-probe.py"
        valid_probe.write_text(fake_probe_source(PROBE_LINES), encoding="utf-8")
        for name, manifest, expected_code, expected_text in manifest_cases:
            manifest_path = temp_path / f"{name}.json"
            write_manifest(manifest_path, manifest)
            result = run_probe_verifier(
                args.probe_verifier,
                manifest_path,
                valid_probe)
            expect_result(name,
                          result,
                          expected_code,
                          expected_text,
                          errors)

    errors.extend(golden_scope_rule_errors(args.probe_verifier))
    print("renderer_probe_contract_case=golden-scope-rule")

    if errors:
        for error in errors:
            print(f"error={error}")
        return 1
    print("renderer_probe_contract_verified=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
