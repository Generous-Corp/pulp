#!/usr/bin/env python3
"""Validate the checked-in Windows VM image contract before runner startup."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys
import tomllib


SCHEMA = 1
SHA256_LENGTH = 64


class ManifestError(ValueError):
    pass


def _path(value: object, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ManifestError(f"{label} must be a non-empty path")
    return Path(os.path.expandvars(os.path.expanduser(value)))


def _digest(path: Path, label: str) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise ManifestError(f"{label} cannot be read: {path}: {exc}") from exc
    return digest.hexdigest()


def _checked_file(raw_path: object, expected: object, label: str) -> Path:
    path = _path(raw_path, f"{label}.path")
    if not path.is_file():
        raise ManifestError(f"{label} is missing: {path}")
    if not isinstance(expected, str) or len(expected) != SHA256_LENGTH:
        raise ManifestError(f"{label}.sha256 must be a 64-character hex digest")
    expected = expected.lower()
    if any(char not in "0123456789abcdef" for char in expected):
        raise ManifestError(f"{label}.sha256 must be lowercase hexadecimal")
    actual = _digest(path, label)
    if actual != expected:
        raise ManifestError(f"{label} sha256 mismatch: expected {expected}, got {actual}")
    return path


def load_manifest(path: Path, base_override: str | None = None) -> dict[str, Path]:
    try:
        with path.open("rb") as stream:
            data = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ManifestError(f"cannot read manifest {path}: {exc}") from exc
    if data.get("schema") != SCHEMA:
        raise ManifestError(f"schema must be {SCHEMA}")
    if data.get("strategy") != "ephemeral-overlay":
        raise ManifestError("strategy must be ephemeral-overlay")
    if data.get("provisioning") != "ephemeral":
        raise ManifestError("provisioning must be ephemeral")
    if data.get("architecture") != "arm64":
        raise ManifestError("architecture must be arm64")

    base = data.get("base_image")
    firmware = data.get("firmware")
    overlay = data.get("overlay")
    cache = data.get("cache")
    if not isinstance(base, dict) or not isinstance(firmware, dict):
        raise ManifestError("base_image and firmware tables are required")
    if not isinstance(overlay, dict) or overlay.get("copy_on_write") is not True:
        raise ManifestError("overlay.copy_on_write must be true")
    if overlay.get("cleanup") != "on-exit":
        raise ManifestError("overlay.cleanup must be on-exit")
    state_dir = _path(overlay.get("state_dir"), "overlay.state_dir")
    if not state_dir.is_absolute():
        raise ManifestError("overlay.state_dir must be absolute after expansion")
    if not isinstance(cache, dict) or cache.get("mode") != "guest-local":
        raise ManifestError("cache.mode must be guest-local")

    base_path = _checked_file(
        base_override if base_override is not None else base.get("path"),
        base.get("sha256"),
        "base_image",
    )
    code_path = _checked_file(firmware.get("code_path"), firmware.get("code_sha256"), "firmware.code")
    vars_path = _checked_file(
        firmware.get("vars_template_path"), firmware.get("vars_template_sha256"), "firmware.vars_template"
    )
    return {"base": base_path, "firmware": code_path, "vars": vars_path, "state": state_dir}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--base", help="override base image path; digest remains manifest-pinned")
    parser.add_argument("--print", dest="print_name", choices=("base", "firmware", "vars", "all"))
    args = parser.parse_args(argv)
    try:
        paths = load_manifest(args.manifest, args.base)
    except ManifestError as exc:
        print(f"windows-vm-manifest: FAIL: {exc}", file=sys.stderr)
        return 1
    if args.print_name:
        if args.print_name == "all":
            print("\n".join(str(paths[name]) for name in ("base", "firmware", "vars", "state")))
        else:
            print(paths[args.print_name])
    else:
        print(f"windows-vm-manifest: PASS: {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
