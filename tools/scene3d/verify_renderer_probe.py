#!/usr/bin/env python3
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


PROBE_FIELDS = {
    "scene",
    "width",
    "height",
    "success",
    "gpu_available",
    "adapter_info_available",
    "adapter_backend_type",
    "adapter_backend",
    "adapter_name",
    "adapter_vendor",
    "adapter_architecture",
    "adapter_scope",
    "adapter_backend_preference",
    "fallback_adapter_requested",
    "null_backend_requested",
    "scene_data_consumed",
    "depth_target_allocated",
    "color_target_allocated",
    "vertex_buffer_uploaded",
    "index_buffer_uploaded",
    "uniform_buffer_uploaded",
    "texture_uploaded",
    "texture_decoded",
    "fallback_texture_used",
    "base_color_texture_srgb_applied",
    "texture_sampler_applied",
    "texture_sampler_clamp_s",
    "texture_sampler_clamp_t",
    "texture_sampler_linear",
    "texture_mipmap_filter_downgraded",
    "base_color_transform_applied",
    "base_color_texcoord1_used",
    "base_color_factor_applied",
    "unlit_material_applied",
    "alpha_mask_applied",
    "alpha_blend_applied",
    "alpha_blend_depth_write_disabled",
    "alpha_blend_sorted",
    "vertex_color_applied",
    "metallic_roughness_factor_applied",
    "metallic_roughness_texture_applied",
    "double_sided_material_applied",
    "emissive_factor_applied",
    "emissive_strength_applied",
    "emissive_texture_applied",
    "geometry_normals_applied",
    "directional_light_applied",
    "directional_light_transform_applied",
    "point_light_applied",
    "point_light_deferred",
    "spot_light_applied",
    "spot_light_deferred",
    "punctual_light_range_applied",
    "punctual_light_range_deferred",
    "spot_light_cone_deferred",
    "perspective_camera_applied",
    "orthographic_camera_applied",
    "camera_node_translation_applied",
    "camera_node_rotation_applied",
    "camera_aspect_ratio_applied",
    "camera_aspect_ratio_deferred",
    "camera_depth_range_applied",
    "camera_depth_range_deferred",
    "light_node_transform_deferred",
    "camera_node_transform_deferred",
    "transform_animation_initial_pose_applied",
    "transform_animation_deferred",
    "tangent_attributes_available",
    "tangent_attributes_derived",
    "normal_texture_applied",
    "normal_scale_applied",
    "metallic_roughness_texture_deferred",
    "normal_texture_deferred",
    "normal_scale_deferred",
    "occlusion_texture_applied",
    "occlusion_strength_applied",
    "occlusion_texture_deferred",
    "occlusion_strength_deferred",
    "emissive_texture_deferred",
    "non_base_color_texture_transform_applied",
    "non_base_color_texcoord1_used",
    "non_base_color_texture_transform_deferred",
    "non_base_color_texcoord1_deferred",
    "advanced_material_extension_deferred",
    "skinning_deferred",
    "morph_target_deferred",
    "gpu_instancing_deferred",
    "command_submitted",
    "readback_completed",
    "primitive_count",
    "pipeline_cache_entry_count",
    "pipeline_cache_hit_count",
    "distinct_color_count",
    "non_transparent_pixel_count",
    "rgba_fingerprint",
    "pixel_output_produced",
    "final_software_golden_eligible",
}

REQUIRED_MANIFEST_KEYS = {
    "schema_version",
    "golden_kind",
    "fingerprint_algorithm",
    "status",
    "entries",
    "software_adapter",
}

REQUIRED_ENTRY_KEYS = {
    "id",
    "renderer",
    "source",
    "width",
    "height",
    "adapter_backend_type",
    "adapter_scope",
    "scene_data_consumed",
    "primitive_count",
    "depth_target_allocated",
    "color_target_allocated",
    "vertex_buffer_uploaded",
    "index_buffer_uploaded",
    "uniform_buffer_uploaded",
    "texture_uploaded",
    "pipeline_cache_entry_count",
    "pipeline_cache_hit_count",
    "command_submitted",
    "readback_completed",
    "pixel_output_produced",
    "min_distinct_color_count",
    "min_non_transparent_pixel_count",
    "fingerprint",
    "cpp_constant",
}

REQUIRED_SOFTWARE_ADAPTER_KEYS = {
    "status",
    "required_for_final_phase7",
    "pixel_producing",
    "backend_type",
    "adapter_scope",
    "golden_entry_ids",
    "notes",
}


