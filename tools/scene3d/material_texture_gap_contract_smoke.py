#!/usr/bin/env python3
import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from scene3d_launch import argv_for


EXPECTED_FEATURES = [
    ("MaterialTexture:normalTangents", "TextureGapMat"),
    ("MaterialTexture:normalScale", "TextureGapMat"),
]

REJECTED_FEATURES = [
    "MaterialTextureTransform:nonBaseColor",
    "MaterialTexcoord:nonBaseColor",
    "MaterialTexture:occlusionStrength",
]


def append_f32(values):
    return b"".join(struct.pack("<f", value) for value in values)


def append_u16(values):
    return b"".join(struct.pack("<H", value) for value in values)


def make_texture_gap_bin():
    return (
        append_f32([
            -1.0, -1.0, 0.0,
            1.0, -1.0, 0.0,
            0.0, 1.0, 0.0,
        ]) +
        append_f32([
            0.0, 0.0, 1.0,
            0.0, 0.0, 1.0,
            0.0, 0.0, 1.0,
        ]) +
        append_f32([
            0.0, 0.0,
            1.0, 0.0,
            0.5, 1.0,
        ]) +
        append_f32([
            0.5, 0.5,
            0.5, 0.5,
            0.5, 0.5,
        ]) +
        append_u16([0, 1, 2]) +
        b"\x00\x00" +
        b"\x89PNG\r\n\x1a\n"
    )


def make_texture_gap_gltf(byte_length):
    return {
        "asset": {
            "version": "2.0",
            "generator": "pulp-scene3d-material-texture-gap-contract",
        },
        "extensionsUsed": ["KHR_texture_transform"],
        "buffers": [{
            "uri": "texture-gap.bin",
            "byteLength": byte_length,
        }],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
            {"buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962},
            {"buffer": 0, "byteOffset": 72, "byteLength": 24, "target": 34962},
            {"buffer": 0, "byteOffset": 96, "byteLength": 24, "target": 34962},
            {"buffer": 0, "byteOffset": 120, "byteLength": 6, "target": 34963},
            {"buffer": 0, "byteOffset": 128, "byteLength": 8},
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
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 4, "componentType": 5123, "count": 3, "type": "SCALAR"},
        ],
        "images": [
            {"bufferView": 5, "mimeType": "image/png", "name": "base"},
            {"bufferView": 5, "mimeType": "image/png", "name": "metalrough"},
            {"bufferView": 5, "mimeType": "image/png", "name": "normal"},
            {"bufferView": 5, "mimeType": "image/png", "name": "occlusion"},
            {"bufferView": 5, "mimeType": "image/png", "name": "emissive"},
        ],
        "textures": [
            {"source": 0},
            {"source": 1},
            {"source": 2},
            {"source": 3},
            {"source": 4},
        ],
        "materials": [{
            "name": "TextureGapMat",
            "pbrMetallicRoughness": {
                "baseColorTexture": {
                    "index": 0,
                    "texCoord": 0,
                },
                "metallicRoughnessTexture": {
                    "index": 1,
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
            },
            "normalTexture": {
                "index": 2,
                "texCoord": 1,
                "scale": 0.65,
            },
            "occlusionTexture": {
                "index": 3,
                "texCoord": 1,
                "strength": 0.4,
            },
            "emissiveTexture": {
                "index": 4,
                "texCoord": 1,
            },
        }],
        "meshes": [{
            "name": "TextureGapMesh",
            "primitives": [{
                "attributes": {
                    "POSITION": 0,
                    "NORMAL": 1,
                    "TEXCOORD_0": 2,
                    "TEXCOORD_1": 3,
                },
                "indices": 4,
                "material": 0,
                "mode": 4,
            }],
        }],
        "nodes": [{
            "name": "TextureGapNode",
            "mesh": 0,
        }],
        "scenes": [{
            "nodes": [0],
        }],
        "scene": 0,
    }


def run_command(command, expected_code=0):
    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    sys.stdout.write(result.stdout)
    if result.returncode != expected_code:
        raise RuntimeError(
            f"{command[0]} exited with code {result.returncode}; expected {expected_code}")
    return result.stdout


