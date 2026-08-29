#!/usr/bin/env python3
"""Validate a pinned Skia/Dawn update through its reusable release path."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.scripts import fetch_skia_for_release as skia_fetch  # noqa: E402


WARM_MARKERS = (
    "OK: immutable Skia cache generation ready at ",
    "OK: complete Skia/Dawn generation already unpacked from the pinned asset ",
)


def default_cache_root() -> Path:
    configured = os.environ.get("PULP_SKIA_CACHE_ROOT")
    return Path(configured).expanduser() if configured else Path.home() / ".cache/pulp/skia"


def native_platform() -> str:
    machine = platform.machine().lower()
    if sys.platform == "darwin":
        return "darwin-arm64" if machine in {"arm64", "aarch64"} else "darwin-x64"
    if sys.platform.startswith("linux"):
        return "linux-arm64" if machine in {"arm64", "aarch64"} else "linux-x64"
    raise RuntimeError(f"render-update validation has no native provider for {sys.platform}/{machine}")


def run_checked(command: list[str]) -> str:
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            + result.stdout + result.stderr
        )
    return result.stdout


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def classify_fetch(output: str) -> str:
    if "OK: atomically published immutable Skia cache at " in output:
        return "cold-published"
    if "OK: concurrent publisher completed immutable cache at " in output:
        return "concurrent-published"
    if any(marker in output for marker in WARM_MARKERS):
        return "warm-verified"
    raise RuntimeError("Skia fetch returned success without a recognized publication receipt")


def source_sha() -> str:
    dirty = run_checked([
        "git", "status", "--porcelain=v1", "--untracked-files=all",
    ])
    if dirty.strip():
        raise RuntimeError(
            "render-update evidence requires a clean exact source checkout; "
            "commit or remove every tracked and untracked change first"
        )
    value = run_checked(["git", "rev-parse", "HEAD"]).strip()
    if len(value) != 40 or any(character not in "0123456789abcdef" for character in value):
        raise RuntimeError(f"could not bind validation to an exact source SHA: {value!r}")
    return value


def expected_v8_version() -> str:
    manifest = json.loads((ROOT / "tools/deps/manifest.json").read_text(encoding="utf-8"))
    entry = next(item for item in manifest["dependencies"] if item["name"] == "V8")
    return str(entry["version"]).split("-", 3)[2]


def validate(cache_root: Path, build_dir: Path, cache_only: bool) -> dict[str, object]:
    matrix_platform = native_platform()
    _, asset = skia_fetch.manifest_asset(matrix_platform)
    if asset is None:
        raise RuntimeError(f"manifest has no Skia provider for {matrix_platform}")
    asset_sha = str(asset["sha256"])
    generation = skia_fetch.keyed_cache_dest(str(cache_root), matrix_platform, asset_sha)
    fetch = [
        sys.executable,
        "tools/scripts/fetch_skia_for_release.py",
        matrix_platform,
        "--cache-root",
        str(cache_root),
    ]
    first_fetch = classify_fetch(run_checked(fetch))
    second_output = run_checked(fetch)
    if not any(marker in second_output for marker in WARM_MARKERS):
        raise RuntimeError("second Skia fetch did not prove a verified no-download warm hit")

    run_checked([
        sys.executable,
        "tools/scripts/verify_skia_m153_capabilities.py",
        "--platform",
        matrix_platform,
        "--skia-dir",
        str(generation),
    ])

    receipt_path = generation / skia_fetch.GENERATION_RECEIPT
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    receipt_files = receipt.get("files")
    if not isinstance(receipt_files, list):
        raise RuntimeError("generation receipt has no exact extracted-file list")

    mixed_provider: dict[str, object]
    if cache_only:
        mixed_provider = {"status": "not-run-cache-only"}
    elif matrix_platform != "darwin-arm64":
        mixed_provider = {
            "status": "not-applicable",
            "reason": "required mixed-provider execution is owned by the darwin-arm64 release path",
        }
    else:
        run_checked([sys.executable, "tools/scripts/fetch_v8_for_release.py", matrix_platform])
        run_checked([
            "cmake", "-S", ".", "-B", str(build_dir),
            "-DPULP_BUILD_TESTS=OFF",
            "-DPULP_BUILD_EXAMPLES=ON",
            "-DPULP_ENABLE_DESIGN_IMPORT=OFF",
            "-DPULP_BUILD_RUST_CLI=OFF",
            "-DPULP_JS_ENGINE=v8",
            "-DPULP_VALIDATE_V8_PROVIDER_STRICT=ON",
            f"-DSKIA_DIR={generation}",
        ])
        run_checked([
            "tools/ci/governed-build.sh", "cmake", "--build", str(build_dir),
            "--config", "Release", "--target", "pulp-threejs-native-demo",
        ])
        capture = build_dir / "pulp-threejs-provider-identity.png"
        run_checked([
            "cmake",
            f"-DDEMO_BIN={build_dir / 'examples/threejs-native-demo/pulp-threejs-native-demo'}",
            f"-DCAPTURE_PATH={capture}",
            "-DEXPECTED_KIND=v8builder",
            f"-DEXPECTED_VERSION={expected_v8_version()}",
            "-P", "examples/threejs-native-demo/provider_identity_test.cmake",
        ])
        mixed_provider = {
            "status": "pass",
            "capture": str(capture.resolve()),
            "capture_bytes": capture.stat().st_size,
            "capture_sha256": file_sha256(capture),
            "v8_version": expected_v8_version(),
        }

    return {
        "schema_version": 1,
        "source_sha": source_sha(),
        "platform": matrix_platform,
        "asset_sha256": asset_sha,
        "generation": str(generation),
        "generation_receipt": str(receipt_path),
        "generation_receipt_sha256": file_sha256(receipt_path),
        "generation_file_count": len(receipt_files),
        "generation_payload_bytes": sum(int(item["size"]) for item in receipt_files),
        "first_fetch": first_fetch,
        "second_fetch_no_download": True,
        "capability_probe": "pass",
        "mixed_provider": mixed_provider,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache-root", type=Path, default=default_cache_root())
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-render-update-validation")
    parser.add_argument("--cache-only", action="store_true")
    parser.add_argument("--result", type=Path)
    args = parser.parse_args(argv)
    try:
        result = validate(args.cache_root.resolve(), args.build_dir.resolve(), args.cache_only)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"validate_render_update: ERROR: {error}", file=sys.stderr)
        return 1
    payload = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.result:
        args.result.parent.mkdir(parents=True, exist_ok=True)
        args.result.write_text(payload, encoding="utf-8")
    print(payload, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
