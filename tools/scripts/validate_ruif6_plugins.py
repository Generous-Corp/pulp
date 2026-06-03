#!/usr/bin/env python3
"""Validate the opt-in RUIF-6 Rust native UI plugin path.

This intentionally captures through the macOS CLAP editor host and
PluginViewHost::capture_back_buffer_png(), not the CPU screenshot path.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], log_path: Path | None = None) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if log_path is not None:
        log_path.write_text(proc.stdout)
    if proc.returncode != 0:
        if proc.stdout:
            sys.stderr.write(proc.stdout)
        raise SystemExit(f"command failed ({proc.returncode}): {' '.join(cmd)}")
    return proc


def png_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path} is not a PNG")
    return struct.unpack(">II", data[16:24])


def require_gpu_log(log_path: Path, *, expect_rust_provider: bool = False) -> None:
    log = log_path.read_text()
    required = [
        "[plugin-gpu-host] adapter mode=custom use_gpu=true",
        "GpuSurface: backend_type=Metal",
        "SkiaSurface: Graphite initialized",
        "[plugin-gpu-host] first frame logical=1000x600 gpu=2000x1200",
    ]
    if expect_rust_provider:
        required.extend(
            [
                "CLAP: initialized 'PulpElysiumRuifRustProvider'",
                "RUIF Rust provider used: provider_id=test.rust.elysium provider_strict=true fallback=false",
            ]
        )
    missing = [needle for needle in required if needle not in log]
    if missing:
        raise SystemExit(f"{log_path} missing GPU capture evidence: {missing}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build-ruif6-plugin")
    parser.add_argument(
        "--artifact-dir",
        default="planning/artifacts/rust-ui/ruif-6/plugin-validation",
    )
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    artifact_dir = Path(args.artifact_dir)
    artifact_dir.mkdir(parents=True, exist_ok=True)

    capture_tool = build_dir / "examples/elysium-rust-ui-baseline/pulp-elysium-ruif-plugin-capture-mac"
    cpp_binary = build_dir / "CLAP/PulpElysiumRuifCppBaseline.clap/Contents/MacOS/PulpElysiumRuifCppBaseline"
    rust_binary = build_dir / "CLAP/PulpElysiumRuifRustProvider.clap/Contents/MacOS/PulpElysiumRuifRustProvider"

    for path in [capture_tool, cpp_binary, rust_binary]:
        if not path.exists():
            raise SystemExit(f"missing required build artifact: {path}")

    cache = build_dir / "CMakeCache.txt"
    cache_text = cache.read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache_text:
        raise SystemExit("RUIF-6 validation requires CMAKE_BUILD_TYPE=Release")
    if "PULP_BUILD_NATIVE_UI_RUST_TESTS:BOOL=ON" not in cache_text:
        raise SystemExit("RUIF-6 validation requires PULP_BUILD_NATIVE_UI_RUST_TESTS=ON")

    captures = [
        ("cpp-clap-editor", cpp_binary, "cpp-clap-editor.png"),
        ("rust-clap-editor-open1", rust_binary, "rust-clap-editor-open1.png"),
        ("rust-clap-editor-open2", rust_binary, "rust-clap-editor-open2.png"),
    ]

    for label, binary, png_name in captures:
        log_path = artifact_dir / f"{label}.log"
        png_path = artifact_dir / png_name
        run(
            [
                str(capture_tool),
                "--bundle-binary",
                str(binary),
                "--output",
                str(png_path),
                "--width",
                "1000",
                "--height",
                "600",
            ],
            log_path,
        )
        require_gpu_log(log_path, expect_rust_provider=label.startswith("rust-"))
        if png_size(png_path) != (2000, 1200):
            raise SystemExit(f"{png_path} did not capture expected 2000x1200 GPU PNG")

    cpp_png = (artifact_dir / "cpp-clap-editor.png").read_bytes()
    rust_open1 = (artifact_dir / "rust-clap-editor-open1.png").read_bytes()
    rust_open2 = (artifact_dir / "rust-clap-editor-open2.png").read_bytes()
    if cpp_png != rust_open1:
        raise SystemExit("Rust CLAP editor capture differs from C++ CLAP reference")
    if rust_open1 != rust_open2:
        raise SystemExit("Rust CLAP editor reopen capture is not deterministic")

    dlopen_script = (
        "import ctypes, sys\n"
        "for path in sys.argv[1:]:\n"
        "    ctypes.CDLL(path)\n"
        "    print('OK', path)\n"
    )
    dlopen = run(
        ["python3", "-c", dlopen_script, str(cpp_binary), str(rust_binary)],
        artifact_dir / "clap-dlopen.log",
    )

    pluginval_path = shutil.which("pluginval")
    pluginval_status = "not-installed"
    if pluginval_path is not None and (build_dir / "VST3/PulpElysiumRuifRustProvider.vst3").exists():
        pluginval_status = "available-not-run-by-script"

    report = {
        "status": "pass",
        "build_dir": str(build_dir),
        "artifact_dir": str(artifact_dir),
        "validated_formats": ["CLAP"],
        "not_validated": {
            "VST3": "SDK not found or target not built",
            "AUv2": "AudioUnitSDK not found or target not built",
            "AUv3": "not in RUIF-6 local scope",
            "pluginval": pluginval_status,
        },
        "gpu_capture": "macOS CLAP editor host via capture_back_buffer_png",
        "reference_comparison": "rust CLAP capture byte-identical to C++ CLAP capture",
        "reopen_check": "rust CLAP open1/open2 byte-identical",
        "png_size": [2000, 1200],
        "dlopen_returncode": dlopen.returncode,
        "vst3_local_status": "not-built" if not (build_dir / "VST3").exists() else "present",
    }
    (artifact_dir / "ruif6-plugin-validation-report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
