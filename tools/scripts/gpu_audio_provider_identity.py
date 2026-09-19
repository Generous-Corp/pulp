#!/usr/bin/env python3
"""Validate the exact Skia/Dawn provider used by the GPU-audio proof."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

import fetch_skia_for_release as skia_fetch


SCHEMA = "pulp.gpu-audio-provider-identity.v1"
SHA256_RE = re.compile(r"[0-9a-f]{64}")
SHA1_RE = re.compile(r"[0-9a-f]{40}")


class IdentityError(ValueError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise IdentityError(f"invalid_{label}") from error
    if not isinstance(value, dict):
        raise IdentityError(f"invalid_{label}")
    return value


def require_sha(value: Any, pattern: re.Pattern[str], reason: str) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise IdentityError(reason)
    return value


def skia_contract(manifest: dict[str, Any], platform: str) -> tuple[str, str]:
    dependencies = manifest.get("dependencies")
    if not isinstance(dependencies, list):
        raise IdentityError("invalid_dependency_manifest")
    skia = next(
        (item for item in dependencies if isinstance(item, dict) and item.get("name") == "Skia"),
        None,
    )
    if skia is None:
        raise IdentityError("skia_dependency_missing")
    determinism = skia.get("determinism")
    if not isinstance(determinism, dict):
        raise IdentityError("skia_determinism_missing")
    expected_dawn = require_sha(
        determinism.get("built_dawn"), SHA1_RE, "invalid_expected_dawn_sha"
    )
    manifest_key = skia_fetch.MATRIX_TO_MANIFEST.get(platform)
    if manifest_key is None:
        raise IdentityError("unsupported_provider_platform")
    assets = determinism.get("release_assets")
    if not isinstance(assets, dict):
        raise IdentityError("skia_release_assets_missing")
    asset = assets.get(manifest_key)
    if not isinstance(asset, dict):
        raise IdentityError("provider_platform_asset_missing")
    expected_asset = require_sha(
        asset.get("sha256"), SHA256_RE, "invalid_expected_asset_sha256"
    )
    return expected_dawn, expected_asset


def parse_dawn_header(path: Path) -> str:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise IdentityError("dawn_header_unreadable") from error
    match = re.search(r"kDawnVersion\s*=\s*\{(?P<body>[^}]*)\}", text, re.DOTALL)
    if match is None:
        raise IdentityError("dawn_header_version_missing")
    values = re.findall(r"0x([0-9a-fA-F]{2})", match.group("body"))
    if len(values) != 20:
        raise IdentityError("dawn_header_version_invalid")
    return "".join(values).lower()


def generation_files(generation: dict[str, Any]) -> dict[str, dict[str, Any]]:
    files = generation.get("files")
    if not isinstance(files, list):
        raise IdentityError("generation_files_missing")
    result: dict[str, dict[str, Any]] = {}
    for item in files:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str):
            raise IdentityError("generation_file_invalid")
        if item["path"] in result:
            raise IdentityError("generation_file_duplicate")
        result[item["path"]] = item
    return result


def validate(args: argparse.Namespace) -> dict[str, Any]:
    manifest_path = Path(args.manifest).resolve(strict=True)
    skia_dir = Path(args.skia_dir).resolve(strict=True)
    dawn_header = Path(args.dawn_header).resolve(strict=True)
    dawn_library = Path(args.dawn_library).resolve(strict=True)
    expected_dawn, expected_asset = skia_contract(
        load_json(manifest_path, "dependency_manifest"), args.platform
    )

    stamp_path = skia_dir / ".skia-asset-sha256"
    try:
        asset_sha = stamp_path.read_text(encoding="ascii").strip()
    except (OSError, UnicodeError) as error:
        raise IdentityError("provider_asset_stamp_unreadable") from error
    require_sha(asset_sha, SHA256_RE, "provider_asset_stamp_invalid")
    if asset_sha != expected_asset:
        raise IdentityError("provider_asset_not_pinned")

    # This is the canonical archive/receipt/extracted-tree validator. It checks
    # the retained ZIP, every authenticated generation entry, the stamp, and
    # the platform's required Skia/Dawn archives. A matching stamp alone is not
    # provider identity.
    if not skia_fetch.cache_generation_valid(skia_dir, args.platform, expected_asset):
        raise IdentityError("provider_generation_invalid")

    generation_path = skia_dir / ".skia-generation-manifest.json"
    generation = load_json(generation_path, "generation_manifest")
    if generation.get("asset_sha256") != asset_sha:
        raise IdentityError("generation_asset_mismatch")
    files = generation_files(generation)

    try:
        header_relative = dawn_header.relative_to(skia_dir).as_posix()
        library_relative = dawn_library.relative_to(skia_dir).as_posix()
    except ValueError as error:
        raise IdentityError("provider_file_outside_root") from error

    observed_hashes: dict[str, str] = {}
    for label, path, relative in (
        ("dawn_header", dawn_header, header_relative),
        ("dawn_library", dawn_library, library_relative),
    ):
        record = files.get(relative)
        if record is None:
            raise IdentityError(f"{label}_missing_from_generation")
        recorded_sha = require_sha(
            record.get("sha256"), SHA256_RE, f"{label}_generation_sha_invalid"
        )
        observed_sha = sha256_file(path)
        observed_hashes[label] = observed_sha
        if observed_sha != recorded_sha:
            raise IdentityError(f"{label}_generation_sha_mismatch")
        if record.get("size") != path.stat().st_size:
            raise IdentityError(f"{label}_generation_size_mismatch")

    header_dawn = parse_dawn_header(dawn_header)
    if header_dawn != expected_dawn:
        raise IdentityError("dawn_header_manifest_mismatch")

    return {
        "schema": SCHEMA,
        "status": "passed",
        "reason": "ok",
        "provider_identity_status": "passed",
        "expected_asset_sha256": expected_asset,
        "validated_asset_sha256": asset_sha,
        "expected_dawn_revision": expected_dawn,
        "header_dawn_revision": header_dawn,
        "dawn_header_sha256": observed_hashes["dawn_header"],
        "linked_dawn_archive_sha256": observed_hashes["dawn_library"],
        "manifest_sha256": sha256_file(manifest_path),
        "generation_receipt_sha256": sha256_file(generation_path),
    }


def write_receipt(receipt: dict[str, Any], path: str | None) -> None:
    encoded = json.dumps(receipt, sort_keys=True, separators=(",", ":")) + "\n"
    if path:
        Path(path).write_text(encoded, encoding="utf-8")
    sys.stdout.write(encoded)


def write_cmake(receipt: dict[str, Any], path: str | None) -> None:
    if not path or receipt.get("status") != "passed":
        return
    content = (
        f'set(PULP_GPU_AUDIO_EXPECTED_DAWN_SHA "{receipt["expected_dawn_revision"]}")\n'
        f'set(PULP_GPU_AUDIO_EXPECTED_ASSET_SHA256 "{receipt["expected_asset_sha256"]}")\n'
        f'set(PULP_GPU_AUDIO_DAWN_ARCHIVE_SHA256 "{receipt["linked_dawn_archive_sha256"]}")\n'
    )
    Path(path).write_text(content, encoding="utf-8")


def identity_keys() -> tuple[str, ...]:
    return (
        "expected_asset_sha256",
        "validated_asset_sha256",
        "expected_dawn_revision",
        "header_dawn_revision",
        "dawn_header_sha256",
        "linked_dawn_archive_sha256",
        "manifest_sha256",
        "generation_receipt_sha256",
    )


def load_passed_receipt(path: Path, label: str) -> dict[str, Any]:
    receipt = load_json(path.resolve(strict=True), label)
    if (
        receipt.get("schema") != SCHEMA
        or receipt.get("status") != "passed"
        or receipt.get("provider_identity_status") != "passed"
    ):
        raise IdentityError(f"invalid_{label}")
    return receipt


def require_same_identity(
    expected: dict[str, Any], observed: dict[str, Any], reason: str
) -> None:
    if any(expected.get(key) != observed.get(key) for key in identity_keys()):
        raise IdentityError(reason)


def prelink(args: argparse.Namespace) -> dict[str, Any]:
    configured_path = Path(args.configured_receipt)
    configured = load_passed_receipt(configured_path, "configured_receipt")
    current = validate(args)
    require_same_identity(configured, current, "configured_provider_identity_stale")
    current["configured_receipt_sha256"] = sha256_file(configured_path.resolve(strict=True))
    return current


def bind(args: argparse.Namespace) -> dict[str, Any]:
    configured_path = Path(args.configured_receipt)
    prelink_path = Path(args.prelink_receipt)
    configured = load_passed_receipt(configured_path, "configured_receipt")
    linked = load_passed_receipt(prelink_path, "prelink_receipt")
    receipt = validate(args)
    require_same_identity(configured, linked, "prelink_provider_identity_stale")
    require_same_identity(configured, receipt, "configured_provider_identity_stale")
    expected_configured_sha = sha256_file(configured_path.resolve(strict=True))
    if linked.get("configured_receipt_sha256") != expected_configured_sha:
        raise IdentityError("prelink_provider_identity_stale")
    receipt["configured_receipt_sha256"] = expected_configured_sha
    receipt["prelink_receipt_sha256"] = sha256_file(prelink_path.resolve(strict=True))
    executable = Path(args.executable).resolve(strict=True)
    receipt["executable_sha256"] = sha256_file(executable)
    return receipt


def verify(args: argparse.Namespace) -> dict[str, Any]:
    bound = load_passed_receipt(Path(args.receipt), "bound_receipt")
    current = bind(args)
    for key in (
        *identity_keys(),
        "configured_receipt_sha256",
        "prelink_receipt_sha256",
        "executable_sha256",
    ):
        if bound.get(key) != current.get(key):
            raise IdentityError("bound_provider_identity_stale")
    return current


def add_identity_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--skia-dir", required=True)
    parser.add_argument("--dawn-header", required=True)
    parser.add_argument("--dawn-library", required=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_parser = subparsers.add_parser("validate")
    add_identity_arguments(validate_parser)
    validate_parser.add_argument("--result")
    validate_parser.add_argument("--cmake-output")
    prelink_parser = subparsers.add_parser("prelink")
    add_identity_arguments(prelink_parser)
    prelink_parser.add_argument("--configured-receipt", required=True)
    prelink_parser.add_argument("--result", required=True)
    bind_parser = subparsers.add_parser("bind")
    add_identity_arguments(bind_parser)
    bind_parser.add_argument("--configured-receipt", required=True)
    bind_parser.add_argument("--prelink-receipt", required=True)
    bind_parser.add_argument("--executable", required=True)
    bind_parser.add_argument("--result", required=True)
    verify_parser = subparsers.add_parser("verify")
    add_identity_arguments(verify_parser)
    verify_parser.add_argument("--configured-receipt", required=True)
    verify_parser.add_argument("--prelink-receipt", required=True)
    verify_parser.add_argument("--executable", required=True)
    verify_parser.add_argument("--receipt", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "validate":
            receipt = validate(args)
        elif args.command == "prelink":
            receipt = prelink(args)
        elif args.command == "bind":
            receipt = bind(args)
        else:
            receipt = verify(args)
    except (IdentityError, FileNotFoundError) as error:
        reason = str(error) if isinstance(error, IdentityError) else "provider_path_missing"
        receipt = {
            "schema": SCHEMA,
            "status": "failed",
            "reason": reason,
            "provider_identity_status": "failed",
        }
        write_receipt(receipt, getattr(args, "result", None))
        return 1
    write_receipt(receipt, getattr(args, "result", None))
    write_cmake(receipt, getattr(args, "cmake_output", None))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
