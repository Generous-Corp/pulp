#!/usr/bin/env python3
"""Compile, link, and run Pulp's required Skia m153 capability contract.

This is deliberately a toolchain probe, not product policy. Generic installation
of Skia's process-global log handler and Graphite executor selection are routed
to Vellum; Pulp uses this check to prove the published provider can support that
future integration without accepting a header-only or missing-symbol archive.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform as host_platform
import re
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
MINIMUM_MILESTONE = 153
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


def _compiler() -> str:
    requested = os.environ.get("CXX")
    compiler = shutil.which(requested) if requested else None
    compiler = compiler or shutil.which("clang++") or shutil.which("c++")
    if not compiler:
        raise RuntimeError("no C++ compiler found (set CXX)")
    return compiler


def _require_native_host(platform: str) -> None:
    machine = host_platform.machine().lower()
    if platform == "darwin-universal":
        if sys.platform != "darwin" or machine not in {"arm64", "aarch64"}:
            raise RuntimeError(
                "darwin-universal capability acceptance requires a darwin-arm64 "
                "host that can run arm64 natively and x86_64 explicitly through Rosetta"
            )
        return
    if sys.platform == "darwin":
        native = {"darwin-arm64" if machine in {"arm64", "aarch64"} else "darwin-x64"}
    elif sys.platform.startswith("linux"):
        native = {"linux-arm64" if machine in {"arm64", "aarch64"} else "linux-x64"}
    else:
        native = set()
    if platform not in native:
        raise RuntimeError(
            f"host {sys.platform}/{machine} cannot compile and execute the "
            f"{platform} provider probe; run it natively on the matching desktop host"
        )


def _sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _source_sha() -> str:
    dirty = subprocess.run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
    )
    if dirty.returncode != 0 or dirty.stdout.strip():
        raise RuntimeError(
            "universal capability proof requires a clean exact source checkout"
        )
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
    )
    value = result.stdout.strip()
    if result.returncode != 0 or not re.fullmatch(r"[0-9a-f]{40}", value):
        raise RuntimeError("could not bind universal capability proof to exact source HEAD")
    return value


def _compile_and_run(
    include_root: Path,
    library: Path,
    compiler: str,
    temp_dir: Path,
    architecture: str | None,
) -> dict[str, str]:
    suffix = architecture or "native"
    source = temp_dir / f"probe-{suffix}.cpp"
    binary = temp_dir / f"probe-{suffix}"
    source.write_text(PROBE_SOURCE, encoding="utf-8")

    command = [compiler]
    if architecture is not None:
        command.extend(["-arch", architecture])
    command.extend([
        "-std=c++20",
        "-O0",
        f"-I{include_root}",
        str(source),
        str(library),
        "-pthread",
        "-o",
        str(binary),
    ])
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode != 0:
        label = architecture or "native"
        raise RuntimeError(
            f"Skia m153 {label} capability link failed:\n"
            + result.stdout
            + result.stderr
        )

    run_command = [str(binary)]
    run_mode = "native"
    if architecture == "x86_64":
        run_command = ["/usr/bin/arch", "-x86_64", str(binary)]
        run_mode = "rosetta-x86_64"
    result = subprocess.run(run_command, text=True, capture_output=True)
    if result.returncode != 0:
        label = architecture or "native"
        raise RuntimeError(
            f"Skia m153 {label} capability probe exited {result.returncode}:\n"
            + result.stdout
            + result.stderr
        )
    return {
        "architecture": architecture or host_platform.machine().lower(),
        "compile": "pass",
        "link": "pass",
        "run": "pass",
        "run_mode": run_mode,
    }


def verify(skia_dir: Path, platform: str) -> dict[str, object]:
    _require_native_host(platform)
    initial_source_sha = _source_sha() if platform == "darwin-universal" else None
    dependency = _manifest_skia()
    release = str(dependency.get("version", ""))
    match = re.fullmatch(r"chrome/m([0-9]+)", release)
    milestone = int(match.group(1)) if match else 0
    if milestone < MINIMUM_MILESTONE:
        raise RuntimeError(
            f"capability contract requires chrome/m{MINIMUM_MILESTONE}+; "
            f"manifest has {release or '<empty>'}"
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
    receipt_path = skia_dir / skia_fetch.GENERATION_RECEIPT
    initial_receipt_sha = _sha256_file(receipt_path)

    with tempfile.TemporaryDirectory(prefix="pulp-skia-m153-probe-") as temp:
        temp_dir = Path(temp)
        architectures = ["arm64", "x86_64"] if platform == "darwin-universal" else [None]
        probes = [
            _compile_and_run(include_root, library, compiler, temp_dir, architecture)
            for architecture in architectures
        ]

    if platform == "darwin-universal":
        if not skia_fetch.cache_generation_valid(skia_dir, platform, expected_sha):
            raise RuntimeError("universal Skia generation changed during capability proof")
        if _sha256_file(receipt_path) != initial_receipt_sha:
            raise RuntimeError("universal Skia generation receipt changed during capability proof")
        if _source_sha() != initial_source_sha:
            raise RuntimeError("Pulp source changed during universal capability proof")
    result: dict[str, object] = {
        "schema_version": 1,
        "status": "pass",
        "platform": platform,
        "asset_sha256": expected_sha,
        "generation": str(skia_dir),
        "generation_receipt_sha256": initial_receipt_sha,
        "probe_source_sha256": _sha256_bytes(PROBE_SOURCE.encode("utf-8")),
        "probes": probes,
    }
    if platform == "darwin-universal":
        result["source_sha"] = initial_source_sha

    print(
        f"verify_skia_m153_capabilities: OK — {release} SkLogHandler exported symbols and "
        "Graphite ContextOptions::fExecutor compile/link/run against "
        f"{library}"
    )
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--skia-dir",
        type=Path,
        default=REPO_ROOT / "external/skia-build",
        help="materialized skia-builder archive root",
    )
    parser.add_argument(
        "--result",
        type=Path,
        help="write the source/asset/generation-bound capability result as JSON",
    )
    parser.add_argument(
        "--platform",
        required=True,
        choices=sorted(NATIVE_PLATFORMS),
        help="matching native desktop platform whose exact manifest asset is materialized",
    )
    args = parser.parse_args(argv)
    try:
        result = verify(args.skia_dir.resolve(), args.platform)
        if args.result:
            args.result.parent.mkdir(parents=True, exist_ok=True)
            args.result.write_text(
                json.dumps(result, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"verify_skia_m153_capabilities: ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
