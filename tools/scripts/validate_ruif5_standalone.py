#!/usr/bin/env python3
"""Validate RUIF-5 C++ vs Rust standalone GPU screenshot parity."""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path


GPU_PATTERNS = (
    "GpuSurface: created Metal surface",
    "GpuSurface: Dawn initialized",
    "GpuSurface: backend_type=Metal",
    "SkiaSurface: Graphite initialized",
    "[gpu-host] first frame: logical=1000x600 gpu=2000x1200 scale=2.0",
)


def run(command: list[str], log_path: Path | None = None) -> None:
    if log_path is None:
        subprocess.run(command, check=True)
        return
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("wb") as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def png_dimensions(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    require(data.startswith(b"\x89PNG\r\n\x1a\n"), f"{path}: not a PNG")
    require(len(data) >= 33 and data[12:16] == b"IHDR", f"{path}: missing IHDR")
    return struct.unpack(">II", data[16:24])


def require_gpu_host(binary: Path) -> None:
    result = subprocess.run(
        ["nm", str(binary)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    require("MacGpuWindowHost" in result.stdout, f"{binary}: MacGpuWindowHost not linked")


def require_gpu_log(log_path: Path) -> None:
    text = log_path.read_text(errors="replace")
    missing = [pattern for pattern in GPU_PATTERNS if pattern not in text]
    require(not missing, f"{log_path}: missing GPU log patterns: {missing}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--build-dir",
        default="build-ruif5-standalone",
        help="Configured RUIF-5 build directory.",
    )
    parser.add_argument(
        "--artifact-dir",
        default="planning/artifacts/rust-ui/ruif-5/standalone-validation",
        help="Output directory for screenshots, layouts, and logs.",
    )
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    artifact_dir = Path(args.artifact_dir).resolve()
    app_dir = build_dir / "examples" / "elysium-rust-ui-baseline"
    cpp_bin = (
        app_dir
        / "PulpElysiumRuifCppBaseline.app"
        / "Contents"
        / "MacOS"
        / "PulpElysiumRuifCppBaseline"
    )
    rust_bin = (
        app_dir
        / "PulpElysiumRuifRustProvider.app"
        / "Contents"
        / "MacOS"
        / "PulpElysiumRuifRustProvider"
    )

    require(cpp_bin.exists(), f"missing C++ baseline binary: {cpp_bin}")
    require(rust_bin.exists(), f"missing Rust provider binary: {rust_bin}")
    require_gpu_host(cpp_bin)
    require_gpu_host(rust_bin)

    screenshots = artifact_dir / "screenshots"
    layouts = artifact_dir / "layouts"
    logs = artifact_dir / "logs"
    cpp_png = screenshots / "elysium-cpp-standalone.png"
    rust_png = screenshots / "elysium-rust-standalone.png"
    cpp_layout = layouts / "elysium-cpp-standalone.layout.json"
    rust_layout = layouts / "elysium-rust-standalone.layout.json"
    cpp_log = logs / "cpp-standalone.log"
    rust_log = logs / "rust-standalone.log"

    for path in (cpp_png, rust_png, cpp_layout, rust_layout, cpp_log, rust_log):
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists():
            path.unlink()

    run(
        [
            str(cpp_bin),
            f"--screenshot={cpp_png}",
            f"--layout={cpp_layout}",
        ],
        cpp_log,
    )
    run(
        [
            str(rust_bin),
            f"--screenshot={rust_png}",
            f"--layout={rust_layout}",
        ],
        rust_log,
    )

    require_gpu_log(cpp_log)
    require_gpu_log(rust_log)
    require(cpp_layout.stat().st_size > 0, f"{cpp_layout}: empty layout")
    require(rust_layout.stat().st_size > 0, f"{rust_layout}: empty layout")
    require(png_dimensions(cpp_png) == (2000, 1200), f"{cpp_png}: wrong dimensions")
    require(png_dimensions(rust_png) == (2000, 1200), f"{rust_png}: wrong dimensions")
    require(cpp_png.read_bytes() == rust_png.read_bytes(), "C++ and Rust screenshots differ")

    print("RUIF-5 standalone validation passed")
    print(f"artifacts: {artifact_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
