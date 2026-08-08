#!/usr/bin/env python3
"""Exercise each agent capability from an isolated installed SDK."""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import pathlib
import re
import shutil
import subprocess
import tempfile

import json_schema_lite


SCHEMA = "pulp.agent-capabilities.v1"
SUPPORTED_REQUIRED_FEATURES = {
    "capability-contract-version-v1",
    "coverage-state-v1",
    "design-runtime-separation-v1",
    "determinism-contract-v1",
    "tombstones-v1",
    "typed-bindings-v1",
}
MAINTENANCE_ONLY = {
    "agent-capability-surface.json",
    "agent-capability-surface.schema.json",
    "legacy-unreviewed-baseline.json",
    "contract-history.json",
}


def run(command: list[str], *, cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True)


def cache_value(cache: str, name: str) -> str | None:
    prefix = name + ":"
    for line in cache.splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1]
    return None


def validate_target_ownership(document: dict, owners: dict[str, str]) -> None:
    for row in document.get("capabilities", []):
        for binding in row.get("bindings", []):
            owner = owners.get(binding["include"])
            if owner is None:
                raise RuntimeError(
                    f"{row['key']} binding {binding['include']} has no reviewed minimal owner"
                )
            if binding["target"] != owner:
                raise RuntimeError(
                    f"{row['key']} binding {binding['include']} declares "
                    f"{binding['target']}, but its minimal owning target is {owner}"
                )


def validate_required_features(
    document: dict, *, required_by_consumer: set[str]
) -> None:
    raw_features = document.get("required_features")
    if not isinstance(raw_features, list) or any(
        not isinstance(feature, str) for feature in raw_features
    ):
        raise RuntimeError(
            "installed manifest required_features must be an array of strings"
        )
    advertised = set(raw_features)
    unknown = sorted(advertised - SUPPORTED_REQUIRED_FEATURES)
    if unknown:
        raise RuntimeError(
            "installed manifest requires unsupported features: " + ", ".join(unknown)
        )
    missing = sorted(required_by_consumer - advertised)
    if missing:
        raise RuntimeError(
            "installed manifest does not provide consumer-required features: "
            + ", ".join(missing)
        )
    if "determinism-contract-v1" in advertised:
        missing_rows = sorted(
            row.get("key", "<unknown>")
            for row in document.get("capabilities", [])
            if "determinism" not in row
        )
        if missing_rows:
            raise RuntimeError(
                "determinism-contract-v1 requires determinism on every live row: "
                + ", ".join(missing_rows)
            )


def load_installed_manifest(prefix: pathlib.Path, owners: dict[str, str]) -> dict:
    shared = prefix / "share/pulp"
    path = shared / "agent-capabilities.json"
    if not path.is_file():
        raise RuntimeError(f"installed manifest missing: {path}")
    document = json.loads(path.read_text())
    if document.get("schema") != SCHEMA:
        raise RuntimeError(f"installed manifest has wrong schema: {document.get('schema')!r}")
    schema_name = document.get("$schema")
    if not isinstance(schema_name, str) or not (shared / schema_name).is_file():
        raise RuntimeError("installed manifest has no installed relative schema")
    schema_document = json.loads((shared / schema_name).read_text())
    schema_problems = json_schema_lite.validate(document, schema_document)
    if schema_problems:
        raise RuntimeError(
            "installed manifest does not satisfy its installed schema: "
            + "; ".join(schema_problems)
        )
    validate_required_features(
        document, required_by_consumer={"determinism-contract-v1"}
    )
    present_maintenance = sorted(
        name for name in MAINTENANCE_ONLY if (shared / name).exists()
    )
    if present_maintenance:
        raise RuntimeError(
            "maintenance-only capability artifacts were installed: "
            + ", ".join(present_maintenance)
        )
    for row in document.get("capabilities", []):
        for binding in row.get("bindings", []):
            header = prefix / "include" / binding["include"]
            if not header.is_file():
                raise RuntimeError(f"advertised installed header missing: {header}")
    validate_target_ownership(document, owners)
    return document


