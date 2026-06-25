#!/usr/bin/env python3
import argparse
import json
import re
import sys
from pathlib import Path


ROOT_KEYS = {
    "schema_version",
    "roadmap_item",
    "baseline_kind",
    "pulp_base_commit",
    "captured_at_utc",
    "schema_boundary",
    "sdk_recipe",
    "consumers",
}
SCHEMA_BOUNDARY_KEYS = {
    "project_ir_is_not_design_ir",
    "design_ir_consumers",
    "project_ir_consumers",
}
SDK_RECIPE_KEYS = {
    "prefix_env",
    "source_build_dir",
    "configure_command",
    "build_command",
    "install_command",
    "required_options",
    "expected_artifacts",
}
CONSUMER_KEYS = {
    "repo",
    "category",
    "remote",
    "checkout_path_candidates",
    "reviewed_head",
    "live_head",
    "live_dirty",
    "dependency_surfaces",
    "affected_roadmap_items",
    "required_commands",
}
COMMAND_KEYS = {
    "id",
    "description",
    "working_dir",
    "command",
    "required_for",
    "expected",
}
EXPECTED_KEYS = {"type", "value"}
EXPECTED_CONSUMERS = {
    "pulp-view-embed": "embed_abi",
    "pulp-embed-iplug2": "embed_adapter",
    "pulp-embed-juce": "embed_adapter",
}
DESIGN_IR_CONSUMERS = {
    "pulp-view-embed",
    "pulp-embed-iplug2",
    "pulp-embed-juce",
}
# Project-importer consumers (ProjectIR) are tracked in private sibling repos
# and are intentionally NOT listed in this public manifest, so this set is
# empty here. The project_importer category + ProjectIR/DesignIR boundary
# checks below are retained so a future public importer validates correctly.
PROJECT_IR_CONSUMERS = set()
ALLOWED_CATEGORIES = {"embed_abi", "embed_adapter", "project_importer"}


def load_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path}: invalid JSON: {exc}") from exc


def require(condition, message, errors):
    if not condition:
        errors.append(message)


def require_exact_keys(value, expected, label, errors):
    require(isinstance(value, dict), f"{label} must be an object", errors)
    if not isinstance(value, dict):
        return
    require(
        set(value.keys()) == expected,
        f"{label} keys must be exactly: " + ", ".join(sorted(expected)),
        errors,
    )


def require_nonempty_string(value, label, errors):
    require(isinstance(value, str) and value,
            f"{label} must be a non-empty string",
            errors)


def require_sha(value, label, errors):
    require(isinstance(value, str) and
            re.fullmatch(r"[0-9a-f]{40}", value) is not None,
            f"{label} must be a 40-character lowercase hex commit",
            errors)


def require_string_list(value, label, errors, min_count=1):
    require(isinstance(value, list) and len(value) >= min_count,
            f"{label} must be a non-empty array",
            errors)
    if isinstance(value, list):
        for index, item in enumerate(value):
            require_nonempty_string(item, f"{label}[{index}]", errors)


def validate_sdk_recipe(recipe, errors):
    require_exact_keys(recipe, SDK_RECIPE_KEYS, "sdk_recipe", errors)
    if not isinstance(recipe, dict):
        return
    require(recipe.get("prefix_env") == "PULP_SDK_PREFIX",
            "sdk_recipe.prefix_env must be PULP_SDK_PREFIX",
            errors)
    for key in ("source_build_dir", "configure_command",
                "build_command", "install_command"):
        require_nonempty_string(recipe.get(key), f"sdk_recipe.{key}", errors)
    configure = recipe.get("configure_command")
    if isinstance(configure, str):
        for fragment in (
            "cmake -S",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_INSTALL_PREFIX=\"$PULP_SDK_PREFIX\"",
        ):
            require(fragment in configure,
                    f"sdk_recipe.configure_command must contain {fragment}",
                    errors)
    build = recipe.get("build_command")
    if isinstance(build, str):
        require("cmake --build" in build,
                "sdk_recipe.build_command must run cmake --build",
                errors)
    install = recipe.get("install_command")
    if isinstance(install, str):
        for fragment in ("cmake --install", "--prefix \"$PULP_SDK_PREFIX\""):
            require(fragment in install,
                    f"sdk_recipe.install_command must contain {fragment}",
                    errors)
    required_options = recipe.get("required_options")
    require_string_list(required_options, "sdk_recipe.required_options", errors)
    if isinstance(required_options, list):
        for option in (
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_INSTALL_PREFIX=\"$PULP_SDK_PREFIX\"",
        ):
            require(option in required_options,
                    f"sdk_recipe.required_options must include {option}",
                    errors)
    artifacts = recipe.get("expected_artifacts")
    require_string_list(artifacts, "sdk_recipe.expected_artifacts", errors)
    if isinstance(artifacts, list):
        require("$PULP_SDK_PREFIX/lib/cmake/Pulp/PulpConfig.cmake" in artifacts,
                "sdk_recipe.expected_artifacts must include "
                "$PULP_SDK_PREFIX/lib/cmake/Pulp/PulpConfig.cmake",
                errors)


