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


def make_animation_bin():
    data = bytearray()
    data += append_f32([0.0, 1.0])
    data += append_f32([0.0, 0.0, 0.0, 2.0, 3.0, 4.0])
    data += append_f32([0.0, 0.0, 0.0, 1.0, 0.0, 0.70710677, 0.0, 0.70710677])
    data += append_f32([1.0, 1.0, 1.0, 2.0, 2.0, 2.0])
    data += append_f32([0.0, 1.0, 1.0, 0.0])
    return bytes(data)


def make_animation_gltf(byte_length):
    return {
        "asset": {
            "version": "2.0",
            "generator": "pulp-scene3d-animation-contract",
        },
        "buffers": [{
            "uri": "anim.bin",
            "byteLength": byte_length,
        }],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 8},
            {"buffer": 0, "byteOffset": 8, "byteLength": 24},
            {"buffer": 0, "byteOffset": 32, "byteLength": 32},
            {"buffer": 0, "byteOffset": 64, "byteLength": 24},
            {"buffer": 0, "byteOffset": 88, "byteLength": 16},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 2, "type": "SCALAR",
             "min": [0], "max": [1]},
            {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 2, "type": "VEC4"},
            {"bufferView": 3, "componentType": 5126, "count": 2, "type": "VEC3"},
            {"bufferView": 4, "componentType": 5126, "count": 2, "type": "VEC2"},
        ],
        "nodes": [{
            "name": "animated",
        }],
        "animations": [{
            "name": "MoveSpinScale",
            "samplers": [
                {"input": 0, "output": 1, "interpolation": "LINEAR"},
                {"input": 0, "output": 2, "interpolation": "STEP"},
                {"input": 0, "output": 3, "interpolation": "LINEAR"},
                {"input": 0, "output": 4, "interpolation": "LINEAR"},
            ],
            "channels": [
                {"sampler": 0, "target": {"node": 0, "path": "translation"}},
                {"sampler": 1, "target": {"node": 0, "path": "rotation"}},
                {"sampler": 2, "target": {"node": 0, "path": "scale"}},
                {"sampler": 3, "target": {"node": 0, "path": "weights"}},
            ],
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
        "meshes": "0",
        "primitives": "0",
        "animations": "1",
        "unsupported_features": "1",
        "diagnostics": "1",
        "error_diagnostics": "0",
    }
    for key, value in expected.items():
        require(stats.get(key) == value,
                f"inspect stat {key}: expected {value}, got {stats.get(key)}")


def require_sidecar(sidecar):
    require(sidecar.get("schema_version") == 1, "schema_version must be 1")
    diagnostics = sidecar.get("diagnostics")
    require(isinstance(diagnostics, list) and len(diagnostics) == 1,
            "sidecar must carry one loader diagnostic")
    require(diagnostics[0].get("severity") == "warning",
            "animation path diagnostic must be a warning")
    require(diagnostics[0].get("code") == "gltf.animation_path_unsupported",
            "animation path diagnostic code drifted")

    unsupported = sidecar.get("unsupported_features")
    require(isinstance(unsupported, list) and len(unsupported) == 2,
            "sidecar must classify two animation native gaps")
    rows = {(item.get("feature"), item.get("node_path")) for item in unsupported}
    require(("TransformAnimation", "MoveSpinScale") in rows,
            "TransformAnimation native gap missing")
    require(("AnimationPath:weights", "MoveSpinScale") in rows,
            "weights animation native gap missing")

    hints = sidecar.get("runtime_hints")
    require({"key": "animationCount", "value": "1"} in hints,
            "animationCount runtime hint missing")


def require_preflight(output):
    expected = [
        "bake_readiness=native_gaps",
        "export_blocked=false",
        "native_runtime_has_gaps=true",
        "has_error_diagnostics=false",
        "native_runtime_gaps=2",
        "diagnostics=1",
        "native_runtime_gap: TransformAnimation node=MoveSpinScale",
        "native_runtime_gap: AnimationPath:weights node=MoveSpinScale",
    ]
    for text in expected:
        require(text in output, f"preflight output missing: {text}")


def main():
    parser = argparse.ArgumentParser(
        description="Verify animated glTF native handoff stays data-only and classified.")
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
        bin_bytes = make_animation_bin()
        (temp_path / "anim.bin").write_bytes(bin_bytes)
        scene_path = temp_path / "animated.gltf"
        scene_path.write_text(
            json.dumps(make_animation_gltf(len(bin_bytes))),
            encoding="utf-8")

        inspect_output = run_command([*argv_for(args.inspect_tool), str(scene_path)])
        require_stats(parse_stats(inspect_output))
        print("scene3d_animation_inspect_verified=true")

        sidecar_json = run_command([
            *argv_for(args.sidecar_tool),
            "--source",
            "pulp-animation-contract",
            "--exported-at",
            "2026-06-03T00:00:00Z",
            "--runtime-evidence",
            "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927",
            str(scene_path),
        ])
        sidecar = json.loads(sidecar_json)
        require_sidecar(sidecar)
        print("scene3d_animation_sidecar_verified=true")

        sidecar_path = temp_path / "animated.pulp3d.json"
        sidecar_path.write_text(sidecar_json, encoding="utf-8")
        preflight_output = run_command([*argv_for(args.preflight_tool), str(sidecar_path)])
        require_preflight(preflight_output)
        print("scene3d_animation_preflight_verified=true")

    print("scene3d_animation_contract_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