def binding_identity(row: dict, binding: dict) -> tuple[str, str, str]:
    return row["key"], binding["role"], binding["qualified_name"]


def binding_source(
    row: dict,
    binding: dict,
    addresses: dict[tuple[str, str, str], str],
) -> str:
    name = binding["qualified_name"]
    reference = (
        f"static_assert(sizeof({name}) > 0);"
        if binding["kind"] == "cpp_type"
        else (
            "auto *volatile binding = "
            f"{addresses[binding_identity(row, binding)]}; (void)binding;"
        )
    )
    return (
        f"#include <{binding['include']}>\n\n"
        f"int main() {{\n    // {row['key']} / {binding['role']}\n"
        f"    {reference}\n    return 0;\n}}\n"
    )


def load_source_contract(
    source_root: pathlib.Path,
) -> tuple[
    dict[str, str],
    dict[str, str],
    dict[tuple[str, str, str], str],
]:
    path = source_root / "tools/scripts/agent_capability_manifest.py"
    spec = importlib.util.spec_from_file_location("pulp_agent_capability_source", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load capability probe source: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    probe_problems = [
        problem
        for row in module.EXPORTS
        for problem in module._link_probe_problems(row)
    ]
    if probe_problems:
        raise RuntimeError("; ".join(probe_problems))
    probes = {
        row["key"]: module.render_link_probe(row)
        for row in module.EXPORTS
    }
    addresses = {
        binding_identity(row, binding): binding.get(
            "_address_expression", f"&{binding['qualified_name']}"
        )
        for row in module.EXPORTS
        for binding in row["bindings"]
    }
    return probes, dict(module.REVIEWED_MINIMAL_TARGETS), addresses


def capability_source(
    row: dict,
    probe: str,
    addresses: dict[tuple[str, str, str], str],
) -> str:
    includes = sorted({binding["include"] for binding in row["bindings"]})
    lines = [*(f"#include <{include}>" for include in includes), "", "int main() {"]
    for index, binding in enumerate(row["bindings"]):
        name = binding["qualified_name"]
        if binding["kind"] == "cpp_type":
            lines.append(f"    static_assert(sizeof({name}) > 0);")
        else:
            address = addresses[binding_identity(row, binding)]
            lines.append(
                f"    auto *binding_{index} = {address}; (void)binding_{index};"
            )
    lines.append(f"    {probe}")
    lines.extend(["    return 0;", "}", ""])
    return "\n".join(lines)


def configure_consumer(
    cmake: str,
    prefix: pathlib.Path,
    project: pathlib.Path,
    *,
    source: str,
    target: str,
    generator: str | None,
    generator_options: list[str],
    configuration: str | None,
    use_pulp_dir: bool = True,
) -> subprocess.CompletedProcess[str]:
    project.mkdir(parents=True, exist_ok=True)
    (project / "main.cpp").write_text(source)
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(pulp_agent_capability_consumer LANGUAGES CXX)\n"
        "find_package(Pulp CONFIG REQUIRED)\n"
        f"if(NOT TARGET {target})\n"
        f"  message(FATAL_ERROR \"Declared capability target is absent: {target}\")\n"
        "endif()\n"
        "add_executable(consumer main.cpp)\n"
        f"target_link_libraries(consumer PRIVATE {target})\n"
        "file(GENERATE OUTPUT \"${CMAKE_BINARY_DIR}/consumer-path-$<CONFIG>.txt\" "
        "CONTENT \"$<TARGET_FILE:consumer>\")\n"
    )
    query = project / "build/.cmake/api/v1/query/codemodel-v2"
    query.parent.mkdir(parents=True, exist_ok=True)
    query.write_text("")
    command = [cmake]
    if generator:
        command.extend(["-G", generator])
    command.extend(generator_options)
    command.extend([
        "-S", str(project), "-B", str(project / "build"),
        "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE",
        "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE",
        "-DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=TRUE",
        "-DCMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY=TRUE",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
        "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON",
        f"-DPULP_SDK_PREFIX={prefix}",
    ])
    if use_pulp_dir:
        config_files = sorted(prefix.rglob("PulpConfig.cmake"))
        if len(config_files) != 1:
            raise RuntimeError(
                f"expected one installed PulpConfig.cmake, got {config_files}"
            )
        command.append(f"-DPulp_DIR={config_files[0].parent}")
    else:
        command.append(f"-DCMAKE_PREFIX_PATH={prefix}")
    if configuration:
        command.append(f"-DCMAKE_BUILD_TYPE={configuration}")
    return run(command, cwd=project)


