#!/usr/bin/env python3
"""Run the real GPU trace integration test or report a CTest skip."""

from __future__ import annotations

import argparse
import os
import platform
import subprocess
import sys
from collections.abc import Mapping
from pathlib import Path

SKIP_RETURN_CODE = 77
PINNED_VERSION = "v57.2"
PLATFORMS = {
    ("Darwin", "arm64"): "mac-arm64",
    ("Darwin", "aarch64"): "mac-arm64",
    ("Darwin", "x86_64"): "mac-amd64",
    ("Linux", "x86_64"): "linux-amd64",
    ("Linux", "amd64"): "linux-amd64",
    ("Linux", "aarch64"): "linux-arm64",
    ("Linux", "arm64"): "linux-arm64",
}


class InvalidProcessorOverride(ValueError):
    """An explicit processor path was supplied but is not executable."""


def _usable_file(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def resolve_trace_processor(
    environment: Mapping[str, str] = os.environ,
    *,
    system: str | None = None,
    machine: str | None = None,
) -> Path | None:
    """Resolve the explicit or SDK-matched processor used by the Rust test."""
    explicit = environment.get("PULP_TRACE_PROCESSOR")
    if explicit is not None:
        path = Path(explicit)
        if not _usable_file(path):
            raise InvalidProcessorOverride(
                f"PULP_TRACE_PROCESSOR is not an executable file: {path}"
            )
        return path

    platform_key = (system or platform.system(), machine or platform.machine())
    platform_name = PLATFORMS.get(platform_key)
    if platform_name is None:
        return None
    if pulp_home := environment.get("PULP_HOME"):
        home = Path(pulp_home)
    elif user_home := environment.get("HOME"):
        home = Path(user_home) / ".pulp"
    else:
        return None
    candidate = (
        home
        / "tools"
        / "trace-processor"
        / PINNED_VERSION
        / platform_name
        / "trace_processor_shell"
    )
    return candidate if _usable_file(candidate) else None


def run_integration(
    *,
    cargo: str,
    target_dir: Path,
    manifest: Path,
    environment: Mapping[str, str] = os.environ,
) -> int:
    """Execute Cargo with one resolved processor and preserve its exit code."""
    try:
        processor = resolve_trace_processor(environment)
    except InvalidProcessorOverride as error:
        print(f"GPU trace integration configuration error: {error}", file=sys.stderr)
        return 1
    if processor is None:
        print(
            "GPU trace integration skipped: the SDK-matched trace_processor is "
            "not installed; run `pulp trace fetch` or set PULP_TRACE_PROCESSOR",
            file=sys.stderr,
        )
        return SKIP_RETURN_CODE

    child_environment = dict(environment)
    child_environment["CARGO_TARGET_DIR"] = str(target_dir)
    child_environment["PULP_TRACE_PROCESSOR"] = str(processor)
    completed = subprocess.run(
        [
            cargo,
            "test",
            "--manifest-path",
            str(manifest),
            "--test",
            "trace_gpu_analysis_tool_test",
        ],
        env=child_environment,
        check=False,
    )
    return completed.returncode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cargo", required=True)
    parser.add_argument("--target-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args(argv)
    return run_integration(
        cargo=args.cargo,
        target_dir=args.target_dir,
        manifest=args.manifest,
    )


if __name__ == "__main__":
    raise SystemExit(main())
