#!/usr/bin/env python3
"""Compile, link, and run Pulp's required Skia m153 capability contract.

This is deliberately a toolchain probe, not product policy. Generic installation
of Skia's process-global log handler and Graphite executor selection are routed
to Vellum; Pulp uses this check to prove the published provider can support that
future integration without accepting a header-only or missing-symbol archive.
"""

from __future__ import annotations

import argparse
import json
import os
import platform as host_platform
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

try:
    from tools.scripts import fetch_skia_for_release as skia_fetch
except ModuleNotFoundError:  # Direct script execution from outside the repo root.
    import fetch_skia_for_release as skia_fetch


REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "tools/deps/manifest.json"
REQUIRED_RELEASE = "chrome/m153"
NATIVE_PLATFORMS = {
    "darwin-arm64",
    "darwin-x64",
    "darwin-universal",
    "linux-arm64",
    "linux-x64",
}

PROBE_SOURCE = r"""
#include "include/core/SkExecutor.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/utils/SkLogHandler.h"

#include <cstdarg>
#include <memory>

class ProbeLogHandler final : public SkLogHandler {
public:
    void onLog(SkLogPriority, const char[], va_list) override {}
};

int main(int argc, char**) {
    auto executor = SkExecutor::MakeFIFOThreadPool(1, false);
    if (!executor) return 2;

    skgpu::graphite::ContextOptions options;
    options.fExecutor = executor.get();
    if (options.fExecutor != executor.get()) return 3;

    // Exercise both exported symbols without installing the process-global,
    // first-install-wins handler during the normal probe path.
    auto existing = SkLogHandler::GetInstance();
    if (argc == 153) {
        return SkLogHandler::SetInstance(sk_make_sp<ProbeLogHandler>()) ? 0 : 4;
    }
    return existing ? 0 : 0;
}
"""


def _manifest_skia() -> dict:
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    for dep in data.get("dependencies", []):
        if dep.get("name") == "Skia":
            return dep
    raise RuntimeError("tools/deps/manifest.json has no Skia dependency")


def _find_include_root(skia_dir: Path) -> Path:
    for candidate in (skia_dir / "build/include", skia_dir, skia_dir / "include"):
        if (candidate / "include/utils/SkLogHandler.h").is_file():
            return candidate
    raise RuntimeError(f"SkLogHandler.h not found under {skia_dir}")


def _find_skia_library(skia_dir: Path) -> Path:
    names = (
        "build/mac-gpu/lib/Release/libskia.a",
        "build/linux-gpu/lib/Release/libskia.a",
        "build/win-gpu/lib/Release/skia.lib",
    )
    for relative in names:
        candidate = skia_dir / relative
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"materialized release Skia library not found under {skia_dir}")


def _compiler() -> str:
    requested = os.environ.get("CXX")
    compiler = shutil.which(requested) if requested else None
    compiler = compiler or shutil.which("clang++") or shutil.which("c++")
    if not compiler:
        raise RuntimeError("no C++ compiler found (set CXX)")
    return compiler


def _require_native_host(platform: str) -> None:
    machine = host_platform.machine().lower()
    if sys.platform == "darwin":
        native = {"darwin-universal"}
        native.add("darwin-arm64" if machine in {"arm64", "aarch64"} else "darwin-x64")
    elif sys.platform.startswith("linux"):
        native = {"linux-arm64" if machine in {"arm64", "aarch64"} else "linux-x64"}
    else:
        native = set()
    if platform not in native:
        raise RuntimeError(
            f"host {sys.platform}/{machine} cannot compile and execute the "
            f"{platform} provider probe; run it natively on the matching desktop host"
        )


def verify(skia_dir: Path, platform: str) -> None:
    _require_native_host(platform)
    dependency = _manifest_skia()
    release = str(dependency.get("version", ""))
    if release != REQUIRED_RELEASE:
        raise RuntimeError(
            f"capability contract requires {REQUIRED_RELEASE}, manifest has {release or '<empty>'}"
        )

    try:
        manifest_key = skia_fetch.MATRIX_TO_MANIFEST[platform]
        asset = dependency["determinism"]["release_assets"][manifest_key]
        expected_sha = asset["sha256"]
    except KeyError as error:
        raise RuntimeError(f"manifest has no exact Skia asset for {platform}") from error
    if not skia_fetch.cache_generation_valid(skia_dir, platform, expected_sha):
        raise RuntimeError(
            f"{skia_dir} is not the verified {platform} release generation "
            f"for sha256 {expected_sha}"
        )

    include_root = _find_include_root(skia_dir)
    library = skia_fetch.expected_library_path(platform, str(skia_dir))
    compiler = _compiler()

    with tempfile.TemporaryDirectory(prefix="pulp-skia-m153-probe-") as temp:
        temp_dir = Path(temp)
        source = temp_dir / "probe.cpp"
        binary = temp_dir / "probe"
        source.write_text(PROBE_SOURCE, encoding="utf-8")

        command = [
            compiler,
            "-std=c++20",
            "-O0",
            f"-I{include_root}",
            str(source),
            str(library),
            "-pthread",
            "-o",
            str(binary),
        ]
        result = subprocess.run(command, text=True, capture_output=True)
        if result.returncode != 0:
            raise RuntimeError(
                "Skia m153 capability link failed:\n"
                + result.stdout
                + result.stderr
            )

        result = subprocess.run([str(binary)], text=True, capture_output=True)
        if result.returncode != 0:
            raise RuntimeError(
                f"Skia m153 capability probe exited {result.returncode}:\n"
                + result.stdout
                + result.stderr
            )

    print(
        "verify_skia_m153_capabilities: OK — SkLogHandler exported symbols and "
        "Graphite ContextOptions::fExecutor compile/link/run against "
        f"{library}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--skia-dir",
        type=Path,
        default=REPO_ROOT / "external/skia-build",
        help="materialized skia-builder archive root",
    )
    parser.add_argument(
        "--platform",
        required=True,
        choices=sorted(NATIVE_PLATFORMS),
        help="matching native desktop platform whose exact manifest asset is materialized",
    )
    args = parser.parse_args(argv)
    try:
        verify(args.skia_dir.resolve(), args.platform)
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"verify_skia_m153_capabilities: ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