def load_manifest(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def parse_key_values(text: str):
    values = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key.strip()):
            continue
        values[key.strip()] = value.strip()
    return values


def find_entry(manifest, entry_id):
    for entry in manifest.get("entries", []):
        if isinstance(entry, dict) and entry.get("id") == entry_id:
            return entry
    raise ValueError(f"missing golden entry: {entry_id}")


def validate_manifest_shape(manifest, entry):
    errors = []
    if not isinstance(manifest, dict):
        return ["manifest root must be an object"]
    if set(manifest.keys()) != REQUIRED_MANIFEST_KEYS:
        errors.append(
            "manifest fields: expected "
            f"{sorted(REQUIRED_MANIFEST_KEYS)!r}, got "
            f"{sorted(manifest.keys())!r}")
    if manifest.get("schema_version") != 1:
        errors.append("manifest schema_version must be 1")
    if manifest.get("golden_kind") != "renderer3d_rgba_fingerprint":
        errors.append("manifest golden_kind must be renderer3d_rgba_fingerprint")
    if manifest.get("fingerprint_algorithm") != "fnv1a64":
        errors.append("manifest fingerprint_algorithm must be fnv1a64")
    if manifest.get("status") not in {
            "interim_default_adapter",
            "final_software_adapter"}:
        errors.append("manifest status is not supported")

    entries = manifest.get("entries")
    if not isinstance(entries, list) or not entries:
        errors.append("manifest entries must be a non-empty array")

    if not isinstance(entry, dict):
        errors.append("manifest entry must be an object")
    else:
        entry_fields = set(entry.keys())
        if entry_fields != REQUIRED_ENTRY_KEYS:
            errors.append(
                "manifest entry fields: expected "
                f"{sorted(REQUIRED_ENTRY_KEYS)!r}, got "
                f"{sorted(entry_fields)!r}")
        for key in (
            "scene_data_consumed",
            "depth_target_allocated",
            "color_target_allocated",
            "vertex_buffer_uploaded",
            "index_buffer_uploaded",
            "uniform_buffer_uploaded",
            "texture_uploaded",
            "command_submitted",
            "readback_completed",
            "pixel_output_produced",
        ):
            if key in entry and not isinstance(entry.get(key), bool):
                errors.append(f"manifest entry {key} must be a boolean")
        for key in (
            "width",
            "height",
            "primitive_count",
            "pipeline_cache_entry_count",
            "pipeline_cache_hit_count",
            "min_distinct_color_count",
            "min_non_transparent_pixel_count",
        ):
            if key in entry and not isinstance(entry.get(key), int):
                errors.append(f"manifest entry {key} must be an integer")
        for key in (
            "id",
            "renderer",
            "source",
            "adapter_backend_type",
            "adapter_scope",
            "fingerprint",
            "cpp_constant",
        ):
            if key in entry and not isinstance(entry.get(key), str):
                errors.append(f"manifest entry {key} must be a string")

    software = manifest.get("software_adapter")
    if not isinstance(software, dict):
        errors.append("manifest software_adapter must be an object")
    else:
        software_fields = set(software.keys())
        if software_fields != REQUIRED_SOFTWARE_ADAPTER_KEYS:
            errors.append(
                "manifest software_adapter fields: expected "
                f"{sorted(REQUIRED_SOFTWARE_ADAPTER_KEYS)!r}, got "
                f"{sorted(software_fields)!r}")
        if "pixel_producing" in software and not isinstance(
                software.get("pixel_producing"), bool):
            errors.append(
                "manifest software_adapter.pixel_producing must be a boolean")
        golden_ids = software.get("golden_entry_ids")
        if "golden_entry_ids" in software and not isinstance(golden_ids, list):
            errors.append(
                "manifest software_adapter.golden_entry_ids must be an array")
        if isinstance(golden_ids, list):
            for index, value in enumerate(golden_ids):
                if not isinstance(value, str):
                    errors.append(
                        "manifest software_adapter.golden_entry_ids"
                        f"[{index}] must be a string")
    return errors


def expected_final_software_eligible(manifest, entry_id):
    software = manifest.get("software_adapter", {})
    if not isinstance(software, dict):
        return False
    golden_ids = software.get("golden_entry_ids", [])
    return (
        manifest.get("status") == "final_software_adapter" and
        software.get("pixel_producing") is True and
        isinstance(golden_ids, list) and
        entry_id in golden_ids
    )


def require_equal(values, key, expected, errors):
    actual = values.get(key)
    if actual != expected:
        errors.append(f"{key}: expected {expected!r}, got {actual!r}")


