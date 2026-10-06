#!/usr/bin/env python3
import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from scene3d_launch import argv_for


EXPECTED_FEATURES = {
    ("MorphWeights", "MorphMesh"),
    ("MorphTargets", "MorphMesh"),
    ("Skinning", "SkinnedMorphed"),
    ("GpuInstancing", "InstancedNode"),
}


def append_f32(values):
    return b"".join(struct.pack("<f", value) for value in values)


def append_u16(values):
    return b"".join(struct.pack("<H", value) for value in values)


def make_unsupported_bin():
    return (
        append_f32([
            -1.0, -1.0, 0.0,
            1.0, -1.0, 0.0,
            0.0, 1.0, 0.0,
        ]) +
        append_f32([
            -0.8, -1.0, 0.0,
            1.2, -1.0, 0.0,
            0.0, 1.2, 0.0,
        ]) +
        append_f32([
            0.0, 0.0, 0.0,
            2.0, 0.0, 0.0,
        ]) +
        append_u16([0, 1, 2]) +
        append_u16([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]) +
        append_f32([
            1.0, 0.0, 0.0, 0.0,
            1.0, 0.0, 0.0, 0.0,
            1.0, 0.0, 0.0, 0.0,
        ])
    )


def make_unsupported_gltf(byte_length):
    return {
        "asset": {
            "version": "2.0",
            "generator": "pulp-scene3d-unsupported-mesh-contract",
        },
        "extensionsUsed": ["EXT_mesh_gpu_instancing"],
        "buffers": [{
            "uri": "unsupported.bin",
            "byteLength": byte_length,
        }],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
            {"buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962},
            {"buffer": 0, "byteOffset": 72, "byteLength": 24, "target": 34962},
            {"buffer": 0, "byteOffset": 96, "byteLength": 6, "target": 34963},
            {"buffer": 0, "byteOffset": 102, "byteLength": 24, "target": 34962},
            {"buffer": 0, "byteOffset": 126, "byteLength": 48, "target": 34962},
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
            {
                "bufferView": 1,
                "componentType": 5126,
                "count": 3,
                "type": "VEC3",
                "min": [-0.8, -1.0, 0.0],
                "max": [1.2, 1.2, 0.0],
            },
            {
                "bufferView": 2,
                "componentType": 5126,
                "count": 2,
                "type": "VEC3",
                "min": [0.0, 0.0, 0.0],
                "max": [2.0, 0.0, 0.0],
            },
            {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"},
            {"bufferView": 4, "componentType": 5123, "count": 3, "type": "VEC4"},
            {"bufferView": 5, "componentType": 5126, "count": 3, "type": "VEC4"},
        ],
        "meshes": [{
            "name": "MorphMesh",
            "weights": [0.5],
            "primitives": [{
                "attributes": {
                    "POSITION": 0,
                    "JOINTS_0": 4,
                    "WEIGHTS_0": 5,
                },
                "targets": [{
                    "POSITION": 1,
                }],
                "indices": 3,
                "mode": 4,
            }],
        }],
        "nodes": [
            {"name": "SkinnedMorphed", "mesh": 0, "skin": 0},
            {"name": "Joint"},
            {
                "name": "InstancedNode",
                "mesh": 0,
                "extensions": {
                    "EXT_mesh_gpu_instancing": {
                        "attributes": {
                            "TRANSLATION": 2,
                        },
                    },
                },
            },
        ],
        "skins": [{
            "name": "Skin",
            "joints": [1],
        }],
        "scenes": [{
            "nodes": [0, 1, 2],
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
        "nodes": "3",
        "roots": "3",
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
        "unsupported_features": "4",
        "diagnostics": "4",
        "error_diagnostics": "0",
    }
    for key, value in expected.items():
        require(stats.get(key) == value,
                f"inspect stat {key}: expected {value}, got {stats.get(key)}")


def require_sidecar(sidecar):
    require(sidecar.get("schema_version") == 1, "schema_version must be 1")
    diagnostics = sidecar.get("diagnostics")
    require(isinstance(diagnostics, list) and len(diagnostics) == 4,
            "sidecar must carry four loader warnings")
    codes = {item.get("code") for item in diagnostics}
    require("gltf.mesh_morph_weights_unsupported" in codes,
            "morph weights diagnostic missing")
    require("gltf.primitive_morph_targets_unsupported" in codes,
            "morph targets diagnostic missing")
    require("gltf.node_skin_unsupported" in codes,
            "skinning diagnostic missing")
    require("gltf.node_instancing_unsupported" in codes,
            "GPU instancing diagnostic missing")

    unsupported = sidecar.get("unsupported_features")
    require(isinstance(unsupported, list) and len(unsupported) == 4,
            "sidecar must classify four unsupported mesh native gaps")
    actual = {(item.get("feature"), item.get("node_path")) for item in unsupported}
    require(actual == EXPECTED_FEATURES,
            f"unsupported mesh features drifted: expected {EXPECTED_FEATURES!r}, got {actual!r}")


def require_preflight(output):
    expected = [
        "bake_readiness=native_gaps",
        "export_blocked=false",
        "texture_encoding_blocked=false",
        "native_runtime_has_gaps=true",
        "has_error_diagnostics=false",
        "export_blockers=0 texture_encoding_blockers=0 native_runtime_gaps=4 diagnostics=4",
        "native_runtime_gap: MorphWeights node=MorphMesh",
        "native_runtime_gap: MorphTargets node=MorphMesh",
        "native_runtime_gap: Skinning node=SkinnedMorphed",
        "native_runtime_gap: GpuInstancing node=InstancedNode",
    ]
    for text in expected:
        require(text in output, f"preflight output missing: {text}")


def main():
    parser = argparse.ArgumentParser(
        description="Verify unsupported mesh features flow through inspect, sidecar, and preflight.")
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
        bin_bytes = make_unsupported_bin()
        (temp_path / "unsupported.bin").write_bytes(bin_bytes)
        scene_path = temp_path / "unsupported.gltf"
        scene_path.write_text(
            json.dumps(make_unsupported_gltf(len(bin_bytes))),
            encoding="utf-8")

        inspect_output = run_command([*argv_for(args.inspect_tool), str(scene_path)])
        require_stats(parse_stats(inspect_output))
        print("scene3d_unsupported_mesh_inspect_verified=true")

        sidecar_json = run_command([
            *argv_for(args.sidecar_tool),
            "--source",
            "pulp-unsupported-mesh-contract",
            "--exported-at",
            "2026-06-03T00:00:00Z",
            "--runtime-evidence",
            "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927",
            str(scene_path),
        ])
        sidecar = json.loads(sidecar_json)
        require_sidecar(sidecar)
        print("scene3d_unsupported_mesh_sidecar_verified=true")

        sidecar_path = temp_path / "unsupported.pulp3d.json"
        sidecar_path.write_text(sidecar_json, encoding="utf-8")
        preflight_output = run_command([*argv_for(args.preflight_tool), str(sidecar_path)])
        require_preflight(preflight_output)
        print("scene3d_unsupported_mesh_preflight_verified=true")

    print("scene3d_unsupported_mesh_contract_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
