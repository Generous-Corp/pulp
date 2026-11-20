#!/usr/bin/env python3
"""Reader and fail-closed validator for the repository's Vellum lock."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / "pulp.lock"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
TAG = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+$")


def load(path: Path = LOCK) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError("lock root must be an object")
    return value


def validate(lock: dict[str, Any], *, tag_sha: str | None = None,
             sdk_archive: Path | None = None, gpu_archive: Path | None = None) -> list[str]:
    errors: list[str] = []
    if set(lock) != {"schema", "vellum", "render_toolchain"}:
        errors.append("lock has unknown or missing top-level fields")
    if lock.get("schema") != "pulp.lock.v1":
        errors.append("schema must be pulp.lock.v1")
    vellum = lock.get("vellum")
    if not isinstance(vellum, dict):
        return [*errors, "vellum must be an object"]
    required = {"tag", "sha", "sdk_url", "sdk_digest", "gpu_runtime_digest"}
    if set(vellum) != required:
        errors.append("vellum must contain exactly tag, sha, sdk_url, sdk_digest and gpu_runtime_digest")
    tag, sha = vellum.get("tag"), vellum.get("sha")
    if not isinstance(tag, str) or not TAG.fullmatch(tag):
        errors.append("vellum.tag must be a release tag such as v0.2.0")
    if not isinstance(sha, str) or not HEX40.fullmatch(sha):
        errors.append("vellum.sha must be lowercase 40-hex")
    sdk_url = vellum.get("sdk_url")
    if not isinstance(sdk_url, str) or not sdk_url.startswith("https://"):
        errors.append("vellum.sdk_url must be an https URL")
    for field in ("sdk_digest", "gpu_runtime_digest"):
        value = vellum.get(field)
        if not isinstance(value, str) or not HEX64.fullmatch(value):
            errors.append(f"vellum.{field} must be lowercase 64-hex")
    render = lock.get("render_toolchain")
    if not isinstance(render, dict) or set(render) != {"lock_digest"}:
        errors.append("render_toolchain must contain exactly lock_digest")
    elif not isinstance(render.get("lock_digest"), str) or not HEX64.fullmatch(render["lock_digest"]):
        errors.append("render_toolchain.lock_digest must be lowercase 64-hex")
    if tag_sha is not None and isinstance(sha, str) and sha != tag_sha:
        errors.append(f"vellum.tag resolves to {tag_sha}, but lock pins {sha}")
    for path, field in ((sdk_archive, "sdk_digest"), (gpu_archive, "gpu_runtime_digest")):
        if path is None:
            continue
        try:
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError as exc:
            errors.append(f"cannot hash {path}: {exc}")
            continue
        if digest != vellum.get(field):
            errors.append(f"{field} mismatch: lock {vellum.get(field)}, file {digest}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", type=Path, default=LOCK)
    parser.add_argument("--tag-sha")
    parser.add_argument("--sdk-archive", type=Path)
    parser.add_argument("--gpu-runtime-archive", type=Path)
    args = parser.parse_args()
    try:
        errors = validate(load(args.lock), tag_sha=args.tag_sha,
                          sdk_archive=args.sdk_archive, gpu_archive=args.gpu_runtime_archive)
    except ValueError as exc:
        print(f"pulp.lock: {exc}")
        return 1
    for error in errors:
        print(f"pulp.lock: {error}")
    if errors:
        return 1
    print(f"pulp.lock: valid ({args.lock})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
