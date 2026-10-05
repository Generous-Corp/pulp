#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from scene3d_launch import argv_for


EXPECTED_EXTENSIONS = [
    "KHR_materials_clearcoat",
    "KHR_materials_transmission",
    "KHR_materials_sheen",
    "KHR_materials_specular",
    "KHR_materials_volume",
    "KHR_materials_anisotropy",
    "KHR_materials_iridescence",
    "KHR_materials_diffuse_transmission",
    "KHR_materials_ior",
    "KHR_materials_dispersion",
]


def make_advanced_material_gltf():
    return {
        "asset": {
            "version": "2.0",
            "generator": "pulp-scene3d-advanced-material-contract",
        },
        "extensionsUsed": EXPECTED_EXTENSIONS,
        "materials": [{
            "name": "PhysicalMat",
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            },
            "extensions": {
                "KHR_materials_clearcoat": {"clearcoatFactor": 0.5},
                "KHR_materials_transmission": {"transmissionFactor": 0.25},
                "KHR_materials_sheen": {"sheenRoughnessFactor": 0.5},
                "KHR_materials_specular": {"specularFactor": 0.8},
                "KHR_materials_volume": {"thicknessFactor": 0.2},
                "KHR_materials_anisotropy": {"anisotropyStrength": 0.4},
                "KHR_materials_iridescence": {"iridescenceFactor": 0.3},
                "KHR_materials_diffuse_transmission": {"diffuseTransmissionFactor": 0.2},
                "KHR_materials_ior": {"ior": 1.4},
                "KHR_materials_dispersion": {"dispersion": 0.1},
            },
        }],
        "scenes": [{}],
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
        "nodes": "0",
        "roots": "0",
        "meshes": "0",
        "primitives": "0",
        "indexed_primitives": "0",
        "vertices": "0",
        "indices": "0",
        "materials": "1",
        "textures": "0",
        "texture_samplers": "0",
        "texture_bytes": "0",
        "advanced_material_extensions": str(len(EXPECTED_EXTENSIONS)),
        "cameras": "0",
        "lights": "0",
        "animations": "0",
        "unsupported_features": "0",
        "diagnostics": str(len(EXPECTED_EXTENSIONS)),
        "error_diagnostics": "0",
    }
    for key, value in expected.items():
        require(stats.get(key) == value,
                f"inspect stat {key}: expected {value}, got {stats.get(key)}")


def require_sidecar(sidecar):
    require(sidecar.get("schema_version") == 1, "schema_version must be 1")
    diagnostics = sidecar.get("diagnostics")
    require(isinstance(diagnostics, list) and len(diagnostics) == len(EXPECTED_EXTENSIONS),
            "sidecar must carry one warning diagnostic per advanced material extension")
    diagnostic_codes = [item.get("code") for item in diagnostics]
    require(diagnostic_codes == ["gltf.material_extension_deferred"] * len(EXPECTED_EXTENSIONS),
            f"advanced material diagnostic codes drifted: {diagnostic_codes!r}")

    unsupported = sidecar.get("unsupported_features")
    require(isinstance(unsupported, list) and len(unsupported) == len(EXPECTED_EXTENSIONS),
            "sidecar must classify every advanced material extension as a native gap")
    actual = [(item.get("feature"), item.get("node_path")) for item in unsupported]
    expected = [(f"MaterialExtension:{extension}", "PhysicalMat")
                for extension in EXPECTED_EXTENSIONS]
    require(actual == expected,
            f"advanced material unsupported-feature rows drifted: expected {expected!r}, got {actual!r}")


def require_preflight(output):
    expected = [
        "bake_readiness=native_gaps",
        "export_blocked=false",
        "texture_encoding_blocked=false",
        "native_runtime_has_gaps=true",
        "has_error_diagnostics=false",
        f"export_blockers=0 texture_encoding_blockers=0 native_runtime_gaps={len(EXPECTED_EXTENSIONS)} diagnostics={len(EXPECTED_EXTENSIONS)}",
    ]
    for extension in EXPECTED_EXTENSIONS:
        expected.append(
            f"native_runtime_gap: MaterialExtension:{extension} node=PhysicalMat")
    for text in expected:
        require(text in output, f"preflight output missing: {text}")


def main():
    parser = argparse.ArgumentParser(
        description="Verify advanced glTF material extensions flow through inspect, sidecar, and preflight.")
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
        scene_path = temp_path / "advanced-material.gltf"
        scene_path.write_text(json.dumps(make_advanced_material_gltf()), encoding="utf-8")

        inspect_output = run_command([*argv_for(args.inspect_tool), str(scene_path)])
        require_stats(parse_stats(inspect_output))
        print("scene3d_advanced_material_inspect_verified=true")

        sidecar_json = run_command([
            *argv_for(args.sidecar_tool),
            "--source",
            "pulp-advanced-material-contract",
            "--exported-at",
            "2026-06-03T00:00:00Z",
            "--runtime-evidence",
            "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927",
            str(scene_path),
        ])
        sidecar = json.loads(sidecar_json)
        require_sidecar(sidecar)
        print("scene3d_advanced_material_sidecar_verified=true")

        sidecar_path = temp_path / "advanced-material.pulp3d.json"
        sidecar_path.write_text(sidecar_json, encoding="utf-8")
        preflight_output = run_command([*argv_for(args.preflight_tool), str(sidecar_path)])
        require_preflight(preflight_output)
        print("scene3d_advanced_material_preflight_verified=true")

    print("scene3d_advanced_material_contract_verified=true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
