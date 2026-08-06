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
    required_companions = {
        document.get("$schema"),
        "agent-capability-surface.json",
        "agent-capability-surface.schema.json",
    }
    if None in required_companions:
        raise RuntimeError("installed manifest has no relative schema reference")
    for relative in sorted(required_companions):
        companion = prefix / "share/pulp" / relative
        if not companion.is_file():
            raise RuntimeError(f"installed capability companion missing: {companion}")
        json.loads(companion.read_text())
    for row in document.get("capabilities", []):
        for binding in row.get("bindings", []):
            header = prefix / "include" / binding["include"]
            if not header.is_file():
                raise RuntimeError(f"advertised installed header missing: {header}")
    return document


def consumer_source(document: dict) -> str:
    includes = sorted({
        binding["include"]
        for row in document["capabilities"]
        for binding in row["bindings"]
    })
    rows = {row["key"]: row for row in document["capabilities"]}
    lines = [*(f"#include <{header}>" for header in includes), "", "int main() {"]
    binding_index = 0
    for row in document["capabilities"]:
        lines.extend(["    {", f"        // {row['key']}"])
        for binding in row["bindings"]:
            name = binding["qualified_name"]
            if binding["kind"] == "cpp_type":
                lines.append(f"        static_assert(sizeof({name}) > 0);")
            else:
                lines.append(f"        auto *binding_{binding_index} = &{name};")
                lines.append(f"        (void)binding_{binding_index};")
            binding_index += 1
        lines.append("    }")
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


def configure_consumer(cmake: str, prefix: pathlib.Path, project: pathlib.Path,
                       document: dict | None = None) -> subprocess.CompletedProcess[str]:
    document = document or load_installed_manifest(prefix)
    targets = sorted({
        binding["target"]
        for row in document["capabilities"]
        for binding in row["bindings"]
    })
    project.mkdir(parents=True, exist_ok=True)
    (project / "main.cpp").write_text(consumer_source(document))
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(pulp_agent_capability_consumer LANGUAGES CXX)\n"
        "find_package(Pulp CONFIG REQUIRED NO_DEFAULT_PATH "
        "PATHS \"${PULP_SDK_PREFIX}/lib/cmake/Pulp\")\n"
        "add_executable(consumer main.cpp)\n"
        f"target_link_libraries(consumer PRIVATE {' '.join(targets)})\n"
        f"get_target_property(PULP_EXPORTED_INCLUDE_DIRS {targets[0]} "
        "INTERFACE_INCLUDE_DIRECTORIES)\n"
        "file(WRITE \"${CMAKE_BINARY_DIR}/pulp-include-dirs.txt\" "
        "\"${PULP_EXPORTED_INCLUDE_DIRS}\n\")\n"
    )
    return run([
        cmake, "-S", str(project), "-B", str(project / "build"),
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE",
        "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE",
        "-DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=TRUE",
        "-DCMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY=TRUE",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
        "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON",
        f"-DPULP_SDK_PREFIX={prefix}",
    ], cwd=project)


