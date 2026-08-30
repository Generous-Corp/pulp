#!/usr/bin/env python3
"""Validate the exact Forge Modular inputs to a release installer."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from binary_identity import content_sha256


HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")
DATE = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}")
PRODUCT = "Forge Modular"
FORMATS = {
    "au": "Audio Unit",
    "vst3": "VST3",
    "clap": "CLAP",
    "standalone": "Standalone application",
}
TOOLCHAIN_PATHS = (
    "tools/rack",
    "tools/dsp_vocabulary.py",
    "docs/status/agent-capabilities.json",
    "external/fonts",
    "examples/forge-modular",
    "core",
    "build/shape_text",
)
SIGNED_TOOL_NAMES = {"rack_patch_decode", "shape_text"}


class ValidationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise ValidationError(message)


def git(root: Path, *args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(root), *args], stderr=subprocess.PIPE, text=True
        ).strip()
    except subprocess.CalledProcessError as error:
        fail(error.stderr.strip() or f"git {' '.join(args)} failed in {root}")


def parse_fields(path: Path) -> dict[str, str]:
    if not path.is_file():
        fail(f"missing build provenance: {path}")
    fields: dict[str, str] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if "=" not in raw:
            fail(f"malformed build provenance at {path}:{number}")
        key, value = raw.split("=", 1)
        if not key or key in fields:
            fail(f"duplicate or empty build provenance key at {path}:{number}")
        fields[key] = value
    return fields


def parse_cache(path: Path) -> dict[str, str]:
    if not path.is_file():
        fail(f"Forge build cache is missing: {path}")
    values: dict[str, str] = {}
    counts: dict[str, int] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"([^:#=]+):[^=]+=(.*)$", raw)
        if not match:
            continue
        key, value = match.groups()
        counts[key] = counts.get(key, 0) + 1
        values[key] = value
    for key in (
        "CMAKE_HOME_DIRECTORY",
        "CMAKE_BUILD_TYPE",
        "FORGE_TARGET_PLATFORM",
        "PULP_SDK_PLATFORM",
        "FORGE_MODULAR_REQUIRE_TOOLCHAIN",
    ):
        if counts.get(key) != 1:
            fail(f"Forge build cache must contain exactly one {key} entry")
    return values


def forge_source_snapshot(forge_root: Path, forge_ref: str) -> str:
    tool = forge_root / "tools" / "forge_source_snapshot.py"
    if not tool.is_file():
        fail(f"Forge canonical source snapshot tool is missing: {tool}")
    try:
        output = subprocess.check_output(
            [sys.executable, str(tool), "--root", str(forge_root)],
            stderr=subprocess.PIPE, text=True,
        )
    except subprocess.CalledProcessError as error:
        fail(error.stderr.strip() or "Forge canonical source snapshot failed")
    fields: dict[str, str] = {}
    for raw in output.splitlines():
        if "=" not in raw:
            fail("Forge canonical source snapshot output is malformed")
        key, value = raw.split("=", 1)
        if not key or key in fields:
            fail("Forge canonical source snapshot output has duplicate or empty keys")
        fields[key] = value
    if fields.get("source_git_head") != forge_ref:
        fail("Forge canonical source snapshot does not match --forge-ref")
    if fields.get("source_git_dirty") != "false":
        fail("Forge canonical source snapshot reports a dirty checkout")
    snapshot = fields.get("source_snapshot_sha256", "")
    if not HEX64.fullmatch(snapshot):
        fail("Forge canonical source snapshot is not an exact SHA-256")
    return snapshot


def digest_tree(resources: Path) -> str:
    digest = hashlib.sha256()
    count = 0
    for relative in TOOLCHAIN_PATHS:
        candidate = resources / relative
        if not candidate.exists():
            fail(f"Forge Modular toolchain is missing {relative}: {resources}")
        paths = [candidate]
        if candidate.is_dir():
            paths = sorted(path for path in candidate.rglob("*") if path.is_file())
        for path in paths:
            rel = path.relative_to(resources).as_posix().encode()
            digest.update(len(rel).to_bytes(8, "big"))
            digest.update(rel)
            if path.name in SIGNED_TOOL_NAMES:
                data = bytes.fromhex(content_sha256(str(path)))
            else:
                data = path.read_bytes()
            digest.update(len(data).to_bytes(8, "big"))
            digest.update(data)
            count += 1
    if count == 0:
        fail(f"Forge Modular toolchain contains no files: {resources}")
    return digest.hexdigest()


def validate_bundle(
    bundle: Path,
    kind: str,
    version: str,
    forge_ref: str,
    pulp_ref: str,
    target_platform: str,
) -> dict[str, str]:
    if not bundle.is_dir():
        fail(f"missing {kind} artifact: {bundle}")
    resources = bundle / "Contents" / "Resources"
    fields = parse_fields(resources / "FORGE_BUILD_INFO")
    expected = {
        "schema": "1",
        "version": version,
        "product": PRODUCT,
        "role": "Rack module and patch generator",
        "format": FORMATS[kind],
        "build": f"Release · {target_platform}",
        "source_git_head": forge_ref,
        "source_git_dirty": "false",
    }
    for key, value in expected.items():
        if fields.get(key) != value:
            fail(f"{bundle}: {key}={fields.get(key)!r}, expected {value!r}")
    if fields.get("product_id") != "com.generous.forge.modular":
        fail(f"{bundle}: product_id is not the Forge Modular identity")
    if fields.get("pulp_sdk", "").split(" · ")[-1] != pulp_ref:
        fail(f"{bundle}: pulp_sdk does not name exact PULP_SDK_REF {pulp_ref}")
    if fields.get("expected_pulp_sdk_ref", pulp_ref) != pulp_ref:
        fail(f"{bundle}: expected_pulp_sdk_ref does not match {pulp_ref}")
    if not HEX64.fullmatch(fields.get("source_snapshot_sha256", "")):
        fail(f"{bundle}: source_snapshot_sha256 is not an exact SHA-256")

    stamp = resources / "tools" / "rack" / "FORGE_TOOLCHAIN_STAMP"
    if not stamp.is_file():
        fail(f"Forge Modular toolchain stamp is missing: {bundle}")
    lines = stamp.read_text(encoding="utf-8").splitlines()
    if len(lines) != 3 or lines[0] != version or not DATE.fullmatch(lines[1]) \
            or lines[2] != f"pulp {pulp_ref}":
        fail(f"Forge Modular toolchain stamp is not exact: {stamp}")
    shape_text = resources / "build" / "shape_text"
    if not os.access(shape_text, os.X_OK):
        fail(f"Forge Modular shape_text is not executable: {shape_text}")
    rack_decoder = resources / "tools" / "rack" / "rack_patch_decode"
    if not os.access(rack_decoder, os.X_OK):
        fail(f"Forge Modular Rack saved-patch decoder is not executable: {rack_decoder}")
    return {
        "bundle": str(bundle.resolve()),
        "source_snapshot_sha256": fields["source_snapshot_sha256"],
        "toolchain_sha256": digest_tree(resources),
        "shape_text": str(shape_text.resolve()),
        "rack_patch_decode": str(rack_decoder.resolve()),
    }


def validate_bundles(
    bundles: dict[str, Path],
    version: str,
    forge_ref: str,
    pulp_ref: str,
    architecture: str,
    expected_snapshot: str,
) -> dict[str, object]:
    if architecture not in ("arm64", "x86_64"):
        fail(f"unsupported release architecture: {architecture}")
    target_platform = "darwin-arm64" if architecture == "arm64" else "darwin-x64"
    records = {
        kind: validate_bundle(path, kind, version, forge_ref, pulp_ref, target_platform)
        for kind, path in bundles.items()
    }
    snapshots = {record["source_snapshot_sha256"] for record in records.values()}
    toolchains = {record["toolchain_sha256"] for record in records.values()}
    if len(snapshots) != 1:
        fail("Forge Modular bundles do not share one exact Forge source snapshot")
    if snapshots != {expected_snapshot}:
        fail("Forge Modular bundles do not match the canonical Forge source snapshot")
    if len(toolchains) != 1:
        fail("Forge Modular bundles do not share one exact bundled toolchain")
    return {
        "schema": "forge.modular-release-inputs.v1",
        "forge_ref": forge_ref,
        "pulp_ref": pulp_ref,
        "version": version,
        "architecture": architecture,
        "source_snapshot_sha256": next(iter(snapshots)),
        "toolchain_sha256": next(iter(toolchains)),
        "bundles": records,
    }


def source_bundles(build: Path) -> dict[str, Path]:
    return {
        "au": build / "AU" / "Forge Modular.component",
        "vst3": build / "VST3" / "Forge Modular.vst3",
        "clap": build / "CLAP" / "Forge Modular.clap",
        "standalone": build / "modular" / "Forge Modular.app",
    }


def installed_bundles(expanded: Path) -> dict[str, Path]:
    names = {
        "au": "Forge Modular.component",
        "vst3": "Forge Modular.vst3",
        "clap": "Forge Modular.clap",
        "standalone": "Forge Modular.app",
    }
    top_level: list[Path] = []
    all_bundles: list[Path] = []
    suffixes = (".app", ".component", ".vst3", ".clap")
    install_prefixes = {
        ("Applications",),
        ("Library", "Audio", "Plug-Ins", "Components"),
        ("Library", "Audio", "Plug-Ins", "VST3"),
        ("Library", "Audio", "Plug-Ins", "CLAP"),
    }
    for payload in expanded.rglob("Payload"):
        if not payload.is_dir():
            continue
        for path in payload.rglob("*"):
            if not path.is_dir() or not path.name.endswith(suffixes):
                continue
            all_bundles.append(path)
            relative = path.relative_to(payload)
            if len(relative.parts) == 1 or relative.parts[:-1] in install_prefixes:
                top_level.append(path)

    found: dict[str, Path] = {}
    for kind, name in names.items():
        matches = sorted(path for path in top_level if path.name == name)
        if len(matches) != 1:
            fail(f"expanded package has {len(matches)} {kind} Modular payloads, expected one")
        found[kind] = matches[0]
    expected = {path.resolve() for path in found.values()}
    extras = sorted(str(path) for path in {path.resolve() for path in all_bundles} - expected)
    if extras:
        fail("expanded package contains non-Modular product artifacts: " + ", ".join(extras))
    return found


def validate_source(args: argparse.Namespace) -> dict[str, object]:
    forge_root = args.forge_root.resolve(strict=True)
    if not HEX40.fullmatch(args.forge_ref):
        fail("--forge-ref must be a full lowercase 40-character Git SHA")
    top = Path(git(forge_root, "rev-parse", "--show-toplevel")).resolve()
    if top != forge_root:
        fail("--forge-root must be the Forge checkout root")
    if git(forge_root, "rev-parse", "HEAD") != args.forge_ref:
        fail("Forge checkout HEAD does not match --forge-ref")
    if git(forge_root, "status", "--porcelain", "--untracked-files=all"):
        fail("Forge checkout must be completely clean")
    symbolic = subprocess.run(
        ["git", "-C", str(forge_root), "symbolic-ref", "-q", "HEAD"],
        stdout=subprocess.DEVNULL,
    )
    if symbolic.returncode == 0:
        fail("Forge checkout must be detached at --forge-ref")

    cache = parse_cache(args.build_dir / "CMakeCache.txt")
    if Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() != forge_root:
        fail("Forge build was not configured from --forge-root")
    if cache["CMAKE_BUILD_TYPE"] != "Release":
        fail("Forge Modular release requires a Release build")
    expected_platform = "darwin-arm64" if args.architecture == "arm64" else "darwin-x64"
    if cache["FORGE_TARGET_PLATFORM"] != expected_platform \
            or cache["PULP_SDK_PLATFORM"] != expected_platform:
        fail(f"Forge build does not target {expected_platform}")
    if cache["FORGE_MODULAR_REQUIRE_TOOLCHAIN"] != "ON":
        fail("Forge build did not require the bundled Modular toolchain")
    expected_snapshot = forge_source_snapshot(forge_root, args.forge_ref)
    return validate_bundles(
        source_bundles(args.build_dir), args.version, args.forge_ref,
        args.pulp_ref, args.architecture, expected_snapshot,
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    sub = result.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--version", required=True)
    common.add_argument("--forge-ref", required=True)
    common.add_argument("--pulp-ref", required=True)
    common.add_argument("--architecture", required=True)
    source = sub.add_parser("source", parents=[common])
    source.add_argument("--forge-root", type=Path, required=True)
    source.add_argument("--build-dir", type=Path, required=True)
    package = sub.add_parser("package", parents=[common])
    package.add_argument("--expanded-root", type=Path, required=True)
    package.add_argument("--source-snapshot", required=True)
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if not HEX40.fullmatch(args.pulp_ref):
        print("error: --pulp-ref must be a full lowercase 40-character Git SHA", file=sys.stderr)
        return 2
    try:
        if args.command == "source":
            report = validate_source(args)
        else:
            if not HEX64.fullmatch(args.source_snapshot):
                fail("--source-snapshot must be an exact lowercase SHA-256")
            report = validate_bundles(
                installed_bundles(args.expanded_root), args.version,
                args.forge_ref, args.pulp_ref, args.architecture,
                args.source_snapshot,
            )
    except (OSError, ValidationError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
