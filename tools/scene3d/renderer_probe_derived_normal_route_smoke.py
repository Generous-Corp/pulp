#!/usr/bin/env python3
import argparse
import json
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from scene3d_launch import argv_for


EXPECTED_TRUE_FIELDS = [
    "success",
    "scene_data_consumed",
    "texture_uploaded",
    "texture_decoded",
    "base_color_texture_srgb_applied",
    "geometry_normals_applied",
    "tangent_attributes_available",
    "tangent_attributes_derived",
    "normal_texture_applied",
    "normal_scale_applied",
    "command_submitted",
    "readback_completed",
    "pixel_output_produced",
]

EXPECTED_FALSE_FIELDS = [
    "fallback_texture_used",
    "normal_texture_deferred",
    "normal_scale_deferred",
    "non_base_color_texture_transform_deferred",
    "non_base_color_texcoord1_deferred",
]


def append_f32(values):
    return b"".join(struct.pack("<f", value) for value in values)


def append_u16(values):
    return b"".join(struct.pack("<H", value) for value in values)


def png_chunk(kind, data):
    return (
        struct.pack(">I", len(data)) +
        kind +
        data +
        struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    )


def make_png_rgba(width, height, pixels):
    rows = []
    stride = width * 4
    for y in range(height):
        rows.append(b"\x00" + bytes(pixels[y * stride:(y + 1) * stride]))
    raw = b"".join(rows)
    return (
        b"\x89PNG\r\n\x1a\n" +
        png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
        png_chunk(b"IDAT", zlib.compress(raw)) +
        png_chunk(b"IEND", b"")
    )


def make_scene_bin():
    chunks = []
    views = []

    def add_view(data, target=None):
        offset = sum(len(chunk) for chunk in chunks)
        chunks.append(data)
        while sum(len(chunk) for chunk in chunks) % 4:
            chunks.append(b"\x00")
        view = {
            "buffer": 0,
            "byteOffset": offset,
            "byteLength": len(data),
        }
        if target is not None:
            view["target"] = target
        views.append(view)
        return len(views) - 1

    position_view = add_view(append_f32([
        -1.0, -1.0, 0.0,
        1.0, -1.0, 0.0,
        0.0, 1.0, 0.0,
    ]), 34962)
    normal_view = add_view(append_f32([
        0.0, 0.0, 1.0,
        0.0, 0.0, 1.0,
        0.0, 0.0, 1.0,
    ]), 34962)
    texcoord_view = add_view(append_f32([
        0.0, 0.0,
        1.0, 0.0,
        0.5, 1.0,
    ]), 34962)
    index_view = add_view(append_u16([0, 1, 2]), 34963)

    base_png = make_png_rgba(1, 1, [
        204, 204, 204, 255,
    ])
    # A tilted normal encoded in tangent space. Non-default normalScale should
    # be reported only if tangent derivation lets the renderer sample it.
    normal_png = make_png_rgba(1, 1, [
        255, 128, 128, 255,
    ])
    base_image_view = add_view(base_png)
    normal_image_view = add_view(normal_png)

    return b"".join(chunks), views, {
        "position": position_view,
        "normal": normal_view,
        "texcoord": texcoord_view,
        "indices": index_view,
        "base_image": base_image_view,
        "normal_image": normal_image_view,
    }


def make_scene_gltf(byte_length, views, ids):
    return {
        "asset": {
            "version": "2.0",
            "generator": "pulp-renderer3d-derived-normal-route-contract",
        },
        "buffers": [{
            "uri": "derived-normal-route.bin",
            "byteLength": byte_length,
        }],
        "bufferViews": views,
        "accessors": [
            {
                "bufferView": ids["position"],
                "componentType": 5126,
                "count": 3,
                "type": "VEC3",
                "min": [-1.0, -1.0, 0.0],
                "max": [1.0, 1.0, 0.0],
            },
            {"bufferView": ids["normal"], "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": ids["texcoord"], "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": ids["indices"], "componentType": 5123, "count": 3, "type": "SCALAR"},
        ],
        "images": [
            {"bufferView": ids["base_image"], "mimeType": "image/png", "name": "base"},
            {"bufferView": ids["normal_image"], "mimeType": "image/png", "name": "normal"},
        ],
        "textures": [
            {"source": 0},
            {"source": 1},
        ],
        "materials": [{
            "name": "DerivedNormalMat",
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
            },
            "normalTexture": {
                "index": 1,
                "scale": 0.65,
            },
        }],
        "meshes": [{
            "name": "DerivedNormalMesh",
            "primitives": [{
                "attributes": {
                    "POSITION": 0,
                    "NORMAL": 1,
                    "TEXCOORD_0": 2,
                },
                "indices": 3,
                "material": 0,
                "mode": 4,
            }],
        }],
        "nodes": [{"name": "DerivedNormalNode", "mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
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
        description="Verify renderer probe reports derived tangents for normal maps.")
    parser.add_argument("--probe-tool", type=Path, required=True)
    args = parser.parse_args()

    if not args.probe_tool.exists():
        print(f"probe_tool_exists=false path={args.probe_tool}")
        return 2

    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        bin_bytes, views, ids = make_scene_bin()
        (temp_path / "derived-normal-route.bin").write_bytes(bin_bytes)
        scene_path = temp_path / "derived-normal-route.gltf"
        scene_path.write_text(
            json.dumps(make_scene_gltf(len(bin_bytes), views, ids)),
            encoding="utf-8")

        result = subprocess.run(
            [
                *argv_for(args.probe_tool),
                "--scene",
                "boxtextured",
                "--fixture",
                str(scene_path),
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
        require(values.get("scene") == "boxtextured",
                f"scene: expected 'boxtextured', got {values.get('scene')!r}")
        require(values.get("primitive_count") == "1",
                f"primitive_count: expected '1', got {values.get('primitive_count')!r}")
        for key in EXPECTED_TRUE_FIELDS:
            require(values.get(key) == "true",
                    f"{key}: expected 'true', got {values.get(key)!r}")
        for key in EXPECTED_FALSE_FIELDS:
            require(values.get(key) == "false",
                    f"{key}: expected 'false', got {values.get(key)!r}")
        require(int(values.get("non_transparent_pixel_count", "0")) > 0,
                "non_transparent_pixel_count must be positive")

    print("renderer_probe_derived_normal_route_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
