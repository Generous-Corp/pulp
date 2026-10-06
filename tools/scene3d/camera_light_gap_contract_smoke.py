#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from scene3d_launch import argv_for


EXPECTED_FEATURES = [
    ("PunctualLight:additionalDirectional", "FillDir"),
    ("PunctualLight:additionalDirectional", "DuplicateKeyDirNode"),
    ("PunctualLight:additionalPoint", "PointB"),
    ("PunctualLight:additionalSpot", "SpotB"),
    ("PunctualLight:unattachedDirectional", "UnattachedDir"),
    ("PunctualLight:unattachedPoint", "UnattachedPoint"),
    ("PunctualLight:unattachedSpot", "UnattachedSpot"),
    ("Camera:nodeTransform", "MainCameraNode"),
    ("Camera:additionalPerspective", "SecondPerspective"),
    ("Camera:additionalPerspective", "DuplicateMainCameraNode"),
    ("Camera:additionalOrthographic", "OrthoCamera"),
]


def make_camera_light_gap_gltf():
    return {
        "asset": {
            "version": "2.0",
            "generator": "pulp-scene3d-camera-light-gap-contract",
        },
        "extensionsUsed": ["KHR_lights_punctual"],
        "extensions": {
            "KHR_lights_punctual": {
                "lights": [
                    {"name": "KeyDir", "type": "directional"},
                    {"name": "FillDir", "type": "directional"},
                    {"name": "PointA", "type": "point", "range": 4.0},
                    {"name": "PointB", "type": "point", "range": 8.0},
                    {"name": "SpotA", "type": "spot", "spot": {"outerConeAngle": 0.8}},
                    {"name": "SpotB", "type": "spot", "spot": {"outerConeAngle": 0.6}},
                    {"name": "UnattachedDir", "type": "directional"},
                    {"name": "UnattachedPoint", "type": "point"},
                    {"name": "UnattachedSpot", "type": "spot", "spot": {"outerConeAngle": 0.7}},
                ],
            },
        },
        "cameras": [
            {
                "name": "MainCamera",
                "type": "perspective",
                "perspective": {
                    "yfov": 0.75,
                    "znear": 0.1,
                    "zfar": 100.0,
                },
            },
            {
                "name": "SecondPerspective",
                "type": "perspective",
                "perspective": {
                    "yfov": 0.6,
                    "znear": 0.2,
                },
            },
            {
                "name": "OrthoCamera",
                "type": "orthographic",
                "orthographic": {
                    "xmag": 2.0,
                    "ymag": 2.0,
                    "znear": 0.01,
                    "zfar": 10.0,
                },
            },
        ],
        "nodes": [
            {
                "name": "MainCameraNode",
                "camera": 0,
                "scale": [2.0, 1.0, 1.0],
            },
            {
                "name": "SecondCameraNode",
                "camera": 1,
            },
            {
                "name": "DuplicateMainCameraNode",
                "camera": 0,
            },
            {
                "name": "KeyDirNode",
                "extensions": {"KHR_lights_punctual": {"light": 0}},
            },
            {
                "name": "FillDirNode",
                "extensions": {"KHR_lights_punctual": {"light": 1}},
            },
            {
                "name": "DuplicateKeyDirNode",
                "extensions": {"KHR_lights_punctual": {"light": 0}},
            },
            {
                "name": "PointANode",
                "extensions": {"KHR_lights_punctual": {"light": 2}},
            },
            {
                "name": "PointBNode",
                "extensions": {"KHR_lights_punctual": {"light": 3}},
            },
            {
                "name": "SpotANode",
                "extensions": {"KHR_lights_punctual": {"light": 4}},
            },
            {
                "name": "SpotBNode",
                "extensions": {"KHR_lights_punctual": {"light": 5}},
            },
        ],
        "scenes": [{
            "nodes": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9],
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
        "nodes": "10",
        "roots": "10",
        "meshes": "0",
        "primitives": "0",
        "indexed_primitives": "0",
        "vertices": "0",
        "indices": "0",
        "materials": "0",
        "textures": "0",
        "texture_samplers": "0",
        "texture_bytes": "0",
        "advanced_material_extensions": "0",
        "cameras": "3",
        "lights": "9",
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
    require(sidecar.get("diagnostics") == [], "diagnostics must be empty")
    unsupported = sidecar.get("unsupported_features")
    require(isinstance(unsupported, list) and len(unsupported) == len(EXPECTED_FEATURES),
            "sidecar must classify camera/light native gaps")
    actual = [(item.get("feature"), item.get("node_path")) for item in unsupported]
    require(actual == EXPECTED_FEATURES,
            f"camera/light gap rows drifted: expected {EXPECTED_FEATURES!r}, got {actual!r}")
    hints = sidecar.get("runtime_hints")
    require({"key": "preferredCamera", "value": "MainCameraNode"} in hints,
            "preferredCamera runtime hint missing")
    require({"key": "lightCount", "value": "9"} in hints,
            "lightCount runtime hint missing")


def require_preflight(output):
    expected = [
        "bake_readiness=native_gaps",
        "export_blocked=false",
        "texture_encoding_blocked=false",
        "native_runtime_has_gaps=true",
        "has_error_diagnostics=false",
        "runtime_evidence_missing=false",
        f"export_blockers=0 texture_encoding_blockers=0 native_runtime_gaps={len(EXPECTED_FEATURES)} diagnostics=0",
    ]
    for feature, node_path in EXPECTED_FEATURES:
        expected.append(f"native_runtime_gap: {feature} node={node_path}")
    for text in expected:
        require(text in output, f"preflight output missing: {text}")


def main():
    parser = argparse.ArgumentParser(
        description="Verify additional/unattached camera and light gaps flow through inspect, sidecar, and preflight.")
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
        scene_path = temp_path / "camera-light-gap.gltf"
        scene_path.write_text(json.dumps(make_camera_light_gap_gltf()),
                              encoding="utf-8")

        inspect_output = run_command([*argv_for(args.inspect_tool), str(scene_path)])
        require_stats(parse_stats(inspect_output))
        print("scene3d_camera_light_gap_inspect_verified=true")

        sidecar_json = run_command([
            *argv_for(args.sidecar_tool),
            "--source",
            "pulp-camera-light-gap-contract",
            "--exported-at",
            "2026-06-03T00:00:00Z",
            "--runtime-evidence",
            "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927",
            str(scene_path),
        ])
        sidecar = json.loads(sidecar_json)
        require_sidecar(sidecar)
        print("scene3d_camera_light_gap_sidecar_verified=true")

        sidecar_path = temp_path / "camera-light-gap.pulp3d.json"
        sidecar_path.write_text(sidecar_json, encoding="utf-8")
        preflight_output = run_command([*argv_for(args.preflight_tool), str(sidecar_path)])
        require_preflight(preflight_output)
        print("scene3d_camera_light_gap_preflight_verified=true")

    print("scene3d_camera_light_gap_contract_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
