#!/usr/bin/env python3
"""Build and verify the C0 immutable compatibility baseline.

Exit 1 means a baseline mismatch. Exit 2 means the harness could not produce a
verdict (configure/build/tool failure), keeping compatibility drift distinct
from infrastructure failure in CI and packet receipts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DRIFT = 1
HARNESS_FAILURE = 2
SDK_RECEIPT = "installed-sdk-0.837.0.json"
SDK_RECEIPT_SHA256 = "a32cb189965e0150cb473cacdb0568f9bf686667dc7be49e293f0b73d0bb62ed"


def run(command: list[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_installed_sdk(sdk: Path, receipt_path: Path) -> dict[str, str]:
    receipt = json.loads(receipt_path.read_text())
    provenance_path = sdk / "sdk-provenance.json"
    if sha256(provenance_path) != receipt["sdk_provenance_sha256"]:
        raise RuntimeError("installed SDK provenance receipt hash mismatch")
    provenance = json.loads(provenance_path.read_text())
    for key, expected in receipt["provenance"].items():
        if provenance.get(key) != expected:
            raise RuntimeError(f"installed SDK provenance mismatch: {key}")
    for relative, expected in receipt["files"].items():
        path = sdk / relative
        if not path.is_file() or sha256(path) != expected:
            raise RuntimeError(f"installed SDK file hash mismatch: {relative}")
    cmake_root = sdk / "lib" / "cmake" / "Pulp"
    frozen_cmake = {
        relative for relative in receipt["files"]
        if relative.startswith("lib/cmake/Pulp/")
    }
    actual_cmake = {
        path.relative_to(sdk).as_posix()
        for path in cmake_root.rglob("*")
        if path.is_file()
    }
    unreceipted_cmake = sorted(actual_cmake - frozen_cmake)
    if unreceipted_cmake:
        raise RuntimeError(
            "installed SDK CMake surface has unreceipted files: "
            + ", ".join(unreceipted_cmake)
        )
    return receipt["files"]


def sdk_build_inputs(build: Path, sdk: Path) -> set[str]:
    inputs: set[str] = set()
    for depfile in build.rglob("*.o.d"):
        words = shlex.split(depfile.read_text().replace("\\\n", " "))
        for raw in words[1:]:
            path = Path(raw)
            try:
                inputs.add(path.resolve().relative_to(sdk).as_posix())
            except ValueError:
                pass
    for link_file in build.rglob("link.txt"):
        for raw in shlex.split(link_file.read_text()):
            path = Path(raw)
            try:
                inputs.add(path.resolve().relative_to(sdk).as_posix())
            except ValueError:
                pass
    return inputs


def verify_sdk_build_inputs(build: Path, sdk: Path, frozen: dict[str, str]) -> None:
    consumed = sdk_build_inputs(build, sdk)
    if not consumed:
        raise RuntimeError("installed SDK build produced no dependency receipt")
    missing = sorted(consumed - frozen.keys())
    if missing:
        raise RuntimeError("installed SDK build used unreceipted files: " + ", ".join(missing))


def processor_virtual_facts(repo: Path) -> str:
    import node_abi_gate

    header = repo / "core" / "format" / "include" / "pulp" / "format" / "processor.hpp"
    virtuals = node_abi_gate.virtual_order(header.read_text(), "Processor")
    return "".join(f"{index}:{item.signature}\n" for index, item in enumerate(virtuals))


def verify_processor_virtuals(repo: Path, fixtures: Path) -> tuple[int, str]:
    frozen = (fixtures / "expected" / "processor-virtuals.txt").read_text().splitlines()
    current = processor_virtual_facts(repo).splitlines()
    if current[:len(frozen)] != frozen:
        return DRIFT, "compatibility_drift=COMP-07.processor-virtual-order\n"
    return 0, "compatibility_ok=COMP-07.processor-virtual-order\n"


def locate(repo: Path, build: Path) -> tuple[Path, Path]:
    suffix = ".exe" if sys.platform == "win32" else ""
    runner = build / f"sample-region-compat-runner{suffix}"
    consumer = build / f"sample-region-old-sdk-consumer{suffix}"
    if not runner.is_file() or not consumer.is_file():
        raise RuntimeError(f"compatibility executables absent from {build}")
    return runner, consumer


def configure_and_build(repo: Path, build: Path) -> None:
    source = repo / "test" / "sample_region_compat"
    configured = run([
        "cmake", "-G", "Unix Makefiles", "-S", str(source), "-B", str(build),
        "-DCMAKE_BUILD_TYPE=Release", f"-DPULP_SOURCE_DIR={repo}",
        "-DPULP_BUILD_TESTS=OFF", "-DPULP_BUILD_EXAMPLES=OFF",
    ])
    if configured.returncode:
        raise RuntimeError("configure failed:\n" + configured.stdout + configured.stderr)
    built = run([
        str(repo / "tools" / "ci" / "governed-build.sh"), "cmake", "--build",
        str(build), "--target", "sample-region-compat-runner",
        "sample-region-old-sdk-consumer",
    ])
    if built.returncode:
        raise RuntimeError("build failed:\n" + built.stdout + built.stderr)


def build_installed_consumer(repo: Path, sdk: Path, build: Path,
                             receipt_path: Path) -> Path:
    frozen = verify_installed_sdk(sdk, receipt_path)
    pulp_dir = sdk / "lib" / "cmake" / "Pulp"
    if not (pulp_dir / "PulpConfig.cmake").is_file():
        raise RuntimeError(f"installed SDK has no PulpConfig.cmake: {pulp_dir}")
    configured = run([
        "cmake", "-G", "Unix Makefiles", "-S",
        str(repo / "test" / "sample_region_compat" / "installed_sdk"),
        "-B", str(build), "-DCMAKE_BUILD_TYPE=Release", f"-DPulp_DIR={pulp_dir}",
    ])
    if configured.returncode:
        raise RuntimeError("installed-SDK configure failed:\n" + configured.stdout + configured.stderr)
    built = run(["cmake", "--build", str(build), "--target",
                 "sample-region-old-sdk-consumer", "-j2"])
    if built.returncode:
        raise RuntimeError("installed-SDK build failed:\n" + built.stdout + built.stderr)
    verify_sdk_build_inputs(build, sdk, frozen)
    suffix = ".exe" if sys.platform == "win32" else ""
    return build / f"sample-region-old-sdk-consumer{suffix}"


def verify_once(runner: Path, consumer: Path, fixtures: Path,
                repo: Path | None = None) -> tuple[int, str]:
    if sha256(fixtures / SDK_RECEIPT) != SDK_RECEIPT_SHA256:
        return DRIFT, "compatibility_drift=PKT-C0-01.installed-sdk-receipt\n"
    checked = run([str(runner), "--verify", str(fixtures)])
    output = checked.stdout + checked.stderr
    if checked.returncode not in (0, DRIFT):
        return HARNESS_FAILURE, output
    old = run([str(consumer)])
    output += old.stdout + old.stderr
    if old.returncode:
        return HARNESS_FAILURE, output + "harness_failure=old-installed-sdk-consumer\n"
    sentinel = (fixtures / "expected" / "old-installed-sdk-consumer.txt").read_text()
    if old.stdout != sentinel:
        return DRIFT, output + "compatibility_drift=COMP-07.old-installed-sdk-consumer\n"
    if repo is not None:
        virtual_status, virtual_output = verify_processor_virtuals(repo, fixtures)
        output += virtual_output
        if virtual_status:
            return virtual_status, output
    return checked.returncode, output


def perturbations(fixtures: Path) -> list[tuple[str, Path, bytes, bytes]]:
    return [
        ("graph-v1", fixtures / "graphs" / "legacy-v1.pulpgraph", b"0.625", b"0.525"),
        ("graph-v2", fixtures / "graphs" / "legacy-v2.pulpgraph", b"0.25", b"0.35"),
        ("callback-feedback-graph", fixtures / "graphs" / "legacy-feedback-v1.pulpgraph", b"0.375", b"0.275"),
        ("no-region", fixtures / "expected" / "no-region-v2.pulpgraph", b"format_version", b"format_Version"),
        ("callback-feedback-output", fixtures / "expected" / "render-f32-bits.txt", b"[feedback_fixed]", b"[Feedback_fixed]"),
        ("signed-bake", fixtures / "bake" / "legacy-v1.pulpbake", b"PULPBAKE", b"QULPBAKE"),
        ("signed-bake-public-key", fixtures / "bake" / "legacy-v1-public-key.hex", b"79", b"69"),
        ("signed-bake-output", fixtures / "expected" / "bake-render-f32-bits.txt", b"3e300000", b"3e300001"),
        ("processor-abi", fixtures / "expected" / "abi.txt", b"processor_size=", b"processor_size=9"),
        ("processor-virtual-order", fixtures / "expected" / "processor-virtuals.txt", b"0:~Processor", b"0:~processor"),
        ("old-installed-sdk", fixtures / "expected" / "old-installed-sdk-consumer.txt", b"pass", b"fail"),
        ("installed-sdk-receipt", fixtures / SDK_RECEIPT,
         b"pulp.sample-region-c0-installed-sdk-receipt.v1",
         b"pulp.sample-region-c0-installed-sdk-receipt.V1"),
    ]


def prove_negative_controls(runner: Path, consumer: Path, fixtures: Path,
                            repo: Path) -> tuple[int, str]:
    lines: list[str] = []
    for name, path, needle, replacement in perturbations(fixtures):
        with tempfile.TemporaryDirectory(prefix=f"pulp-c0-{name}-") as raw:
            copy = Path(raw) / "fixtures"
            shutil.copytree(fixtures, copy)
            target = copy / path.relative_to(fixtures)
            data = target.read_bytes()
            pos = data.find(needle)
            if pos < 0:
                return HARNESS_FAILURE, f"harness_failure=negative-control-anchor.{name}\n"
            target.write_bytes(data[:pos] + replacement + data[pos + len(needle):])
            status, detail = verify_once(runner, consumer, copy, repo)
            if status != DRIFT:
                return HARNESS_FAILURE, (
                    f"harness_failure=negative-control-not-detected.{name}\n" + detail
                )
            lines.append(f"negative_control={name}:detected")
    return 0, "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--installed-sdk", type=Path,
                        help="also compile the frozen consumer against this installed SDK")
    args = parser.parse_args()
    repo = args.repo.resolve()
    fixtures = repo / "test" / "fixtures" / "sample-region-compat"
    owned_tmp = None
    try:
        if args.build_dir:
            build = args.build_dir.resolve()
        else:
            owned_tmp = tempfile.TemporaryDirectory(prefix="pulp-c0-build-")
            build = Path(owned_tmp.name)
        if not args.skip_build:
            configure_and_build(repo, build)
        runner, consumer = locate(repo, build)
        if args.installed_sdk:
            installed_build = build / "installed-sdk-consumer"
            consumer = build_installed_consumer(
                repo, args.installed_sdk.resolve(), installed_build,
                fixtures / SDK_RECEIPT)
        status, output = verify_once(runner, consumer, fixtures, repo)
        sys.stdout.write(output)
        if status:
            return status
        if args.negative_controls:
            status, output = prove_negative_controls(runner, consumer, fixtures, repo)
            sys.stdout.write(output)
            if status:
                return status
        print("sample_region_compat_baseline_verified=true")
        return 0
    except (KeyError, OSError, RuntimeError, ValueError) as exc:
        print(f"harness_failure={exc}", file=sys.stderr)
        return HARNESS_FAILURE
    finally:
        if owned_tmp is not None:
            owned_tmp.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
