#!/usr/bin/env python3
import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from scene3d_launch import argv_for


def append_f32(values):
    return b"".join(struct.pack("<f", value) for value in values)


def append_u16(values):
    return b"".join(struct.pack("<H", value) for value in values)


def make_line_bin():
    return (
        append_f32([
            0.0, 0.0, 0.0,
            1.0, 0.0, 0.0,
        ]) +
        append_u16([0, 1])
    )


def make_line_gltf(byte_length):
    return {
        "asset": {
            "version": "2.0",
            "generator": "pulp-scene3d-primitive-mode-contract",
        },
        "buffers": [{
            "uri": "lines.bin",
            "byteLength": byte_length,
        }],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 24, "target": 34962},
            {"buffer": 0, "byteOffset": 24, "byteLength": 4, "target": 34963},
        ],
        "accessors": [
            {
                "bufferView": 0,
                "componentType": 5126,
                "count": 2,
                "type": "VEC3",
                "min": [0.0, 0.0, 0.0],
                "max": [1.0, 0.0, 0.0],
            },
            {"bufferView": 1, "componentType": 5123, "count": 2, "type": "SCALAR"},
        ],
        "meshes": [{
            "name": "LineMesh",
            "primitives": [{
                "attributes": {
                    "POSITION": 0,
                },
                "indices": 1,
                "mode": 1,
            }],
        }],
        "nodes": [{
            "name": "LineNode",
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
        "primitives": "0",
        "indexed_primitives": "0",
        "vertices": "0",
        "indices": "0",
        "materials": "0",
        "textures": "0",
        "texture_samplers": "0",
        "texture_bytes": "0",
        "advanced_material_extensions": "0",
        "cameras": "0",
        "lights": "0",
        "animations": "0",
        "unsupported_features": "1",
        "diagnostics": "2",
        "error_diagnostics": "0",
    }
    for key, value in expected.items():
        require(stats.get(key) == value,
                f"inspect stat {key}: expected {value}, got {stats.get(key)}")


def require_sidecar(sidecar):
    require(sidecar.get("schema_version") == 1, "schema_version must be 1")
    diagnostics = sidecar.get("diagnostics")
    require(isinstance(diagnostics, list) and len(diagnostics) == 3,
            "sidecar must carry loader diagnostics plus sidecar validation diagnostics")
    codes = [(item.get("severity"), item.get("code")) for item in diagnostics]
    require(("warning", "gltf.primitive_unsupported_mode") in codes,
            "primitive mode loader diagnostic missing")
    mesh_without_primitives = [
        row for row in codes
        if row == ("warning", "scene.mesh_without_primitives")
    ]
    require(len(mesh_without_primitives) == 2,
            "sidecar must carry both loader and sidecar empty-mesh validation diagnostics")

    unsupported = sidecar.get("unsupported_features")
    require(isinstance(unsupported, list) and len(unsupported) == 1,
            "sidecar must classify one primitive-mode native gap")
    require(unsupported[0].get("feature") == "PrimitiveMode:Lines",
            "PrimitiveMode:Lines unsupported feature missing")
    require(unsupported[0].get("node_path") == "LineMesh",
            "PrimitiveMode:Lines node path drifted")


def require_preflight(output):
    expected = [
        "bake_readiness=native_gaps",
        "export_blocked=false",
        "native_runtime_has_gaps=true",
        "has_error_diagnostics=false",
        "export_blockers=0",
        "texture_encoding_blockers=0",
        "native_runtime_gaps=1",
        "diagnostics=3",
        "native_runtime_gap: PrimitiveMode:Lines node=LineMesh",
    ]
    for text in expected:
        require(text in output, f"preflight output missing: {text}")


def main():
    parser = argparse.ArgumentParser(
        description="Verify non-triangle primitive modes flow through inspect, sidecar, and preflight.")
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
        bin_bytes = make_line_bin()
        (temp_path / "lines.bin").write_bytes(bin_bytes)
        scene_path = temp_path / "lines.gltf"
        scene_path.write_text(
            json.dumps(make_line_gltf(len(bin_bytes))),
            encoding="utf-8")

        inspect_output = run_command([*argv_for(args.inspect_tool), str(scene_path)])
        require_stats(parse_stats(inspect_output))
        print("scene3d_primitive_mode_inspect_verified=true")

        sidecar_json = run_command([
            *argv_for(args.sidecar_tool),
            "--source",
            "pulp-primitive-mode-contract",
            "--exported-at",
            "2026-06-03T00:00:00Z",
            "--runtime-evidence",
            "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927",
            str(scene_path),
        ])
        sidecar = json.loads(sidecar_json)
        require_sidecar(sidecar)
        print("scene3d_primitive_mode_sidecar_verified=true")

        sidecar_path = temp_path / "lines.pulp3d.json"
        sidecar_path.write_text(sidecar_json, encoding="utf-8")
        preflight_output = run_command([*argv_for(args.preflight_tool), str(sidecar_path)])
        require_preflight(preflight_output)
        print("scene3d_primitive_mode_preflight_verified=true")

    print("scene3d_primitive_mode_contract_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
