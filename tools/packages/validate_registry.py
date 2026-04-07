#!/usr/bin/env python3
"""Validate the Pulp package registry against its JSON schema."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REGISTRY = ROOT / "tools" / "packages" / "registry.json"
SCHEMA = ROOT / "tools" / "packages" / "registry-schema.json"

SLUG_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]*$")

ALLOWED_LICENSES = {
    "MIT", "MIT-0", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0",
    "ISC", "zlib", "BSL-1.0", "Unlicense", "CC0-1.0",
}

REJECTED_LICENSES = {
    "GPL-2.0", "GPL-2.0-only", "GPL-2.0-or-later",
    "GPL-3.0", "GPL-3.0-only", "GPL-3.0-or-later",
    "LGPL-2.1", "LGPL-2.1-only", "LGPL-2.1-or-later",
    "LGPL-3.0", "LGPL-3.0-only", "LGPL-3.0-or-later",
    "AGPL-3.0", "AGPL-3.0-only", "AGPL-3.0-or-later",
    "SSPL-1.0",
}

BUILD_STATUS_KEY_PATTERN = re.compile(r"^[A-Za-z]+-[a-z0-9]+$")


def load_json(path: Path) -> dict:
    if not path.exists():
        print(f"ERROR: {path} not found", file=sys.stderr)
        sys.exit(1)
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as e:
        print(f"ERROR: {path}: {e}", file=sys.stderr)
        sys.exit(1)


def validate_schema(registry: dict, schema: dict) -> list[str]:
    try:
        import jsonschema
    except ImportError:
        print(
            "WARNING: jsonschema not installed — skipping schema validation.\n"
            "  Install with: pip install jsonschema",
            file=sys.stderr,
        )
        return []

    errors = []
    validator = jsonschema.Draft7Validator(schema)
    for error in sorted(validator.iter_errors(registry), key=lambda e: list(e.path)):
        path = ".".join(str(p) for p in error.absolute_path) or "(root)"
        errors.append(f"  {path}: {error.message}")
    return errors


def validate_structural(registry: dict) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []

    if registry.get("registry_version") != 2:
        errors.append("registry_version must be 2")

    packages = registry.get("packages", {})
    for slug, pkg in packages.items():
        if not SLUG_PATTERN.match(slug):
            errors.append(f"  {slug}: invalid slug (must match [a-z0-9][a-z0-9-]*)")

        ver = pkg.get("verification", {})
        if ver.get("verified_version") != pkg.get("version"):
            warnings.append(
                f"  {slug}: verification.verified_version ({ver.get('verified_version')}) "
                f"!= version ({pkg.get('version')})"
            )

        platforms = pkg.get("platforms", {})
        if not platforms:
            errors.append(f"  {slug}: must have at least one platform")

        for key in ver.get("build_status", {}):
            if not BUILD_STATUS_KEY_PATTERN.match(key):
                warnings.append(f"  {slug}: build_status key '{key}' should be Platform-arch format")

    return errors, warnings


def validate_licenses(registry: dict) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []

    for slug, pkg in registry.get("packages", {}).items():
        lic = pkg.get("license", "")
        if lic in REJECTED_LICENSES:
            errors.append(f"  {slug}: license '{lic}' is incompatible with Pulp's MIT license")
        elif lic not in ALLOWED_LICENSES:
            warnings.append(f"  {slug}: license '{lic}' not in known-allowed list — review required")

    return errors, warnings


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate the Pulp package registry")
    parser.add_argument("--strict", action="store_true", help="Exit non-zero on warnings")
    parser.add_argument("--check-licenses", action="store_true", help="Verify license compatibility")
    args = parser.parse_args()

    registry = load_json(REGISTRY)
    schema = load_json(SCHEMA)

    packages = registry.get("packages", {})
    pkg_count = len(packages)
    version = registry.get("registry_version", "?")
    print(f"Validating registry.json ({pkg_count} packages, registry v{version})...")

    all_errors: list[str] = []
    all_warnings: list[str] = []

    schema_errors = validate_schema(registry, schema)
    if schema_errors:
        print("\nSchema validation errors:")
        for e in schema_errors:
            print(e)
        all_errors.extend(schema_errors)

    struct_errors, struct_warnings = validate_structural(registry)
    all_errors.extend(struct_errors)
    all_warnings.extend(struct_warnings)

    if args.check_licenses:
        lic_errors, lic_warnings = validate_licenses(registry)
        all_errors.extend(lic_errors)
        all_warnings.extend(lic_warnings)

    for slug, pkg in packages.items():
        overlaps = len(pkg.get("overlaps_with_builtin", {}))
        note = f" ({overlaps} built-in overlaps noted)" if overlaps else ""
        status = "FAIL" if any(slug in e for e in all_errors) else "OK"
        print(f"  {slug} {'.' * max(1, 30 - len(slug))} {status}{note}")

    if struct_errors:
        print("\nStructural errors:")
        for e in struct_errors:
            print(e)

    if struct_warnings:
        print("\nWarnings:")
        for w in struct_warnings:
            print(w)

    if args.check_licenses:
        if lic_errors:
            print("\nLicense errors:")
            for e in lic_errors:
                print(e)
        if lic_warnings:
            print("\nLicense warnings:")
            for w in lic_warnings:
                print(w)

    error_count = len(all_errors)
    warning_count = len(all_warnings)
    print(f"\n{pkg_count} packages validated, {error_count} errors, {warning_count} warnings.")

    if error_count > 0:
        return 1
    if args.strict and warning_count > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
