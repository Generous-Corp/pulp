#!/usr/bin/env python3
import argparse
import subprocess
import sys
from pathlib import Path
from scene3d_launch import argv_for


EXPORTED_AT = "2026-06-03T00:00:00Z"
RUNTIME_EVIDENCE = "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927"


def run_command(command):
    return subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def expect_case(name, result, expected_code, fragments, errors):
    sys.stdout.write(result.stdout)
    if result.returncode != expected_code:
        errors.append(
            f"{name}: expected exit {expected_code}, got {result.returncode}"
        )
    for fragment in fragments:
        if fragment not in result.stdout:
            errors.append(
                f"{name}: expected output containing {fragment!r}"
            )
    print(f"scene3d_sidecar_cli_contract_case={name}")


def main():
    parser = argparse.ArgumentParser(
        description="Verify pulp-scene3d-sidecar CLI provenance contract.")
    parser.add_argument("--sidecar-tool", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()

    if not args.sidecar_tool.exists():
        print(f"sidecar_tool_exists=false path={args.sidecar_tool}")
        return 2
    if not args.fixture.exists():
        print(f"fixture_exists=false path={args.fixture}")
        return 2

    errors = []

    valid = run_command([
        *argv_for(args.sidecar_tool),
        "--source",
        "khronos-boxtextured",
        "--exported-at",
        EXPORTED_AT,
        "--runtime-evidence",
        RUNTIME_EVIDENCE,
        str(args.fixture),
    ])
    expect_case(
        "valid-explicit-provenance",
        valid,
        0,
        [
            '"source": "khronos-boxtextured"',
            '"exporter": "pulp-scene3d-sidecar"',
            f'"exported_at": "{EXPORTED_AT}"',
            f'"runtime_evidence": "{RUNTIME_EVIDENCE}"',
        ],
        errors,
    )

    missing_exported_at = run_command([
        *argv_for(args.sidecar_tool),
        "--source",
        "khronos-boxtextured",
        str(args.fixture),
    ])
    expect_case(
        "missing-exported-at",
        missing_exported_at,
        64,
        ["--exported-at must be non-empty"],
        errors,
    )

    empty_exported_at = run_command([
        *argv_for(args.sidecar_tool),
        "--source",
        "khronos-boxtextured",
        "--exported-at",
        "",
        str(args.fixture),
    ])
    expect_case(
        "empty-exported-at",
        empty_exported_at,
        64,
        ["--exported-at must be non-empty"],
        errors,
    )

    empty_exporter = run_command([
        *argv_for(args.sidecar_tool),
        "--exporter",
        "",
        "--exported-at",
        EXPORTED_AT,
        str(args.fixture),
    ])
    expect_case(
        "empty-exporter",
        empty_exporter,
        64,
        ["--exporter must be non-empty"],
        errors,
    )

    empty_source_defaults = run_command([
        *argv_for(args.sidecar_tool),
        "--source",
        "",
        "--exported-at",
        EXPORTED_AT,
        str(args.fixture),
    ])
    expect_case(
        "empty-source-defaults-to-path",
        empty_source_defaults,
        0,
        [
            f'"source": "{args.fixture}"',
            f'"exported_at": "{EXPORTED_AT}"',
        ],
        errors,
    )

    if errors:
        for error in errors:
            print(f"error={error}", file=sys.stderr)
        return 1

    print("scene3d_sidecar_cli_contract_verified=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
