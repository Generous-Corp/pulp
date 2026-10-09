#!/usr/bin/env python3
import argparse
import subprocess
import sys
from pathlib import Path
from scene3d_launch import argv_for


EXPECTED_BOXTEXTURED_FIELDS = {
    "scene": "boxtextured",
    "success": "true",
    "scene_data_consumed": "true",
    "primitive_count": "1",
    "texture_uploaded": "true",
    "texture_decoded": "true",
    "fallback_texture_used": "false",
    "base_color_texture_srgb_applied": "true",
    "texture_sampler_applied": "true",
    "texture_sampler_clamp_s": "false",
    "texture_sampler_clamp_t": "false",
    "texture_sampler_linear": "true",
    "texture_mipmap_filter_downgraded": "true",
    "base_color_transform_applied": "false",
    "base_color_texcoord1_used": "false",
    "base_color_factor_applied": "true",
    "unlit_material_applied": "false",
    "alpha_mask_applied": "false",
    "alpha_blend_applied": "false",
    "alpha_blend_depth_write_disabled": "false",
    "alpha_blend_sorted": "false",
    "vertex_color_applied": "false",
    "metallic_roughness_factor_applied": "true",
    "metallic_roughness_texture_applied": "false",
    "double_sided_material_applied": "false",
    "emissive_factor_applied": "false",
    "emissive_strength_applied": "false",
    "emissive_texture_applied": "false",
    "geometry_normals_applied": "true",
    "directional_light_applied": "false",
    "directional_light_transform_applied": "false",
    "point_light_applied": "false",
    "point_light_deferred": "false",
    "spot_light_applied": "false",
    "spot_light_deferred": "false",
    "punctual_light_range_applied": "false",
    "punctual_light_range_deferred": "false",
    "spot_light_cone_deferred": "false",
    "perspective_camera_applied": "false",
    "orthographic_camera_applied": "false",
    "camera_node_translation_applied": "false",
    "camera_node_rotation_applied": "false",
    "camera_aspect_ratio_applied": "false",
    "camera_aspect_ratio_deferred": "false",
    "camera_depth_range_applied": "false",
    "camera_depth_range_deferred": "false",
    "light_node_transform_deferred": "false",
    "camera_node_transform_deferred": "false",
    "transform_animation_initial_pose_applied": "false",
    "transform_animation_deferred": "false",
    "tangent_attributes_available": "false",
    "tangent_attributes_derived": "false",
    "normal_texture_applied": "false",
    "normal_scale_applied": "false",
    "metallic_roughness_texture_deferred": "false",
    "normal_texture_deferred": "false",
    "normal_scale_deferred": "false",
    "occlusion_texture_applied": "false",
    "occlusion_strength_applied": "false",
    "occlusion_texture_deferred": "false",
    "occlusion_strength_deferred": "false",
    "emissive_texture_deferred": "false",
    "non_base_color_texture_transform_applied": "false",
    "non_base_color_texcoord1_used": "false",
    "non_base_color_texture_transform_deferred": "false",
    "non_base_color_texcoord1_deferred": "false",
    "advanced_material_extension_deferred": "false",
    "skinning_deferred": "false",
    "morph_target_deferred": "false",
    "gpu_instancing_deferred": "false",
    "command_submitted": "true",
    "readback_completed": "true",
    "pixel_output_produced": "true",
}


def parse_key_values(text):
    values = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(
        description="Verify Renderer3D probe reports the BoxTextured native material floor.")
    parser.add_argument("--probe-tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()

    if not args.probe_tool.exists():
        print(f"probe_tool_exists=false path={args.probe_tool}")
        return 2
    if not args.fixture.exists():
        print(f"fixture_exists=false path={args.fixture}")
        return 2

    result = subprocess.run(
        [
            *argv_for(args.probe_tool),
            "--scene",
            "boxtextured",
            "--fixture",
            str(args.fixture),
            "--width",
            "128",
            "--height",
            "128",
            "--adapter-scope",
            "macos_default_metal",
        ],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    sys.stdout.write(result.stdout)
    if result.returncode != 0:
        print(f"probe_exit_code={result.returncode}", file=sys.stderr)
        return result.returncode

    values = parse_key_values(result.stdout)
    for key, expected in EXPECTED_BOXTEXTURED_FIELDS.items():
        actual = values.get(key)
        require(actual == expected,
                f"{key}: expected {expected!r}, got {actual!r}")

    print("renderer_probe_material_floor_verified=boxtextured")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