def validate_schema_boundary(boundary, errors):
    require_exact_keys(boundary, SCHEMA_BOUNDARY_KEYS, "schema_boundary", errors)
    if not isinstance(boundary, dict):
        return
    require(boundary.get("project_ir_is_not_design_ir") is True,
            "schema_boundary.project_ir_is_not_design_ir must be true",
            errors)
    design = boundary.get("design_ir_consumers")
    project = boundary.get("project_ir_consumers")
    require(set(design or []) == DESIGN_IR_CONSUMERS,
            "schema_boundary.design_ir_consumers must list embed consumers",
            errors)
    require(set(project or []) == PROJECT_IR_CONSUMERS,
            "schema_boundary.project_ir_consumers must list project importers",
            errors)
    require(set(design or []).isdisjoint(set(project or [])),
            "schema_boundary design and project consumers must not overlap",
            errors)


def validate_command(command, label, consumer, errors):
    require_exact_keys(command, COMMAND_KEYS, label, errors)
    if not isinstance(command, dict):
        return
    for key in ("id", "description", "working_dir", "command"):
        require_nonempty_string(command.get(key), f"{label}.{key}", errors)
    require_string_list(command.get("required_for"), f"{label}.required_for", errors)
    expected = command.get("expected")
    require_exact_keys(expected, EXPECTED_KEYS, f"{label}.expected", errors)
    if isinstance(expected, dict):
        require(expected.get("type") in {"artifact", "log"},
                f"{label}.expected.type must be artifact or log",
                errors)
        require_nonempty_string(expected.get("value"),
                                f"{label}.expected.value",
                                errors)

    command_text = command.get("command")
    if not isinstance(command_text, str):
        return

    category = consumer.get("category")
    repo = consumer.get("repo")
    if category in {"embed_abi", "embed_adapter"}:
        require("CMAKE_PREFIX_PATH=\"$PULP_SDK_PREFIX\"" in command_text or
                "cmake --build" in command_text or
                "ctest --test-dir" in command_text or
                "PULP_EMBED_SELFCHECK=1" in command_text,
                f"{label}.command must use the installed Pulp SDK, CTest, "
                "or a self-check",
                errors)
    if category == "embed_adapter":
        require("PULP_VIEW_EMBED_DIR=\"$PULP_VIEW_EMBED_REPO\"" in command_text or
                "PULP_EMBED_SELFCHECK=1" in command_text or
                "cmake --build" in command_text,
                f"{label}.command must reference the pulp-view-embed checkout "
                "or a built adapter artifact",
                errors)
    if category == "project_importer":
        require("ProjectIR" in " ".join(consumer.get("dependency_surfaces", [])),
                f"{repo} must declare ProjectIR dependency surface",
                errors)
        require("DesignIR" not in " ".join(consumer.get("dependency_surfaces", [])),
                f"{repo} must not claim DesignIR covers ProjectIR",
                errors)


