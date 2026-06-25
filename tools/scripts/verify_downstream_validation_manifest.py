#!/usr/bin/env python3
"""Validate the downstream consumer validation manifest.

The manifest is intentionally a checklist, not an executor. Pulp's external
consumer repos are not guaranteed to be checked out on every CI machine, so the
default mode verifies schema, required consumers, canonical SDK recipe shape,
and command/evidence coverage. Use --check-local to add advisory checkout
presence and HEAD reporting on a developer machine.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


EXPECTED_CONSUMERS = {
    "pulp-view-embed": "ViewEmbed C ABI and installed SDK consumption",
    "pulp-embed-iplug2": "iPlug2 editor lifecycle through pulp-view-embed",
    "pulp-embed-juce": "JUCE component lifecycle through pulp-view-embed",
}

EMBED_CONSUMERS = {
    "pulp-view-embed",
    "pulp-embed-iplug2",
    "pulp-embed-juce",
}

# ProjectIR importer consumers live in private sibling repos and are
# intentionally not listed in this public manifest, so this set is empty.
# The import-specific checks below are retained for a future public importer.
IMPORT_CONSUMERS: set[str] = set()

ROOT_KEYS = {
    "schema_version",
    "roadmap_item",
    "manifest_kind",
    "reviewed_at_utc",
    "canonical_sdk_recipe",
    "consumers",
}

RECIPE_KEYS = {
    "build_type",
    "source_dir",
    "build_dir",
    "install_prefix_env",
    "configure",
    "build",
    "install",
    "required_verification",
}

CONSUMER_KEYS = {
    "name",
    "repo_url",
    "local_checkout_hint",
    "reviewed_head",
    "contract",
    "dependency_surfaces",
    "required_checks",
}

CHECK_KEYS = {"id", "command", "expected_evidence"}


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path}: invalid JSON: {exc}") from exc


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def require_exact_keys(value: Any, expected: set[str], label: str, errors: list[str]) -> None:
    require(isinstance(value, dict), f"{label} must be an object", errors)
    if isinstance(value, dict):
        require(
            set(value.keys()) == expected,
            f"{label} keys must be exactly: " + ", ".join(sorted(expected)),
            errors,
        )


def require_string_list(value: Any, label: str, errors: list[str], min_len: int = 1) -> None:
    require(isinstance(value, list) and len(value) >= min_len,
            f"{label} must be an array with at least {min_len} item(s)",
            errors)
    if isinstance(value, list):
        for index, item in enumerate(value):
            require(isinstance(item, str) and item,
                    f"{label}[{index}] must be a non-empty string",
                    errors)


def validate_recipe(recipe: Any, errors: list[str]) -> None:
    require_exact_keys(recipe, RECIPE_KEYS, "canonical_sdk_recipe", errors)
    if not isinstance(recipe, dict):
        return

    require(recipe.get("build_type") == "Release",
            "canonical_sdk_recipe.build_type must be Release",
            errors)
    require(recipe.get("install_prefix_env") == "PULP_SDK_PREFIX",
            "canonical_sdk_recipe.install_prefix_env must be PULP_SDK_PREFIX",
            errors)
    require(recipe.get("source_dir") == "${PULP_SOURCE_DIR}",
            "canonical_sdk_recipe.source_dir must be ${PULP_SOURCE_DIR}",
            errors)
    require(recipe.get("build_dir") == "${PULP_BUILD_DIR}",
            "canonical_sdk_recipe.build_dir must be ${PULP_BUILD_DIR}",
            errors)

    for key in ("configure", "build", "install", "required_verification"):
        require_string_list(recipe.get(key), f"canonical_sdk_recipe.{key}", errors)

    configure = recipe.get("configure") if isinstance(recipe.get("configure"), list) else []
    configure_text = " ".join(str(part) for part in configure)
    for token in (
        "cmake",
        "-S",
        "${PULP_SOURCE_DIR}",
        "-B",
        "${PULP_BUILD_DIR}",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_INSTALL_PREFIX=${PULP_SDK_PREFIX}",
    ):
        require(token in configure_text,
                f"canonical_sdk_recipe.configure must include {token}",
                errors)

    install = recipe.get("install") if isinstance(recipe.get("install"), list) else []
    install_text = " ".join(str(part) for part in install)
    require("--prefix ${PULP_SDK_PREFIX}" in install_text,
            "canonical_sdk_recipe.install must install to ${PULP_SDK_PREFIX}",
            errors)

    verification = recipe.get("required_verification")
    if isinstance(verification, list):
        verification_text = "\n".join(str(item) for item in verification)
        for token in ("Pulp_DIR=", "find_package(Pulp REQUIRED CONFIG)", "clean build"):
            require(token in verification_text,
                    f"canonical_sdk_recipe.required_verification must mention {token}",
                    errors)


def validate_check(check: Any, consumer_name: str, errors: list[str]) -> str | None:
    require_exact_keys(check, CHECK_KEYS, f"{consumer_name}.required_checks[]", errors)
    if not isinstance(check, dict):
        return None
    check_id = check.get("id")
    require(isinstance(check_id, str) and check_id,
            f"{consumer_name}.required_checks[].id must be a non-empty string",
            errors)
    for key in ("command", "expected_evidence"):
        require(isinstance(check.get(key), str) and check[key],
                f"{consumer_name}.required_checks[].{key} must be a non-empty string",
                errors)
    return check_id if isinstance(check_id, str) else None


def validate_consumers(consumers: Any, errors: list[str]) -> None:
    require(isinstance(consumers, list) and len(consumers) == len(EXPECTED_CONSUMERS),
            "consumers must list exactly the roadmap P0.4 downstream repos",
            errors)
    if not isinstance(consumers, list):
        return

    seen: set[str] = set()
    for index, consumer in enumerate(consumers):
        label = f"consumers[{index}]"
        require_exact_keys(consumer, CONSUMER_KEYS, label, errors)
        if not isinstance(consumer, dict):
            continue

        name = consumer.get("name")
        require(name in EXPECTED_CONSUMERS,
                f"{label}.name must be one of: " +
                ", ".join(sorted(EXPECTED_CONSUMERS)),
                errors)
        if isinstance(name, str):
            require(name not in seen, f"{label}.name duplicates {name}", errors)
            seen.add(name)
            require(consumer.get("contract") == EXPECTED_CONSUMERS.get(name),
                    f"{label}.contract must be {EXPECTED_CONSUMERS.get(name)!r}",
                    errors)

        repo_url = consumer.get("repo_url")
        require(isinstance(repo_url, str) and repo_url.startswith("https://github.com/"),
                f"{label}.repo_url must be a GitHub HTTPS URL",
                errors)
        local_hint = consumer.get("local_checkout_hint")
        require(isinstance(local_hint, str) and
                (local_hint.startswith("/Users/danielraffel/Code/") or
                 local_hint.startswith("/Volumes/Workshop/Code/")),
                f"{label}.local_checkout_hint must point under a known local code root",
                errors)
        reviewed_head = consumer.get("reviewed_head")
        require(isinstance(reviewed_head, str) and
                re.fullmatch(r"[0-9a-f]{7,40}", reviewed_head) is not None,
                f"{label}.reviewed_head must be a 7-40 character lowercase hex SHA",
                errors)
        require_string_list(consumer.get("dependency_surfaces"),
                            f"{label}.dependency_surfaces",
                            errors,
                            min_len=2)

        checks = consumer.get("required_checks")
        require(isinstance(checks, list) and checks,
                f"{label}.required_checks must be a non-empty array",
                errors)
        check_ids: set[str] = set()
        if isinstance(checks, list):
            for check in checks:
                check_id = validate_check(check, str(name), errors)
                if check_id is not None:
                    require(check_id not in check_ids,
                            f"{label}.required_checks duplicate id {check_id}",
                            errors)
                    check_ids.add(check_id)

        surfaces = consumer.get("dependency_surfaces")
        surfaces_text = "\n".join(surfaces) if isinstance(surfaces, list) else ""
        checks_text = json.dumps(checks) if isinstance(checks, list) else ""
        if name in EMBED_CONSUMERS:
            require("pulp_view_embed.h" in surfaces_text,
                    f"{label} must track pulp_view_embed.h",
                    errors)
            require("Pulp_DIR=${PULP_SDK_PREFIX}/lib/cmake/Pulp" in checks_text,
                    f"{label} must configure against installed Pulp_DIR",
                    errors)
            require("build" in check_ids,
                    f"{label} must include a build check",
                    errors)
        if name in IMPORT_CONSUMERS:
            require("ProjectIR" in surfaces_text,
                    f"{label} must track ProjectIR, not DesignIR",
                    errors)
            require("DesignIR" not in surfaces_text,
                    f"{label} must not claim DesignIR coverage for ProjectIR importers",
                    errors)
            require("importer-tests" in check_ids,
                    f"{label} must include importer-tests",
                    errors)

    require(seen == set(EXPECTED_CONSUMERS),
            "consumers must contain exactly: " + ", ".join(sorted(EXPECTED_CONSUMERS)),
            errors)


def validate_manifest(manifest: Any) -> list[str]:
    errors: list[str] = []
    require_exact_keys(manifest, ROOT_KEYS, "manifest root", errors)
    if not isinstance(manifest, dict):
        return errors
    require(manifest.get("schema_version") == 1,
            "schema_version must be 1",
            errors)
    require(manifest.get("roadmap_item") == "P0.4",
            "roadmap_item must be P0.4",
            errors)
    require(manifest.get("manifest_kind") == "downstream_consumer_validation",
            "manifest_kind must be downstream_consumer_validation",
            errors)
    reviewed_at = manifest.get("reviewed_at_utc")
    require(isinstance(reviewed_at, str) and
            re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", reviewed_at) is not None,
            "reviewed_at_utc must be an ISO-8601 UTC timestamp",
            errors)
    validate_recipe(manifest.get("canonical_sdk_recipe"), errors)
    validate_consumers(manifest.get("consumers"), errors)
    return errors


def git_head(path: Path) -> str | None:
    result = subprocess.run(
        ["git", "-C", str(path), "rev-parse", "--short", "HEAD"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def git_dirty(path: Path) -> bool | None:
    result = subprocess.run(
        ["git", "-C", str(path), "status", "--porcelain"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode != 0:
        return None
    return bool(result.stdout.strip())


def print_local_checkout_report(manifest: dict[str, Any], require_clean: bool) -> int:
    failures = 0
    for consumer in manifest.get("consumers", []):
        if not isinstance(consumer, dict):
            continue
        hint = Path(str(consumer.get("local_checkout_hint", "")))
        name = consumer.get("name")
        if not hint.exists():
            print(f"downstream_checkout name={name} status=missing path={hint}")
            if require_clean:
                failures += 1
            continue
        head = git_head(hint)
        dirty = git_dirty(hint)
        if head is None:
            print(f"downstream_checkout name={name} status=not_git path={hint}")
            if require_clean:
                failures += 1
        else:
            dirty_text = "unknown" if dirty is None else str(dirty).lower()
            print(
                f"downstream_checkout name={name} status=present "
                f"head={head} dirty={dirty_text} path={hint}")
            if require_clean and dirty:
                failures += 1
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Validate P0.4 downstream consumer validation manifest.")
    parser.add_argument(
        "manifest",
        type=Path,
        nargs="?",
        default=Path("tools/validation/downstream/consumer-validation.json"),
    )
    parser.add_argument(
        "--check-local",
        action="store_true",
        help="also print advisory local checkout presence and current HEADs",
    )
    parser.add_argument(
        "--require-clean",
        action="store_true",
        help="with --check-local, fail when a checkout is missing, non-git, or dirty",
    )
    args = parser.parse_args(argv)

    try:
        manifest = load_json(args.manifest)
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 2

    errors = validate_manifest(manifest)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    local_failures = 0
    if args.check_local and isinstance(manifest, dict):
        local_failures = print_local_checkout_report(manifest, args.require_clean)

    consumer_count = len(manifest.get("consumers", [])) if isinstance(manifest, dict) else 0
    print(
        "downstream_validation_manifest_ok=true "
        f"consumers={consumer_count} roadmap_item=P0.4")
    if local_failures:
        print(f"downstream_validation_local_clean=false failures={local_failures}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