def build_consumer(
    cmake: str, project: pathlib.Path, configuration: str | None
) -> subprocess.CompletedProcess[str]:
    command = [cmake, "--build", str(project / "build")]
    if configuration:
        command.extend(["--config", configuration])
    command.extend(["-j2", "--verbose"])
    return run(command, cwd=project)


def executable_path(
    project: pathlib.Path, configuration: str | None
) -> pathlib.Path:
    candidates = sorted((project / "build").glob("consumer-path-*.txt"))
    if not candidates:
        raise RuntimeError("consumer target path was not generated")
    configured = (
        [
            path
            for path in candidates
            if path.name == f"consumer-path-{configuration}.txt"
        ]
        if configuration
        else []
    )
    selected = configured[0] if configured else candidates[0]
    executable = pathlib.Path(selected.read_text())
    if not executable.is_file():
        raise RuntimeError(f"generated consumer executable is missing: {executable}")
    return executable


def inspect_isolation(
    prefix: pathlib.Path,
    project: pathlib.Path,
    configuration_name: str | None,
    forbidden_roots: list[pathlib.Path],
) -> None:
    reply = project / "build/.cmake/api/v1/reply"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        raise AssertionError("CMake File API produced no reply index")
    index = json.loads(indexes[-1].read_text())
    codemodel_ref = index.get("reply", {}).get("codemodel-v2")
    if not isinstance(codemodel_ref, dict) or not isinstance(
        codemodel_ref.get("jsonFile"), str
    ):
        raise AssertionError("CMake File API produced no codemodel-v2 reply")
    codemodel = json.loads((reply / codemodel_ref["jsonFile"]).read_text())
    configurations = codemodel.get("configurations", [])
    configuration = next(
        (
            item
            for item in configurations
            if configuration_name is not None
            and item.get("name") == configuration_name
        ),
        configurations[0] if configurations else None,
    )
    if not isinstance(configuration, dict):
        raise AssertionError("CMake File API codemodel has no configuration")
    target_ref = next(
        (item for item in configuration.get("targets", []) if item.get("name") == "consumer"),
        None,
    )
    if not isinstance(target_ref, dict) or not isinstance(target_ref.get("jsonFile"), str):
        raise AssertionError("CMake File API codemodel has no consumer target")
    target = json.loads((reply / target_ref["jsonFile"]).read_text())
    include_dirs = [
        include["path"]
        for group in target.get("compileGroups", [])
        for include in group.get("includes", [])
        if isinstance(include, dict) and isinstance(include.get("path"), str)
    ]
    if not include_dirs:
        raise AssertionError("consumer has no evaluated transitive include directories")
    compile_fragments = [
        fragment["fragment"]
        for group in target.get("compileGroups", [])
        for fragment in group.get("compileCommandFragments", [])
        if isinstance(fragment, dict) and isinstance(fragment.get("fragment"), str)
    ]
    link_fragments = [
        fragment["fragment"]
        for fragment in target.get("link", {}).get("commandFragments", [])
        if isinstance(fragment, dict) and isinstance(fragment.get("fragment"), str)
    ]
    artifact_paths = [
        artifact["path"]
        for artifact in target.get("artifacts", [])
        if isinstance(artifact, dict) and isinstance(artifact.get("path"), str)
    ]
    prefix_root = prefix.resolve()
    for entry in include_dirs:
        path = pathlib.Path(entry).resolve()
        if not path.is_relative_to(prefix_root):
            raise AssertionError(f"exported include directory escapes install prefix: {entry}")

    joined_include_dirs = ";".join(include_dirs)
    export_files = sorted(prefix.rglob("PulpTargets*.cmake"))
    if not export_files:
        raise AssertionError("installed SDK has no discoverable PulpTargets export")
    export_metadata = "\n".join(path.read_text() for path in export_files)
    for raw_location in re.findall(
        r'IMPORTED_LOCATION(?:_[A-Z0-9_]+)?\s+"([^"]+)"', export_metadata
    ):
        expanded = raw_location.replace("${_IMPORT_PREFIX}", str(prefix))
        location = pathlib.Path(expanded)
        if location.is_absolute():
            resolved = location.resolve()
            if not resolved.is_relative_to(prefix_root):
                raise AssertionError(
                    f"imported artifact escapes install prefix: {raw_location}"
                )
            if not resolved.is_file():
                raise AssertionError(f"imported artifact is missing: {raw_location}")
    evaluated_inputs = ";".join(
        [joined_include_dirs, *compile_fragments, *link_fragments, *artifact_paths, export_metadata]
    )
    for forbidden in forbidden_roots:
        if any(
            spelling in evaluated_inputs
            for spelling in {str(forbidden), str(forbidden.resolve())}
        ):
            raise AssertionError(f"consumer compile inputs leak checkout path: {forbidden}")
    installed = {str(prefix / "include"), str((prefix / "include").resolve())}
    if not any(spelling in joined_include_dirs for spelling in installed):
        raise AssertionError("exported target omits the installed include directory")