def validate_consumer(consumer, index, errors):
    label = f"consumers[{index}]"
    require_exact_keys(consumer, CONSUMER_KEYS, label, errors)
    if not isinstance(consumer, dict):
        return 0
    repo = consumer.get("repo")
    require(repo in EXPECTED_CONSUMERS,
            f"{label}.repo must be one of: " +
            ", ".join(sorted(EXPECTED_CONSUMERS)),
            errors)
    category = consumer.get("category")
    require(category in ALLOWED_CATEGORIES,
            f"{label}.category must be one of: " +
            ", ".join(sorted(ALLOWED_CATEGORIES)),
            errors)
    if repo in EXPECTED_CONSUMERS:
        require(category == EXPECTED_CONSUMERS[repo],
                f"{label}.category must be {EXPECTED_CONSUMERS[repo]} for {repo}",
                errors)
    remote = consumer.get("remote")
    require(isinstance(remote, str) and
            remote == f"https://github.com/danielraffel/{repo}.git",
            f"{label}.remote must be the danielraffel/{repo} GitHub remote",
            errors)
    paths = consumer.get("checkout_path_candidates")
    require_string_list(paths, f"{label}.checkout_path_candidates", errors, 2)
    if isinstance(paths, list) and repo:
        require(any(path == f"/Volumes/Workshop/Code/{repo}" for path in paths),
                f"{label}.checkout_path_candidates must include "
                f"/Volumes/Workshop/Code/{repo}",
                errors)
        require(any(path == f"/Users/danielraffel/Code/{repo}" for path in paths),
                f"{label}.checkout_path_candidates must include "
                f"/Users/danielraffel/Code/{repo}",
                errors)
    require_sha(consumer.get("reviewed_head"), f"{label}.reviewed_head", errors)
    require_sha(consumer.get("live_head"), f"{label}.live_head", errors)
    require(isinstance(consumer.get("live_dirty"), bool),
            f"{label}.live_dirty must be a boolean",
            errors)
    require_string_list(consumer.get("dependency_surfaces"),
                        f"{label}.dependency_surfaces",
                        errors)
    require_string_list(consumer.get("affected_roadmap_items"),
                        f"{label}.affected_roadmap_items",
                        errors)
    if isinstance(consumer.get("affected_roadmap_items"), list):
        require("P0.4" in consumer["affected_roadmap_items"],
                f"{label}.affected_roadmap_items must include P0.4",
                errors)

    commands = consumer.get("required_commands")
    require(isinstance(commands, list) and len(commands) >= 2,
            f"{label}.required_commands must contain at least 2 commands",
            errors)
    command_count = 0
    seen_ids = set()
    if isinstance(commands, list):
        command_count = len(commands)
        for command_index, command in enumerate(commands):
            command_label = f"{label}.required_commands[{command_index}]"
            validate_command(command, command_label, consumer, errors)
            if isinstance(command, dict):
                command_id = command.get("id")
                require(command_id not in seen_ids,
                        f"{command_label}.id duplicates {command_id}",
                        errors)
                seen_ids.add(command_id)

    if category == "project_importer" and isinstance(commands, list):
        command_text = "\n".join(
            command.get("command", "")
            for command in commands
            if isinstance(command, dict))
        require("./run_spike.sh" in command_text,
                f"{label}.required_commands must run ./run_spike.sh",
                errors)
        require("python3 -m unittest spike.test_spi" in command_text,
                f"{label}.required_commands must run spike.test_spi",
                errors)
    if category in {"embed_abi", "embed_adapter"}:
        surfaces = " ".join(consumer.get("dependency_surfaces", []))
        require("pulp_view_embed.h C ABI" in surfaces,
                f"{label}.dependency_surfaces must include pulp_view_embed.h C ABI",
                errors)
        require("installed Pulp SDK CMake package" in surfaces,
                f"{label}.dependency_surfaces must include installed Pulp SDK "
                "CMake package",
                errors)

    return command_count


def validate_manifest(manifest):
    errors = []
    require_exact_keys(manifest, ROOT_KEYS, "manifest root", errors)
    if not isinstance(manifest, dict):
        return errors, 0, 0
    require(manifest.get("schema_version") == 1,
            "schema_version must be 1",
            errors)
    require(manifest.get("roadmap_item") == "P0.4",
            "roadmap_item must be P0.4",
            errors)
    require(manifest.get("baseline_kind") ==
            "downstream_consumer_validation_manifest",
            "baseline_kind must be downstream_consumer_validation_manifest",
            errors)
    require_sha(manifest.get("pulp_base_commit"), "pulp_base_commit", errors)
    captured_at = manifest.get("captured_at_utc")
    require(isinstance(captured_at, str) and
            re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z",
                         captured_at) is not None,
            "captured_at_utc must be an ISO-8601 UTC timestamp",
            errors)

    validate_schema_boundary(manifest.get("schema_boundary"), errors)
    validate_sdk_recipe(manifest.get("sdk_recipe"), errors)

    consumers = manifest.get("consumers")
    require(isinstance(consumers, list) and len(consumers) == len(EXPECTED_CONSUMERS),
            "consumers must contain exactly the expected downstream repos",
            errors)
    seen = set()
    command_count = 0
    if isinstance(consumers, list):
        for index, consumer in enumerate(consumers):
            if isinstance(consumer, dict):
                repo = consumer.get("repo")
                require(repo not in seen,
                        f"consumers[{index}].repo duplicates {repo}",
                        errors)
                seen.add(repo)
            command_count += validate_consumer(consumer, index, errors)
    require(seen == set(EXPECTED_CONSUMERS),
            "consumers must contain exactly the expected downstream repos",
            errors)

    return errors, len(consumers) if isinstance(consumers, list) else 0, command_count


def main():
    parser = argparse.ArgumentParser(
        description="Validate the P0.4 downstream consumer validation manifest.")
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()

    try:
        manifest = load_json(args.manifest)
        errors, consumer_count, command_count = validate_manifest(manifest)
    except OSError as exc:
        print(exc, file=sys.stderr)
        return 2
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 2

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(
        "downstream_validation_manifest_verified=true "
        f"consumers={consumer_count} commands={command_count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
