#!/usr/bin/env python3
"""Generate and verify SHA256SUMS manifests for release assets."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path


DEFAULT_EXCLUDES = {"SHA256SUMS", "checksums.sha256"}
MANIFEST_LINE_RE = re.compile(r"^([0-9a-f]{64}) [ *](.+)$")


class ManifestError(RuntimeError):
    pass


def sha256_hex(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_asset_name(name: str) -> None:
    if not name or name in {".", ".."}:
        raise ManifestError(f"invalid asset name: {name!r}")
    if "/" in name or "\\" in name:
        raise ManifestError(f"asset name must be a basename: {name!r}")
    if "\n" in name or "\r" in name:
        raise ManifestError(f"asset name contains a newline: {name!r}")


def asset_files(asset_dir: Path, excludes: set[str]) -> list[Path]:
    if not asset_dir.is_dir():
        raise ManifestError(f"asset directory does not exist: {asset_dir}")
    files = []
    for path in asset_dir.iterdir():
        if not path.is_file():
            continue
        validate_asset_name(path.name)
        if path.name in excludes:
            continue
        files.append(path)
    return sorted(files, key=lambda path: path.name)


def required_names(values: list[str]) -> set[str]:
    names = set(values)
    for name in names:
        validate_asset_name(name)
    return names


def generate_manifest(
    asset_dir: Path,
    output: Path,
    required: set[str],
    *,
    exact_required: bool,
    excludes: set[str],
) -> list[str]:
    output_name = output.name
    validate_asset_name(output_name)
    excludes = set(excludes)
    excludes.add(output_name)

    files = asset_files(asset_dir, excludes)
    present = {path.name for path in files}

    missing = sorted(required - present)
    if missing:
        raise ManifestError("missing required release asset(s): " + ", ".join(missing))

    if exact_required:
        unexpected = sorted(present - required)
        if unexpected:
            raise ManifestError("unexpected release asset(s): " + ", ".join(unexpected))

    lines = [f"{sha256_hex(path)}  {path.name}" for path in files]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return lines


def parse_manifest_line(line: str, line_no: int) -> tuple[str, str]:
    match = MANIFEST_LINE_RE.match(line.rstrip("\n"))
    if not match:
        raise ManifestError(f"malformed manifest line {line_no}: {line.rstrip()!r}")
    digest, name = match.groups()
    validate_asset_name(name)
    return digest, name


def verify_manifest(asset_dir: Path, manifest: Path) -> list[str]:
    if not manifest.is_file():
        raise ManifestError(f"manifest does not exist: {manifest}")

    seen: set[str] = set()
    verified: list[str] = []
    lines = manifest.read_text(encoding="utf-8").splitlines()
    if not lines:
        raise ManifestError(f"manifest is empty: {manifest}")

    for index, line in enumerate(lines, start=1):
        expected, name = parse_manifest_line(line, index)
        if name in seen:
            raise ManifestError(f"duplicate manifest entry: {name}")
        seen.add(name)

        asset = asset_dir / name
        if not asset.is_file():
            raise ManifestError(f"manifest asset is missing: {name}")
        actual = sha256_hex(asset)
        if actual != expected:
            raise ManifestError(
                f"checksum mismatch for {name}: expected {expected}, actual {actual}"
            )
        verified.append(name)

    return verified


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate = subparsers.add_parser("generate", help="write a SHA256SUMS file")
    generate.add_argument("asset_dir", type=Path)
    generate.add_argument("--output", type=Path, required=True)
    generate.add_argument(
        "--required-name",
        action="append",
        default=[],
        help="asset basename that must be present; may be repeated",
    )
    generate.add_argument(
        "--exact-required",
        action="store_true",
        help="fail if any non-excluded asset is not listed as required",
    )
    generate.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="asset basename to exclude in addition to checksum manifests",
    )

    verify = subparsers.add_parser("verify", help="verify assets against a manifest")
    verify.add_argument("asset_dir", type=Path)
    verify.add_argument("manifest", type=Path)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "generate":
            excludes = DEFAULT_EXCLUDES | set(args.exclude)
            lines = generate_manifest(
                args.asset_dir,
                args.output,
                required_names(args.required_name),
                exact_required=args.exact_required,
                excludes=excludes,
            )
            print(f"Wrote {args.output} with {len(lines)} asset(s).")
        elif args.command == "verify":
            verified = verify_manifest(args.asset_dir, args.manifest)
            print(f"Verified {len(verified)} asset(s) from {args.manifest}.")
        else:
            raise AssertionError(args.command)
    except ManifestError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