def expect_failure(label: str, operation) -> None:
    try:
        operation()
    except (AssertionError, RuntimeError, json.JSONDecodeError):
        return
    raise AssertionError(f"negative control unexpectedly passed: {label}")


def configure_build_run(
    cmake: str,
    prefix: pathlib.Path,
    project: pathlib.Path,
    source: str,
    target: str,
    generator: str | None,
    generator_options: list[str],
    configuration: str | None,
    forbidden_roots: list[pathlib.Path],
    use_pulp_dir: bool = True,
) -> None:
    configured = configure_consumer(
        cmake,
        prefix,
        project,
        source=source,
        target=target,
        generator=generator,
        generator_options=generator_options,
        configuration=configuration,
        use_pulp_dir=use_pulp_dir,
    )
    if configured.returncode != 0:
        raise RuntimeError(
            f"installed consumer configure failed:\n{configured.stdout}\n{configured.stderr}"
        )
    inspect_isolation(prefix, project, configuration, forbidden_roots)
    built = build_consumer(cmake, project, configuration)
    if built.returncode != 0:
        raise RuntimeError(
            f"installed consumer build failed:\n{built.stdout}\n{built.stderr}"
        )
    executed = run([str(executable_path(project, configuration))], cwd=project)
    if executed.returncode != 0:
        raise RuntimeError(f"installed consumer exited {executed.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator")
    parser.add_argument("--config", default="")
    parser.add_argument("--generator-platform")
    parser.add_argument("--generator-toolset")
    parser.add_argument("--toolchain-file")
    parser.add_argument("--osx-architectures")
    parser.add_argument("--osx-sysroot")
    parser.add_argument("--osx-deployment-target")
    args = parser.parse_args()
    configuration = args.config or None
    if configuration and not configuration.replace("_", "").isalnum():
        raise RuntimeError(f"invalid build configuration: {configuration!r}")
    build_dir = args.build_dir.resolve()
    cache = (build_dir / "CMakeCache.txt").read_text()
    source_line = next(
        (line for line in cache.splitlines() if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=")),
        None,
    )
    if source_line is None:
        raise RuntimeError("build CMakeCache.txt does not identify its source checkout")
    source_root = pathlib.Path(source_line.split("=", 1)[1]).resolve()
    probes, owners, addresses = load_source_contract(source_root)
    generator = args.generator or ("Ninja" if shutil.which("ninja") else None)
    generator_options: list[str] = []
    forwarded_definitions: set[str] = set()
    if args.generator_platform:
        generator_options.extend(["-A", args.generator_platform])
    if args.generator_toolset:
        generator_options.extend(["-T", args.generator_toolset])
    for name, value in (
        ("CMAKE_TOOLCHAIN_FILE", args.toolchain_file),
        ("CMAKE_OSX_ARCHITECTURES", args.osx_architectures),
        ("CMAKE_OSX_SYSROOT", args.osx_sysroot),
        ("CMAKE_OSX_DEPLOYMENT_TARGET", args.osx_deployment_target),
    ):
        if value:
            generator_options.append(f"-D{name}={value}")
            forwarded_definitions.add(name)
    configuration_suffix = (
        "_" + "".join(
            char if char.isalnum() else "_" for char in configuration.upper()
        )
        if configuration
        else ""
    )
    cache_definitions = [
        "CMAKE_C_COMPILER",
        "CMAKE_CXX_COMPILER",
        "CMAKE_C_COMPILER_TARGET",
        "CMAKE_CXX_COMPILER_TARGET",
        "CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN",
        "CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN",
        "CMAKE_C_FLAGS",
        "CMAKE_CXX_FLAGS",
        "CMAKE_EXE_LINKER_FLAGS",
        "CMAKE_MSVC_RUNTIME_LIBRARY",
        "CMAKE_TOOLCHAIN_FILE",
        "CMAKE_OSX_ARCHITECTURES",
        "CMAKE_OSX_SYSROOT",
        "CMAKE_OSX_DEPLOYMENT_TARGET",
    ]
    if configuration_suffix:
        cache_definitions.extend(
            [
                "CMAKE_C_FLAGS" + configuration_suffix,
                "CMAKE_CXX_FLAGS" + configuration_suffix,
                "CMAKE_EXE_LINKER_FLAGS" + configuration_suffix,
            ]
        )
    for name in cache_definitions:
        if name in forwarded_definitions:
            continue
        value = cache_value(cache, name)
        if value:
            generator_options.append(f"-D{name}={value}")
            forwarded_definitions.add(name)

    with tempfile.TemporaryDirectory(prefix="pulp-installed-capability-") as temp:
        root = pathlib.Path(temp)
        prefix = root / "sdk"
        install_command = [args.cmake, "--install", str(build_dir)]
        if configuration:
            install_command.extend(["--config", configuration])
        install_command.extend(["--prefix", str(prefix)])
        installed = run(install_command, cwd=root)
        if installed.returncode != 0:
            raise RuntimeError(f"SDK install failed:\n{installed.stdout}\n{installed.stderr}")
        document = load_installed_manifest(prefix, owners)
        installed_keys = {row["key"] for row in document["capabilities"]}
        if set(probes) != installed_keys:
            raise RuntimeError("consumer link probes do not exactly cover installed capabilities")

        schema = prefix / "share/pulp/agent-capabilities.schema.json"
        held_schema = schema.with_suffix(".held")
        schema.rename(held_schema)
        try:
            expect_failure("missing installed schema", lambda: load_installed_manifest(prefix, owners))
        finally:
            held_schema.rename(schema)

        manifest = prefix / "share/pulp/agent-capabilities.json"
        original_manifest = manifest.read_text()
        invalid_manifest = copy.deepcopy(document)
        invalid_manifest["capabilities"][0]["summary"] = 42
        manifest.write_text(json.dumps(invalid_manifest, indent=2) + "\n")
        try:
            expect_failure(
                "installed manifest violates installed schema",
                lambda: load_installed_manifest(prefix, owners),
            )
        finally:
            manifest.write_text(original_manifest)

        missing_determinism = copy.deepcopy(document)
        missing_determinism["capabilities"][0].pop("determinism")
        manifest.write_text(json.dumps(missing_determinism, indent=2) + "\n")
        try:
            expect_failure(
                "required determinism row is missing",
                lambda: load_installed_manifest(prefix, owners),
            )
        finally:
            manifest.write_text(original_manifest)

        unknown_feature = copy.deepcopy(document)
        unknown_feature["required_features"].append("future-contract-v99")
        expect_failure(
            "consumer rejects an unknown required feature",
            lambda: validate_required_features(
                unknown_feature,
                required_by_consumer={"determinism-contract-v1"},
            ),
        )
        manifest.write_text(json.dumps(unknown_feature, indent=2) + "\n")
        try:
            expect_failure(
                "unknown required feature",
                lambda: load_installed_manifest(prefix, owners),
            )
        finally:
            manifest.write_text(original_manifest)

        for label, malformed in (
            ("null required feature list", None),
            ("numeric required feature list", 7),
            ("non-string required feature list", [7]),
            (
                "mixed required feature list",
                ["determinism-contract-v1", 7],
            ),
        ):
            malformed_features = copy.deepcopy(document)
            malformed_features["required_features"] = malformed
            expect_failure(
                label,
                lambda fixture=malformed_features: validate_required_features(
                    fixture,
                    required_by_consumer={"determinism-contract-v1"},
                ),
            )

        legacy_minor_zero = copy.deepcopy(document)
        legacy_minor_zero["schema_minor"] = 0
        legacy_minor_zero["required_features"].remove("determinism-contract-v1")
        for row in legacy_minor_zero["capabilities"]:
            row.pop("determinism")
        installed_schema_document = json.loads(schema.read_text())
        if json_schema_lite.validate(legacy_minor_zero, installed_schema_document):
            raise RuntimeError("minor-0 compatibility fixture no longer parses")
        expect_failure(
            "determinism-requiring consumer rejects minor-0 unspecified contract",
            lambda: validate_required_features(
                legacy_minor_zero,
                required_by_consumer={"determinism-contract-v1"},
            ),
        )

        held_manifest = manifest.with_suffix(".held")
        manifest.rename(held_manifest)
        try:
            expect_failure("missing installed manifest", lambda: load_installed_manifest(prefix, owners))
        finally:
            held_manifest.rename(manifest)

        first = document["capabilities"][0]
        first_binding = first["bindings"][0]
        header = prefix / "include" / first_binding["include"]
        held_header = header.with_suffix(header.suffix + ".held")
        header.rename(held_header)
        try:
            expect_failure("missing advertised header", lambda: load_installed_manifest(prefix, owners))
        finally:
            held_header.rename(header)

        wrong_target = copy.deepcopy(document)
        wrong_target["capabilities"][0]["bindings"][0]["target"] = "Pulp::platform"
        expect_failure("wrong minimal target", lambda: validate_target_ownership(wrong_target, owners))

        # Export metadata is part of the isolation boundary too. Pointing an
        # imported target at a real checkout archive would otherwise configure,
        # link, and run successfully while silently bypassing the installed SDK.
        export_config = next(
            path for path in prefix.rglob("PulpTargets-*.cmake")
            if 'Import target "Pulp::audio"' in path.read_text()
        )
        original_export = export_config.read_text()
        imported_match = re.search(
            r'Import target "Pulp::audio".*?IMPORTED_LOCATION(?:_[A-Z0-9_]+)?\s+"([^"]+)"',
            original_export,
            re.DOTALL,
        )
        if imported_match is None:
            raise RuntimeError("could not locate installed pulp-audio imported artifact")
        archive_name = pathlib.Path(imported_match.group(1)).name
        archive_candidates = [
            path.resolve() for path in build_dir.rglob(archive_name) if path.is_file()
        ]
        if configuration:
            configured_candidates = [
                path for path in archive_candidates if configuration in path.parts
            ]
            if configured_candidates:
                archive_candidates = configured_candidates
        if len(archive_candidates) != 1:
            raise RuntimeError(
                f"expected one build-tree artifact for Pulp::audio, got {archive_candidates}"
            )
        audio_archive = archive_candidates[0]
        export_config.write_text(
            original_export.replace(imported_match.group(1), audio_archive.as_posix())
        )
        leaked_archive_project = root / "leaked-imported-archive"
        try:
            configured = configure_consumer(
                args.cmake, prefix, leaked_archive_project,
                source=binding_source(first, first_binding, addresses),
                target=first_binding["target"], generator=generator,
                generator_options=generator_options, configuration=configuration,
            )
            if configured.returncode != 0 or build_consumer(
                args.cmake, leaked_archive_project, configuration
            ).returncode != 0:
                raise RuntimeError("build-tree archive mutation did not otherwise build")
            expect_failure(
                "imported target points at build-tree archive",
                lambda: inspect_isolation(
                    prefix, leaked_archive_project, configuration, [source_root, build_dir]
                ),
            )
        finally:
            export_config.write_text(original_export)

        specialized = [
            (
                "pulp/signal/fft_backend.hpp", "Pulp::signal-fft-backend",
                "#include <pulp/signal/fft_backend.hpp>\nint main(){ auto *volatile p = &pulp::signal::fft_backend_name; return p(pulp::signal::FftBackend::auto_) == nullptr; }\n",
            ),
            (
                "pulp/signal/modal_spec.hpp", "Pulp::signal-modal-spec",
                "#include <pulp/signal/modal_spec.hpp>\nint main(){ auto *volatile p = &pulp::signal::parse_modal_spec; return p(\"{}\", nullptr).has_value(); }\n",
            ),
        ]
        for index, (include, target, source) in enumerate(specialized):
            if owners.get(include) != target:
                raise AssertionError(f"specialized owner drift for {include}")
            configure_build_run(
                args.cmake, prefix, root / f"specialized-owner-{index}", source,
                target, generator, generator_options, configuration,
                [source_root, build_dir],
            )
            generic_project = root / f"specialized-wrong-generic-{index}"
            configured = configure_consumer(
                args.cmake, prefix, generic_project, source=source,
                target="Pulp::signal", generator=generator,
                generator_options=generator_options, configuration=configuration,
            )
            if configured.returncode == 0 and build_consumer(
                args.cmake, generic_project, configuration
            ).returncode == 0:
                raise AssertionError(f"generic signal target unexpectedly owns {include}")

        # CMAKE_PREFIX_PATH discovery must not assume a literal lib directory.
        config_exports = sorted(prefix.rglob("PulpTargets.cmake"))
        if len(config_exports) != 1:
            raise RuntimeError(f"expected one installed PulpTargets.cmake, got {config_exports}")
        config_dir = config_exports[0].parent
        alternate_config = prefix / "share/cmake/Pulp"
        if alternate_config == config_dir:
            alternate_config = prefix / "share/Pulp/cmake"
        alternate_config.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(config_dir), str(alternate_config))
        try:
            configure_build_run(
                args.cmake, prefix, root / "lib64-layout",
                binding_source(first, first_binding, addresses), first_binding["target"],
                generator, generator_options, configuration, [source_root, build_dir],
                use_pulp_dir=False,
            )
        finally:
            config_dir.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(alternate_config), str(config_dir))

        forbidden = [source_root, build_dir]
        leaked_flag_project = root / "leaked-compiler-flag"
        configured = configure_consumer(
            args.cmake,
            prefix,
            leaked_flag_project,
            source=binding_source(first, first_binding, addresses),
            target=first_binding["target"],
            generator=generator,
            generator_options=[
                *generator_options,
                f"-DCMAKE_CXX_FLAGS=-I{source_root}",
            ],
            configuration=configuration,
        )
        if configured.returncode != 0:
            raise RuntimeError(
                "checkout-path compiler-flag control did not configure:\n"
                f"{configured.stdout}\n{configured.stderr}"
            )
        expect_failure(
            "checkout path injected through compiler flags",
            lambda: inspect_isolation(
                prefix, leaked_flag_project, configuration, forbidden
            ),
        )

        linked_row = next(
            row
            for row in document["capabilities"]
            if row["key"] == "audio.instrument-voice-allocator"
        )
        wrong_target_project = root / "wrong-target-build"
        configured = configure_consumer(
            args.cmake,
            prefix,
            wrong_target_project,
            source=capability_source(
                linked_row, probes[linked_row["key"]], addresses
            ),
            target="Pulp::platform",
            generator=generator,
            generator_options=generator_options,
            configuration=configuration,
        )
        if configured.returncode == 0 and build_consumer(
            args.cmake, wrong_target_project, configuration
        ).returncode == 0:
            raise AssertionError(
                "wrong-target negative control unexpectedly compiled and linked"
            )

        missing_function_project = root / "missing-function-reference"
        configured = configure_consumer(
            args.cmake,
            prefix,
            missing_function_project,
            source=(
                "extern void pulp_missing_capability_symbol();\n"
                "int main() { auto *volatile binding = "
                "&pulp_missing_capability_symbol; return binding == nullptr; }\n"
            ),
            target="Pulp::platform",
            generator=generator,
            generator_options=generator_options,
            configuration=configuration,
        )
        if configured.returncode != 0:
            raise RuntimeError(
                "missing-function negative control did not reach linking:\n"
                f"{configured.stdout}\n{configured.stderr}"
            )
        if build_consumer(
            args.cmake, missing_function_project, configuration
        ).returncode == 0:
            raise AssertionError(
                "volatile missing-function reference unexpectedly linked"
            )

        target_exports = sorted(prefix.rglob("PulpTargets.cmake"))
        if len(target_exports) != 1:
            raise RuntimeError(f"expected one installed PulpTargets.cmake, got {target_exports}")
        targets = target_exports[0]
        held_targets = targets.with_suffix(".held")
        targets.rename(held_targets)
        try:
            failed = configure_consumer(
                args.cmake,
                prefix,
                root / "missing-export",
                source=binding_source(first, first_binding, addresses),
                target=first_binding["target"],
                generator=generator,
                generator_options=generator_options,
                configuration=configuration,
            )
            if failed.returncode == 0:
                raise AssertionError(
                    "negative control unexpectedly configured without PulpTargets.cmake"
                )
        finally:
            held_targets.rename(targets)

        proofs = 0
        for row_index, row in enumerate(document["capabilities"]):
            targets_for_row = {binding["target"] for binding in row["bindings"]}
            if len(targets_for_row) != 1:
                raise RuntimeError(
                    f"{row['key']} spans multiple minimal targets; split its capability contract"
                )
            target = next(iter(targets_for_row))
            configure_build_run(
                args.cmake,
                prefix,
                root / f"capability-{row_index}",
                capability_source(row, probes[row["key"]], addresses),
                target,
                generator,
                generator_options,
                configuration,
                forbidden,
            )
            proofs += 1
            for binding_index, binding in enumerate(row["bindings"]):
                configure_build_run(
                    args.cmake,
                    prefix,
                    root / f"binding-{row_index}-{binding_index}",
                    binding_source(row, binding, addresses),
                    binding["target"],
                    generator,
                    generator_options,
                    configuration,
                    forbidden,
                )
                proofs += 1

    print(
        "agent-capabilities-installed-sdk: "
        f"15 negative/install controls and {proofs} independent capability/binding "
        f"configure-build-run proofs passed with {generator or 'default generator'} "
        f"in {configuration or 'the default single configuration'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
