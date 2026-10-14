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
TOOL_REGISTRY = ROOT / "tools" / "packages" / "tool-registry.json"

SLUG_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]*$")

ALLOWED_LICENSES = {
    "0BSD", "MIT", "MIT-0", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0",
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
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")

# Tools whose binary download is verified by a code-owned Rust fetcher. Their
# registry digests mirror that code and must not drift from it.
CODE_OWNED_DIGESTS = {
    "trace-processor": ROOT / "experimental" / "pulp-rs" / "src" / "cmd" / "trace_fetch.rs",
    "chrome-for-testing": ROOT / "experimental" / "pulp-rs" / "src" / "cmd" / "chrome_for_testing.rs",
}

# `fetch.git_tag` must name an immutable pin: a release tag or a full 40-hex
# commit. `pulp add` resolves it to a full commit and records an archive digest
# in packages.lock.json, so a moving branch would make the lock disagree with
# the registry the moment upstream moves, and an abbreviated SHA cannot be
# resolved by a shallow fetch and is ambiguous as the repository grows.
FULL_COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
# Mixed hex of any length below 40 is an abbreviated commit. An all-digit
# string is let through because date- and number-style release tags
# ("20240101") are common and indistinguishable from a digit-only prefix.
SHORT_COMMIT_PATTERN = re.compile(r"^(?=[0-9a-fA-F]*[a-fA-F])[0-9a-fA-F]{4,39}$")
BRANCH_NAMES = {"master", "main", "develop", "dev", "trunk", "head", "gh-pages", "latest"}


def git_tag_pin_error(git_tag: object) -> str | None:
    """Return why ``git_tag`` is not an immutable pin, or None when it is."""
    if not isinstance(git_tag, str) or not git_tag.strip():
        return "fetch.git_tag must be a non-empty release tag or full 40-hex commit"
    tag = git_tag.strip()
    if tag != git_tag:
        return f"fetch.git_tag '{git_tag}' has surrounding whitespace"
    if FULL_COMMIT_PATTERN.match(tag):
        return None
    if tag.lower() in BRANCH_NAMES or tag.startswith("refs/heads/"):
        return (f"fetch.git_tag '{tag}' is a branch name; pin a release tag or a "
                "full 40-hex commit instead")
    if SHORT_COMMIT_PATTERN.match(tag):
        return (f"fetch.git_tag '{tag}' is an abbreviated commit; use the full "
                "40-hex commit")
    return None


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

        fetch = pkg.get("fetch")
        if isinstance(fetch, dict):
            pin_error = git_tag_pin_error(fetch.get("git_tag"))
            if pin_error:
                errors.append(f"  {slug}: {pin_error}")

        platforms = pkg.get("platforms", {})
        if not platforms:
            errors.append(f"  {slug}: must have at least one platform")

        for key in ver.get("build_status", {}):
            if not BUILD_STATUS_KEY_PATTERN.match(key):
                warnings.append(f"  {slug}: build_status key '{key}' should be Platform-arch format")

        # Mobile platform coverage check
        has_android = "Android" in platforms
        has_ios = "iOS" in platforms
        if not has_android and not has_ios:
            warnings.append(f"  {slug}: no Android or iOS platform entries — consider adding mobile support")

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


def validate_tool_registry(tool_registry: dict) -> list[str]:
    """Enforce the opt-in-tool update/override contract.

    Every tool Pulp manages (``managed_by_pulp: true``) must be updatable and
    version-overridable by the user without waiting for a registry bump. The
    ``pulp tool update`` command and the user override (``--version`` /
    ``PULP_TOOL_<ID>_VERSION`` / ``$PULP_HOME/tool-overrides.json``) both key
    off the tool's ``pinned_version`` anchor, so a managed tool that ships
    without a non-empty ``pinned_version`` has no version to update from or
    override — exactly the "tool users can't update" case this rule forbids.
    """
    errors: list[str] = []
    for tool_id, tool in tool_registry.get("tools", {}).items():
        errors.extend(validate_binary_source_digests(tool_id, tool))
        errors.extend(validate_bundleable_license(tool_id, tool))
        if not tool.get("managed_by_pulp"):
            continue
        pinned = tool.get("pinned_version", "")
        if not isinstance(pinned, str) or not pinned.strip():
            errors.append(
                f"  {tool_id}: managed_by_pulp tool must declare a non-empty "
                "'pinned_version' — it is the anchor for `pulp tool update` and "
                "user version overrides. A managed tool with no update/override "
                "path must not ship."
            )
    return errors


def validate_binary_source_digests(tool_id: str, tool: dict) -> list[str]:
    """Every downloadable binary source must carry the sha256 of its asset.

    `pulp tool install` refuses a registry-pinned download whose digest does
    not match, so a source without one is a download nobody can verify. A
    user override to another version is the only unverified path, and it is
    recorded as such at install time, never here.
    """
    errors: list[str] = []
    for platform, source in (tool.get("binary_sources") or {}).items():
        digest = source.get("sha256") if isinstance(source, dict) else None
        if not isinstance(digest, str) or not SHA256_PATTERN.match(digest):
            errors.append(
                f"  {tool_id}: binary_sources.{platform} must declare 'sha256' as 64 "
                "lowercase hex characters (the digest of the published release asset "
                "at pinned_version). Download the asset into an empty directory, "
                "compare it with the upstream checksum when one is published, and "
                "record it."
            )
    return errors


def validate_code_owned_digests(tool_registry: dict) -> list[str]:
    """Registry digests for code-verified tools must match the owning code."""
    errors: list[str] = []
    for tool_id, owner in CODE_OWNED_DIGESTS.items():
        tool = tool_registry.get("tools", {}).get(tool_id)
        if not tool or not owner.exists():
            continue
        code = owner.read_text(encoding="utf-8")
        for platform, source in (tool.get("binary_sources") or {}).items():
            digest = source.get("sha256", "") if isinstance(source, dict) else ""
            if SHA256_PATTERN.match(digest or "") and f'"{digest}"' not in code:
                errors.append(
                    f"  {tool_id}: binary_sources.{platform}.sha256 {digest} does not "
                    f"appear in {owner.relative_to(ROOT)}, the fetcher that verifies "
                    "this tool. Update both together."
                )
    return errors


class _UnsupportedLicenseExpression(ValueError):
    """The expression is not one this evaluator can judge exactly."""


def _license_tokens(expression: str) -> list[str]:
    return re.findall(r"\(|\)|[^\s()]+", expression)


def license_permits_bundling(expression: object) -> bool:
    """True when an SPDX expression can only resolve to allowed licenses.

    Evaluated with SPDX precedence: parentheses first, then AND, which binds
    tighter than OR. `A OR B` is permitted when either branch is (the
    distributor picks it); `A AND B` only when both are. A `WITH` exception,
    an unknown, copyleft or prose license, an empty value, or any expression
    that does not parse cleanly is not bundleable: the evaluator fails closed.
    """
    if not isinstance(expression, str) or not expression.strip():
        return False
    tokens = _license_tokens(expression)
    pos = 0

    def peek() -> str | None:
        return tokens[pos] if pos < len(tokens) else None

    def take() -> str:
        nonlocal pos
        if pos >= len(tokens):
            raise _UnsupportedLicenseExpression("unexpected end")
        pos += 1
        return tokens[pos - 1]

    def parse_or() -> bool:
        result = parse_and()
        while peek() == "OR":
            take()
            right = parse_and()
            result = result or right
        return result

    def parse_and() -> bool:
        result = parse_atom()
        while peek() == "AND":
            take()
            right = parse_atom()
            result = result and right
        return result

    def parse_atom() -> bool:
        token = take()
        if token == "(":
            inner = parse_or()
            if take() != ")":
                raise _UnsupportedLicenseExpression("unbalanced parenthesis")
            return inner
        if token in {")", "AND", "OR", "WITH"}:
            raise _UnsupportedLicenseExpression(f"unexpected {token}")
        return token in ALLOWED_LICENSES

    try:
        permitted = parse_or()
        if pos != len(tokens):
            return False
        return permitted
    except _UnsupportedLicenseExpression:
        return False


def validate_bundleable_license(tool_id: str, tool: dict) -> list[str]:
    """A tool may claim `bundleable: true` only under a permissive license.

    Pulp never ships GPL, AGPL or LGPL code in its artifacts, and an unknown
    license is not a reviewed one, so `bundleable` is refused for all of them.
    """
    if tool.get("bundleable") is not True or license_permits_bundling(tool.get("license")):
        return []
    return [
        f"  {tool_id}: 'bundleable: true' with license {tool.get('license')!r}, which is "
        "copyleft, unknown or not in the allowed list "
        f"({', '.join(sorted(ALLOWED_LICENSES))}). Pulp's artifacts may not carry "
        "GPL/AGPL/LGPL code. Set 'bundleable': false, or pin a build whose license "
        "is allowed and record it in 'license'."
    ]


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

    tool_errors: list[str] = []
    if TOOL_REGISTRY.exists():
        tool_registry = load_json(TOOL_REGISTRY)
        tool_count = len(tool_registry.get("tools", {}))
        print(
            f"Validating tool-registry.json ({tool_count} tools) — "
            "managed tools must declare an update/override path, binary sources a sha256..."
        )
        tool_errors = validate_tool_registry(tool_registry)
        tool_errors.extend(validate_code_owned_digests(tool_registry))
        all_errors.extend(tool_errors)

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

    if tool_errors:
        print("\nTool-registry update/override errors:")
        for e in tool_errors:
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