def build_consumer(cmake: str, project: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return run([cmake, "--build", str(project / "build"), "-j2", "--verbose"], cwd=project)


def inspect_isolation(prefix: pathlib.Path, project: pathlib.Path,
                      forbidden_roots: list[pathlib.Path]) -> None:
    include_file = project / "build/pulp-include-dirs.txt"
    include_dirs = [entry for entry in include_file.read_text().strip().split(";") if entry]
    if not include_dirs:
        raise AssertionError("Pulp::sequence exported no install include directory")
    prefix_root = prefix.resolve()
    for entry in include_dirs:
        path = pathlib.Path(entry).resolve()
        if not path.is_relative_to(prefix_root):
            raise AssertionError(f"exported include directory escapes install prefix: {entry}")

    commands_path = project / "build/compile_commands.json"
    commands_text = commands_path.read_text()
    for forbidden in forbidden_roots:
        spellings = {str(forbidden), str(forbidden.resolve())}
        if any(spelling in commands_text for spelling in spellings):
            raise AssertionError(f"consumer compile command leaks checkout path: {forbidden}")
    installed_include_spellings = {
        str(prefix / "include"), str((prefix / "include").resolve())}
    if not any(spelling in commands_text for spelling in installed_include_spellings):
        raise AssertionError("consumer compile command does not use the installed include directory")


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
    cache = (build_dir / "CMakeCache.txt").read_text()
    source_line = next((line for line in cache.splitlines()
                        if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")), None)
    if source_line is None:
        raise RuntimeError("build CMakeCache.txt does not identify its source checkout")
    source_root = pathlib.Path(source_line.split("=", 1)[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="pulp-installed-capability-") as temp:
        root = pathlib.Path(temp)
        prefix = root / "sdk"
        installed = run([args.cmake, "--install", str(build_dir), "--prefix", str(prefix)], cwd=root)
        if installed.returncode != 0:
            raise RuntimeError(f"SDK install failed:\n{installed.stdout}\n{installed.stderr}")
        document = load_installed_manifest(prefix)

        for index, companion_name in enumerate((
            "agent-capabilities.schema.json",
            "agent-capability-surface.json",
            "agent-capability-surface.schema.json",
        )):
            companion = prefix / "share/pulp" / companion_name
            held_companion = companion.with_suffix(companion.suffix + ".held")
            companion.rename(held_companion)
            try:
                expect_failure(
                    f"missing installed companion {companion_name}",
                    lambda index=index: configure_consumer(
                        args.cmake, prefix, root / f"missing-companion-{index}"
                    ),
                )
            finally:
                held_companion.rename(companion)

        manifest = prefix / "share/pulp/agent-capabilities.json"
        held_manifest = manifest.with_suffix(".held")
        manifest.rename(held_manifest)
        try:
            expect_failure(
                "missing installed manifest",
                lambda: configure_consumer(args.cmake, prefix, root / "missing-manifest"),
            )
        finally:
            held_manifest.rename(manifest)

        header = (
            prefix / "include" /
            document["capabilities"][0]["bindings"][0]["include"]
        )
        held_header = header.with_suffix(header.suffix + ".held")
        header.rename(held_header)
        try:
            missing_header_project = root / "missing-header"
            configured = configure_consumer(
                args.cmake, prefix, missing_header_project, document)
            if configured.returncode != 0:
                raise AssertionError(
                    "missing-header control did not reach compilation:\n"
                    f"{configured.stdout}\n{configured.stderr}")
            failed = build_consumer(args.cmake, missing_header_project)
            if failed.returncode == 0:
                raise AssertionError("negative control compiled without an advertised header")
        finally:
            held_header.rename(header)

        targets = prefix / "lib/cmake/Pulp/PulpTargets.cmake"
        held_targets = targets.with_suffix(".held")
        targets.rename(held_targets)
        try:
            failed = configure_consumer(args.cmake, prefix, root / "missing-export", document)
            if failed.returncode == 0:
                raise AssertionError("negative control unexpectedly configured without PulpTargets.cmake")
        finally:
            held_targets.rename(targets)

        consumer_project = root / "consumer"
        configured = configure_consumer(args.cmake, prefix, consumer_project, document)
        if configured.returncode != 0:
            raise RuntimeError(
                f"installed consumer configure failed:\n{configured.stdout}\n{configured.stderr}")
        inspect_isolation(prefix, consumer_project, [source_root, build_dir])
        built = build_consumer(args.cmake, consumer_project)
        if built.returncode != 0:
            raise RuntimeError(f"installed consumer build failed:\n{built.stdout}\n{built.stderr}")
        executable = root / "consumer/build/consumer"
        executed = run([str(executable)], cwd=root)
        if executed.returncode != 0:
            raise RuntimeError(f"installed consumer exited {executed.returncode}")

    print("agent-capabilities-installed-sdk: 11 install/schema/surface/isolation/include/export/consumer checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
