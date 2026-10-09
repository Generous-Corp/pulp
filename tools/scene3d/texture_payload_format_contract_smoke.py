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
    ("TextureFormat:image/ktx2", "basisPayload"),
    ("TexturePayload", "emptyPng"),
]


def append_f32(values):
    return b"".join(struct.pack("<f", value) for value in values)


def make_texture_payload_bin():
    return (
        append_f32([
            -1.0, -1.0, 0.0,
            1.0, -1.0, 0.0,
            0.0, 1.0, 0.0,
        ]) +
        b"\xabKTX 20\xbb\r\n\x1a\n"
    )


def make_texture_payload_gltf(byte_length):
    return {
        "asset": {
            "version": "2.0",
            "generator": "pulp-scene3d-texture-payload-format-contract",
        },
        "buffers": [{
            "uri": "texture-payload.bin",
            "byteLength": byte_length,
        }],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
            {"buffer": 0, "byteOffset": 36, "byteLength": 12},
            {"buffer": 0, "byteOffset": 48, "byteLength": 0},
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
        ],
        "images": [
            {
                "bufferView": 1,
                "mimeType": "image/ktx2",
                "name": "basisPayload",
            },
            {
                "bufferView": 2,
                "mimeType": "image/png",
                "name": "emptyPng",
            },
        ],
        "textures": [
            {"source": 0},
            {"source": 1},
        ],
        "meshes": [{
            "name": "PositionOnly",
            "primitives": [{
                "attributes": {
                    "POSITION": 0,
                },
                "mode": 4,
            }],
        }],
        "nodes": [{
            "name": "PositionOnlyNode",
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
        "materials": "0",
        "textures": "2",
        "texture_samplers": "0",
        "texture_bytes": "12",
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
            "sidecar must not carry loader diagnostics for embedded texture payload gaps")

    unsupported = sidecar.get("unsupported_features")
    require(isinstance(unsupported, list) and len(unsupported) == len(EXPECTED_FEATURES),
            "sidecar must classify texture payload and texture format gaps")
    actual = [(item.get("feature"), item.get("node_path")) for item in unsupported]
    require(actual == EXPECTED_FEATURES,
            f"texture payload/format rows drifted: expected {EXPECTED_FEATURES!r}, got {actual!r}")


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


def main():
    parser = argparse.ArgumentParser(
        description="Verify texture payload and texture format gaps flow through inspect, sidecar, and preflight.")
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
        bin_bytes = make_texture_payload_bin()
        (temp_path / "texture-payload.bin").write_bytes(bin_bytes)
        scene_path = temp_path / "texture-payload-format.gltf"
        scene_path.write_text(json.dumps(make_texture_payload_gltf(len(bin_bytes))),
                              encoding="utf-8")

        inspect_output = run_command([*argv_for(args.inspect_tool), str(scene_path)])
        require_stats(parse_stats(inspect_output))
        print("scene3d_texture_payload_format_inspect_verified=true")

        sidecar_json = run_command([
            *argv_for(args.sidecar_tool),
            "--source",
            "pulp-texture-payload-format-contract",
            "--exported-at",
            "2026-06-03T00:00:00Z",
            "--runtime-evidence",
            "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927",
            str(scene_path),
        ])
        sidecar = json.loads(sidecar_json)
        require_sidecar(sidecar)
        print("scene3d_texture_payload_format_sidecar_verified=true")

        sidecar_path = temp_path / "texture-payload-format.pulp3d.json"
        sidecar_path.write_text(sidecar_json, encoding="utf-8")
        preflight_output = run_command([*argv_for(args.preflight_tool), str(sidecar_path)])
        require_preflight(preflight_output)
        print("scene3d_texture_payload_format_preflight_verified=true")

    print("scene3d_texture_payload_format_contract_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
