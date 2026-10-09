#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from scene3d_launch import argv_for


def run_command(command):
    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr, end="")
        raise RuntimeError(
            f"{command[0]} exited with code {result.returncode}")
    return result.stdout


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    exported_at = "2026-06-03T00:00:00Z"
    parser = argparse.ArgumentParser(
        description="Round-trip native scene3d sidecar output through preflight.")
    parser.add_argument("--sidecar-tool", type=Path, required=True)
    parser.add_argument("--preflight-tool", type=Path, required=True)
    parser.add_argument("--runtime-evidence", required=True)
    parser.add_argument("scene", type=Path)
    args = parser.parse_args()

    sidecar_json = run_command([
        *argv_for(args.sidecar_tool),
        "--source",
        "khronos-boxtextured",
        "--exported-at",
        exported_at,
        "--runtime-evidence",
        args.runtime_evidence,
        str(args.scene),
    ])
    sidecar = json.loads(sidecar_json)
    require(sidecar.get("schema_version") == 1, "schema_version must be 1")
    provenance = sidecar.get("provenance")
    require(isinstance(provenance, dict), "provenance must be an object")
    require(provenance.get("source") == "khronos-boxtextured",
            "source provenance mismatch")
    require(provenance.get("exporter") == "pulp-scene3d-sidecar",
            "exporter provenance mismatch")
    require(provenance.get("exported_at") == exported_at,
            "exported_at provenance mismatch")
    require(provenance.get("runtime_evidence") == args.runtime_evidence,
            "runtime evidence mismatch")
    require(sidecar.get("diagnostics") == [], "diagnostics must be empty")
    require(sidecar.get("unsupported_features") == [],
            "unsupported_features must be empty")
    require(sidecar.get("runtime_hints") == [], "runtime_hints must be empty")

    with tempfile.TemporaryDirectory() as temp_dir:
        sidecar_path = Path(temp_dir) / "BoxTextured.pulp3d.json"
        sidecar_path.write_text(sidecar_json, encoding="utf-8")
        preflight = run_command([
            *argv_for(args.preflight_tool),
            "--require-runtime-evidence",
            str(sidecar_path),
        ])

    expected = [
        "bake_readiness=clean",
        "export_blocked=false",
        "texture_encoding_blocked=false",
        "native_runtime_has_gaps=false",
        "has_error_diagnostics=false",
        "runtime_evidence_missing=false",
        "export_blockers=0 texture_encoding_blockers=0 native_runtime_gaps=0 diagnostics=0",
    ]
    for text in expected:
        require(text in preflight, f"preflight output missing: {text}")

    print("BoxTextured sidecar preflight clean")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
