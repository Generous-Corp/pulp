#!/usr/bin/env python3
import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from scene3d_launch import argv_for


EXPECTED_WORLD_TRANSFORM = (
    "2,0,0,0,0,3,0,0,0,0,4,0,12,26,42,1"
)


def append_f32(values):
    return b"".join(struct.pack("<f", value) for value in values)


def append_u16(values):
    return b"".join(struct.pack("<H", value) for value in values)


def make_geometry_bin():
    return (
        append_f32([
            -1.0, -1.0, 0.0,
            1.0, -1.0, 0.0,
            0.0, 1.0, 0.0,
        ]) +
        append_u16([0, 1, 2])
    )


def make_matrix_scene_gltf(byte_length):
    return {
        "asset": {
            "version": "2.0",
            "generator": "pulp-scene3d-render-packet-matrix-contract",
        },
        "buffers": [{
            "uri": "geometry.bin",
            "byteLength": byte_length,
        }],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36},
            {"buffer": 0, "byteOffset": 36, "byteLength": 6},
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
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
        ],
        "meshes": [{
            "name": "MatrixMesh",
            "primitives": [{
                "attributes": {
                    "POSITION": 0,
                },
                "indices": 1,
            }],
        }],
        "nodes": [
            {
                "name": "RootMatrix",
                "matrix": [
                    2.0, 0.0, 0.0, 0.0,
                    0.0, 3.0, 0.0, 0.0,
                    0.0, 0.0, 4.0, 0.0,
                    10.0, 20.0, 30.0, 1.0,
                ],
                "children": [1],
            },
            {
                "name": "ChildTranslatedMesh",
                "mesh": 0,
                "translation": [1.0, 2.0, 3.0],
            },
        ],
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


def main():
    parser = argparse.ArgumentParser(
        description="Verify matrix/TRS world transforms survive through scene3d inspect render packets.")
    parser.add_argument("--inspect-tool", type=Path, required=True)
    args = parser.parse_args()

    if not args.inspect_tool.exists():
        print(f"inspect_tool_exists=false path={args.inspect_tool}")
        return 2

    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        geometry = make_geometry_bin()
        (temp_path / "geometry.bin").write_bytes(geometry)
        scene_path = temp_path / "matrix.gltf"
        scene_path.write_text(
            json.dumps(make_matrix_scene_gltf(len(geometry))),
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
    require_fields(require_single_line(lines, "nodes=", "stats"),
                   {
                       "nodes": "2",
                       "roots": "1",
                       "meshes": "1",
                       "primitives": "1",
                       "indexed_primitives": "1",
                       "vertices": "3",
                       "indices": "3",
                       "materials": "0",
                       "textures": "0",
                       "texture_samplers": "0",
                       "texture_bytes": "0",
                       "advanced_material_extensions": "0",
                       "cameras": "0",
                       "lights": "0",
                       "animations": "0",
                       "unsupported_features": "0",
                       "diagnostics": "0",
                       "error_diagnostics": "0",
                   },
                   "stats")
    require_fields(require_single_line(lines, "render_packet ", "render_packet"),
                   {
                       "transformed_nodes": "2",
                       "primitives": "1",
                       "diagnostics": "0",
                       "has_errors": "false",
                   },
                   "render_packet")
    require_fields(require_single_line(lines, "primitive ", "primitive"),
                   {
                       "node": "1",
                       "mesh": "0",
                       "primitive": "0",
                       "material": "4294967295",
                       "feature_mask": "132",
                       "world_transform": EXPECTED_WORLD_TRANSFORM,
                       "features": "indexed,material_fallback",
                   },
                   "primitive")

    print("scene3d_render_packet_matrix_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
