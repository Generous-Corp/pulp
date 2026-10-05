#!/usr/bin/env python3
import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from scene3d_launch import argv_for


EXPECTED_FEATURES = (
    "normals,texcoord0,indexed,base_color_texture,unlit,double_sided,"
    "alpha_blend,metallic_roughness_texture,normal_texture,occlusion_texture,"
    "emissive_texture,alpha_mask,tangents,texcoord1,color0,"
    "base_color_texture_transform,non_base_color_texture_transform,"
    "non_base_color_texcoord1,normal_scale,occlusion_strength"
)

EXPECTED_PRIMITIVE = {
    "node": "0",
    "mesh": "0",
    "primitive": "0",
    "material": "0",
    "feature_mask": "2097023",
    "world_transform": "1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1",
    "features": EXPECTED_FEATURES,
}


def append_f32(values):
    return b"".join(struct.pack("<f", value) for value in values)


def append_u16(values):
    return b"".join(struct.pack("<H", value) for value in values)


def make_geometry_bin():
    data = bytearray()
    offsets = {}

    def add(name, payload):
        offsets[name] = len(data)
        data.extend(payload)

    add("positions", append_f32([
        -1.0, -1.0, 0.0,
        1.0, -1.0, 0.0,
        0.0, 1.0, 0.0,
    ]))
    add("normals", append_f32([
        0.0, 0.0, 1.0,
        0.0, 0.0, 1.0,
        0.0, 0.0, 1.0,
    ]))
    add("tangents", append_f32([
        1.0, 0.0, 0.0, 1.0,
        1.0, 0.0, 0.0, 1.0,
        1.0, 0.0, 0.0, 1.0,
    ]))
    add("texcoord0", append_f32([
        0.0, 0.0,
        1.0, 0.0,
        0.5, 1.0,
    ]))
    add("texcoord1", append_f32([
        0.0, 1.0,
        1.0, 1.0,
        0.5, 0.0,
    ]))
    add("color0", append_f32([
        1.0, 0.0, 0.0, 1.0,
        0.0, 1.0, 0.0, 1.0,
        0.0, 0.0, 1.0, 1.0,
    ]))
    add("indices", append_u16([0, 1, 2]))
    return bytes(data), offsets


def buffer_view(offsets, name, length):
    return {"buffer": 0, "byteOffset": offsets[name], "byteLength": length}


def make_scene_gltf(byte_length, offsets):
    return {
        "asset": {
            "version": "2.0",
            "generator": "pulp-scene3d-render-packet-material-key-contract",
        },
        "extensionsUsed": [
            "KHR_texture_transform",
            "KHR_materials_unlit",
        ],
        "buffers": [{
            "uri": "geometry.bin",
            "byteLength": byte_length,
        }],
        "bufferViews": [
            buffer_view(offsets, "positions", 36),
            buffer_view(offsets, "normals", 36),
            buffer_view(offsets, "tangents", 48),
            buffer_view(offsets, "texcoord0", 24),
            buffer_view(offsets, "texcoord1", 24),
            buffer_view(offsets, "color0", 48),
            buffer_view(offsets, "indices", 6),
        ],
        "accessors": [
            {
                "bufferView": 0,
                "componentType": 5126,
                "count": 3,
                "type": "VEC3",
                "min": [-1.0, -1.0, 0.0],
                "max": [1.0, 1.0, 0.0],
            },
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
            {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 5, "componentType": 5126, "count": 3, "type": "VEC4"},
            {"bufferView": 6, "componentType": 5123, "count": 3, "type": "SCALAR"},
        ],
        "samplers": [
            {"name": "nearestClamp", "magFilter": 9728, "minFilter": 9984,
             "wrapS": 33071, "wrapT": 33648},
            {"name": "linearRepeat", "magFilter": 9729, "minFilter": 9987,
             "wrapS": 10497, "wrapT": 10497},
        ],
        "images": [
            {"uri": "base.png", "mimeType": "image/png", "name": "base"},
            {"uri": "metalrough.png", "mimeType": "image/png", "name": "metalrough"},
            {"uri": "normal.png", "mimeType": "image/png", "name": "normal"},
            {"uri": "occlusion.png", "mimeType": "image/png", "name": "occlusion"},
            {"uri": "emissive.png", "mimeType": "image/png", "name": "emissive"},
        ],
        "textures": [
            {"source": 0, "sampler": 0},
            {"source": 1, "sampler": 1},
            {"source": 2, "sampler": 1},
            {"source": 3, "sampler": 0},
            {"source": 4, "sampler": 1},
        ],
        "materials": [{
            "name": "RichPBR",
            "doubleSided": True,
            "alphaMode": "MASK",
            "alphaCutoff": 0.42,
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.2, 0.4, 0.6, 0.8],
                "metallicFactor": 0.7,
                "roughnessFactor": 0.35,
                "baseColorTexture": {
                    "index": 0,
                    "texCoord": 1,
                    "extensions": {
                        "KHR_texture_transform": {
                            "offset": [0.25, 0.5],
                            "scale": [2.0, 3.0],
                            "rotation": 0.125,
                            "texCoord": 1,
                        },
                    },
                },
                "metallicRoughnessTexture": {
                    "index": 1,
                    "texCoord": 1,
                    "extensions": {
                        "KHR_texture_transform": {
                            "offset": [0.1, 0.2],
                            "scale": [0.5, 0.75],
                        },
                    },
                },
            },
            "normalTexture": {"index": 2, "texCoord": 1, "scale": 0.65},
            "occlusionTexture": {"index": 3, "texCoord": 1, "strength": 0.4},
            "emissiveTexture": {"index": 4, "texCoord": 1},
            "emissiveFactor": [0.1, 0.2, 0.3],
            "extensions": {
                "KHR_materials_unlit": {},
            },
        }],
        "meshes": [{
            "name": "RichMesh",
            "primitives": [{
                "attributes": {
                    "POSITION": 0,
                    "NORMAL": 1,
                    "TANGENT": 2,
                    "TEXCOORD_0": 3,
                    "TEXCOORD_1": 4,
                    "COLOR_0": 5,
                },
                "indices": 6,
                "material": 0,
            }],
        }],
        "nodes": [{
            "name": "RichNode",
            "mesh": 0,
        }],
        "scenes": [{
            "nodes": [0],
        }],
        "scene": 0,
    }


