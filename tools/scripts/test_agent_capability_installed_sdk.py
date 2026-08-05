#!/usr/bin/env python3
"""Exercise the agent capability contract from an installed SDK only."""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import tempfile


SCHEMA = "pulp.agent-capabilities.v1"


def run(command: list[str], *, cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True)


def load_installed_manifest(prefix: pathlib.Path) -> dict:
    path = prefix / "share/pulp/agent-capabilities.json"
    if not path.is_file():
        raise RuntimeError(f"installed manifest missing: {path}")
    document = json.loads(path.read_text())
    if document.get("schema") != SCHEMA:
        raise RuntimeError(f"installed manifest has wrong schema: {document.get('schema')!r}")
    for row in document.get("capabilities", []):
        header = prefix / "include" / row["include"]
        if not header.is_file():
            raise RuntimeError(f"advertised installed header missing: {header}")
    return document


def consumer_source(document: dict) -> str:
    includes = sorted({row["include"] for row in document["capabilities"]})
    rows = {row["key"]: row for row in document["capabilities"]}
    lines = [*(f"#include <{header}>" for header in includes), "", "int main() {"]
    for row in document["capabilities"]:
        lines.extend(["    {", f"        {row['symbol']} value{{}};", "        (void)value;", "    }"])
    # These non-inline calls force the installed exported targets to provide
    # audio, MIDI, and sequence link closure rather than proving headers only.
    if "audio.instrument-voice-allocator" in rows:
        lines.append("    pulp::audio::InstrumentVoiceAllocator allocator; (void)allocator.prepare(1);")
    if "midi.mpe-voice-tracker" in rows:
        lines.append("    pulp::midi::MpeVoiceTracker tracker; tracker.reset();")
    if "sequence.host-transport-projector" in rows:
        lines.append("    pulp::sequence::HostTransportProjector projector; projector.reset();")
    lines.extend(["    return 0;", "}", ""])
    return "\n".join(lines)


def configure_and_build(cmake: str, prefix: pathlib.Path, project: pathlib.Path,
                        document: dict) -> subprocess.CompletedProcess[str]:
    project.mkdir(parents=True, exist_ok=True)
    (project / "main.cpp").write_text(consumer_source(document))
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(pulp_agent_capability_consumer LANGUAGES CXX)\n"
        "find_package(Pulp CONFIG REQUIRED)\n"
        "add_executable(consumer main.cpp)\n"
        "target_link_libraries(consumer PRIVATE Pulp::sequence)\n"
    )
    configured = run([cmake, "-S", str(project), "-B", str(project / "build"),
                      "-DCMAKE_BUILD_TYPE=Release", f"-DPulp_DIR={prefix / 'lib/cmake/Pulp'}"],
                     cwd=project)
    if configured.returncode != 0:
        return configured
    return run([cmake, "--build", str(project / "build"), "-j2"], cwd=project)


def expect_failure(label: str, operation) -> None:
    try:
        operation()
    except (RuntimeError, json.JSONDecodeError):
        return
    raise AssertionError(f"negative control unexpectedly passed: {label}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()

    with tempfile.TemporaryDirectory(prefix="pulp-installed-capability-") as temp:
        root = pathlib.Path(temp)
        prefix = root / "sdk"
        installed = run([args.cmake, "--install", str(build_dir), "--prefix", str(prefix)], cwd=root)
        if installed.returncode != 0:
            raise RuntimeError(f"SDK install failed:\n{installed.stdout}\n{installed.stderr}")
        document = load_installed_manifest(prefix)

        manifest = prefix / "share/pulp/agent-capabilities.json"
        held_manifest = manifest.with_suffix(".held")
        manifest.rename(held_manifest)
        try:
            expect_failure("missing installed manifest", lambda: load_installed_manifest(prefix))
        finally:
            held_manifest.rename(manifest)

        header = prefix / "include" / document["capabilities"][0]["include"]
        held_header = header.with_suffix(header.suffix + ".held")
        header.rename(held_header)
        try:
            expect_failure("missing advertised installed header", lambda: load_installed_manifest(prefix))
        finally:
            held_header.rename(header)

        targets = prefix / "lib/cmake/Pulp/PulpTargets.cmake"
        held_targets = targets.with_suffix(".held")
        targets.rename(held_targets)
        try:
            failed = configure_and_build(args.cmake, prefix, root / "missing-export", document)
            if failed.returncode == 0:
                raise AssertionError("negative control unexpectedly linked without PulpTargets.cmake")
        finally:
            held_targets.rename(targets)

        consumer = configure_and_build(args.cmake, prefix, root / "consumer", document)
        if consumer.returncode != 0:
            raise RuntimeError(f"installed consumer failed:\n{consumer.stdout}\n{consumer.stderr}")
        executable = root / "consumer/build/consumer"
        executed = run([str(executable)], cwd=root)
        if executed.returncode != 0:
            raise RuntimeError(f"installed consumer exited {executed.returncode}")

    print("agent-capabilities-installed-sdk: 4 install/include/export/consumer checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