def parse_stats(output):
    stats = {}
    for token in output.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        stats[key] = value
    return stats


def require(condition, message):
    if not condition:
        raise ValueError(message)


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
        "texture_samplers": "0",
        "texture_bytes": "40",
        "advanced_material_extensions": "0",
        "cameras": "0",
        "lights": "0",
        "animations": "0",
        "unsupported_features": "0",
        "diagnostics": "0",
        "error_diagnostics": "0",
    }
    for key, value in expected.items():
        require(stats.get(key) == value,
                f"inspect stat {key}: expected {value}, got {stats.get(key)}")


def require_sidecar(sidecar):
    require(sidecar.get("schema_version") == 1, "schema_version must be 1")
    require(sidecar.get("diagnostics") == [],
            "sidecar must not carry loader diagnostics for embedded PNG texture slots")
    unsupported = sidecar.get("unsupported_features")
    require(isinstance(unsupported, list) and len(unsupported) == len(EXPECTED_FEATURES),
            "sidecar must classify the remaining material texture native gaps")
    actual = [(item.get("feature"), item.get("node_path")) for item in unsupported]
    require(actual == EXPECTED_FEATURES,
            f"material texture gap rows drifted: expected {EXPECTED_FEATURES!r}, got {actual!r}")
    unsupported_features = [item.get("feature") for item in unsupported]
    for feature in REJECTED_FEATURES:
        require(feature not in unsupported_features,
                f"sidecar carried stale supported material texture gap: {feature}")


def require_preflight(output):
    expected = [
        "bake_readiness=native_gaps",
        "export_blocked=false",
        "texture_encoding_blocked=false",
        "native_runtime_has_gaps=true",
        "has_error_diagnostics=false",
        f"export_blockers=0 texture_encoding_blockers=0 native_runtime_gaps={len(EXPECTED_FEATURES)} diagnostics=0",
    ]
    for feature, node_path in EXPECTED_FEATURES:
        expected.append(f"native_runtime_gap: {feature} node={node_path}")
    for text in expected:
        require(text in output, f"preflight output missing: {text}")
    for feature in REJECTED_FEATURES:
        require(feature not in output,
                f"preflight output carried stale supported material texture gap: {feature}")


def main():
    parser = argparse.ArgumentParser(
        description="Verify material texture routing gaps flow through inspect, sidecar, and preflight.")
    parser.add_argument("--inspect-tool", type=Path, required=True)
    parser.add_argument("--sidecar-tool", type=Path, required=True)
    parser.add_argument("--preflight-tool", type=Path, required=True)
    args = parser.parse_args()

    for label, path in (
        ("inspect_tool", args.inspect_tool),
        ("sidecar_tool", args.sidecar_tool),
        ("preflight_tool", args.preflight_tool),
    ):
        if not path.exists():
            print(f"{label}_exists=false path={path}")
            return 2

    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        bin_bytes = make_texture_gap_bin()
        (temp_path / "texture-gap.bin").write_bytes(bin_bytes)
        scene_path = temp_path / "material-texture-gap.gltf"
        scene_path.write_text(json.dumps(make_texture_gap_gltf(len(bin_bytes))),
                              encoding="utf-8")

        inspect_output = run_command([*argv_for(args.inspect_tool), str(scene_path)])
        require_stats(parse_stats(inspect_output))
        print("scene3d_material_texture_gap_inspect_verified=true")

        sidecar_json = run_command([
            *argv_for(args.sidecar_tool),
            "--source",
            "pulp-material-texture-gap-contract",
            "--exported-at",
            "2026-06-03T00:00:00Z",
            "--runtime-evidence",
            "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927",
            str(scene_path),
        ])
        sidecar = json.loads(sidecar_json)
        require_sidecar(sidecar)
        print("scene3d_material_texture_gap_sidecar_verified=true")

        sidecar_path = temp_path / "material-texture-gap.pulp3d.json"
        sidecar_path.write_text(sidecar_json, encoding="utf-8")
        preflight_output = run_command([*argv_for(args.preflight_tool), str(sidecar_path)])
        require_preflight(preflight_output)
        print("scene3d_material_texture_gap_preflight_verified=true")

    print("scene3d_material_texture_gap_contract_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