def parse_key_values(line):
    values = {}
    for token in line.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        values[key] = value
    return values


def require(condition, message):
    if not condition:
        raise ValueError(message)


def require_single_line(lines, prefix, label):
    matches = [line for line in lines if line.startswith(prefix)]
    require(len(matches) == 1, f"{label}.count: expected 1, got {len(matches)}")
    return parse_key_values(matches[0])


def require_fields(actual, expected, label):
    require(set(actual.keys()) == set(expected.keys()),
            f"{label}.keys: expected {sorted(expected.keys())!r}, got {sorted(actual.keys())!r}")
    for key, expected_value in expected.items():
        actual_value = actual.get(key)
        require(actual_value == expected_value,
                f"{label}.{key}: expected {expected_value!r}, got {actual_value!r}")


def require_stats(stats):
    expected = {
        "nodes": "1",
        "roots": "1",
        "meshes": "1",
        "primitives": "1",
        "indexed_primitives": "1",
        "vertices": "3",
        "indices": "3",
        "materials": "1",
        "textures": "5",
        "texture_samplers": "2",
        "texture_bytes": "40",
        "advanced_material_extensions": "0",
        "cameras": "0",
        "lights": "0",
        "animations": "0",
        "unsupported_features": "0",
        "diagnostics": "0",
        "error_diagnostics": "0",
    }
    require_fields(stats, expected, "stats")


def main():
    parser = argparse.ArgumentParser(
        description="Verify rich material feature masks survive through scene3d inspect render packets.")
    parser.add_argument("--inspect-tool", type=Path, required=True)
    args = parser.parse_args()

    if not args.inspect_tool.exists():
        print(f"inspect_tool_exists=false path={args.inspect_tool}")
        return 2

    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        geometry, offsets = make_geometry_bin()
        (temp_path / "geometry.bin").write_bytes(geometry)
        for image_name in ("base.png", "metalrough.png", "normal.png", "occlusion.png", "emissive.png"):
            (temp_path / image_name).write_bytes(b"\x89PNG\r\n\x1a\n")
        scene_path = temp_path / "rich-material.gltf"
        scene_path.write_text(
            json.dumps(make_scene_gltf(len(geometry), offsets)),
            encoding="utf-8")

        result = subprocess.run(
            [*argv_for(args.inspect_tool), "--render-packet", str(scene_path)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        sys.stdout.write(result.stdout)
        if result.returncode != 0:
            print(f"inspect_exit_code={result.returncode}", file=sys.stderr)
            return result.returncode

    lines = result.stdout.splitlines()
    require_stats(require_single_line(lines, "nodes=", "stats"))
    require_fields(require_single_line(lines, "render_packet ", "render_packet"),
                   {
                       "transformed_nodes": "1",
                       "primitives": "1",
                       "diagnostics": "0",
                       "has_errors": "false",
                   },
                   "render_packet")
    require_fields(require_single_line(lines, "primitive ", "primitive"),
                   EXPECTED_PRIMITIVE,
                   "primitive")

    print("scene3d_render_packet_material_key_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