def require_bool(values, key, expected, errors):
    require_equal(values, key, "true" if expected else "false", errors)


def int_value(values, key, errors):
    actual = values.get(key)
    if actual is None:
        errors.append(f"{key}: missing")
        return None
    try:
        return int(actual)
    except ValueError:
        errors.append(f"{key}: expected integer, got {actual!r}")
        return None


def require_at_least(values, key, minimum, errors):
    actual = int_value(values, key, errors)
    if actual is not None and actual < minimum:
        errors.append(f"{key}: expected at least {minimum}, got {actual}")


def golden_not_applicable(entry, values, platform):
    """Why this host cannot judge the golden, or None when it can. A golden
    scoped to the macOS default adapter is a fingerprint of that adapter's
    rasterization; another host's adapter draws the same scene with other
    bytes, so off macOS a different backend is not a failure. On macOS it is
    one: there the scope's adapter is the one that should have been picked."""
    scope = str(entry.get("adapter_scope", ""))
    expected = str(entry.get("adapter_backend_type", ""))
    actual = values.get("adapter_backend_type", "")
    if platform == "darwin" or not scope.startswith("macos_") or actual == expected:
        return None
    return (f"golden recorded on {scope} ({expected}); this host's adapter is "
            f"{actual or 'unknown'} ({values.get('adapter_name', 'unnamed')})")


def main():
    parser = argparse.ArgumentParser(
        description="Verify Renderer3D probe output against the golden manifest.")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--entry-id", required=True)
    parser.add_argument("probe_command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    if not args.probe_command:
        print("verify_renderer_probe.py: missing probe command", file=sys.stderr)
        return 64
    if args.probe_command[0] == "--":
        args.probe_command = args.probe_command[1:]

    try:
        manifest = load_manifest(args.manifest)
        entry = find_entry(manifest, args.entry_id)
        manifest_errors = validate_manifest_shape(manifest, entry)
        if manifest_errors:
            for error in manifest_errors:
                print(error, file=sys.stderr)
            return 2
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 2

    result = subprocess.run(
        args.probe_command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        print(f"probe command exited {result.returncode}", file=sys.stderr)
        return 1

    values = parse_key_values(result.stdout)
    not_applicable = golden_not_applicable(entry, values, sys.platform)
    if not_applicable:
        print(f"renderer_probe_golden_not_applicable={args.entry_id}: {not_applicable}")
        return 0
    errors = []
    actual_fields = set(values.keys())
    if actual_fields != PROBE_FIELDS:
        errors.append(
            "probe fields: expected "
            f"{sorted(PROBE_FIELDS)!r}, got {sorted(actual_fields)!r}")
    require_equal(values, "success", "true", errors)
    for key in (
        "depth_target_allocated",
        "color_target_allocated",
        "vertex_buffer_uploaded",
        "index_buffer_uploaded",
        "uniform_buffer_uploaded",
        "texture_uploaded",
        "command_submitted",
        "readback_completed",
        "pixel_output_produced",
    ):
        require_bool(values, key, bool(entry[key]), errors)
    require_equal(values, "adapter_backend_type",
                  str(entry["adapter_backend_type"]), errors)
    require_equal(values, "adapter_scope", str(entry["adapter_scope"]), errors)
    require_equal(values, "width", str(entry["width"]), errors)
    require_equal(values, "height", str(entry["height"]), errors)
    require_equal(values, "scene_data_consumed",
                  "true" if entry["scene_data_consumed"] else "false",
                  errors)
    require_equal(values, "primitive_count", str(entry["primitive_count"]),
                  errors)
    require_equal(values,
                  "pipeline_cache_entry_count",
                  str(entry["pipeline_cache_entry_count"]),
                  errors)
    require_equal(values,
                  "pipeline_cache_hit_count",
                  str(entry["pipeline_cache_hit_count"]),
                  errors)
    require_at_least(values,
                     "distinct_color_count",
                     int(entry["min_distinct_color_count"]),
                     errors)
    require_at_least(values,
                     "non_transparent_pixel_count",
                     int(entry["min_non_transparent_pixel_count"]),
                     errors)
    require_equal(values, "rgba_fingerprint", str(entry["fingerprint"]), errors)
    require_bool(values,
                 "final_software_golden_eligible",
                 expected_final_software_eligible(manifest, args.entry_id),
                 errors)

    if entry.get("source") == "hardcoded":
        require_equal(values, "scene", "hardcoded", errors)
    else:
        require_equal(values, "scene", "boxtextured", errors)

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(f"renderer_probe_verified={args.entry_id}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
