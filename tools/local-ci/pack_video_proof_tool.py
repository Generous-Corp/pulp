#!/usr/bin/env python3
"""Pack the optional video-proof tool add-on source artifact."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import zipfile


PACKAGE_ROOT_RELATIVE = Path("tools/local-ci")
DEFAULT_OUTPUT_ROOT_RELATIVE = Path("build/video-proof-tool")
ZIP_TIMESTAMP = (2026, 1, 1, 0, 0, 0)
INCLUDED_PATHS = (
    Path("package.json"),
    Path("package-lock.json"),
    Path("scripts/compose-video-proof.mjs"),
    Path("scripts/smoke-video-proof.mjs"),
    Path("remotion-proof/README.md"),
    Path("remotion-proof/index.jsx"),
    Path("remotion-proof/validation-proof.jsx"),
)
EXCLUDED_PATHS = (
    "node_modules/",
    ".video-proof-smoke/",
    "video/",
    ".remotion/",
    ".cache/",
)


def repo_root_from(start: Path) -> Path:
    current = start.resolve()
    for candidate in (current, *current.parents):
        if (candidate / "tools" / "local-ci" / "package.json").is_file():
            return candidate
    raise RuntimeError(f"could not find repo root from {start}")


def git_value(repo_root: Path, *args: str) -> str | None:
    try:
        completed = subprocess.run(
            ["git", *args],
            cwd=repo_root,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except OSError:
        return None
    if completed.returncode != 0:
        return None
    value = completed.stdout.strip()
    return value or None


def load_package_json(package_root: Path) -> dict:
    try:
        return json.loads((package_root / "package.json").read_text())
    except FileNotFoundError as exc:
        raise RuntimeError(f"package.json not found at {package_root}") from exc


def included_files(package_root: Path) -> list[Path]:
    missing: list[str] = []
    files: list[Path] = []
    for relative in INCLUDED_PATHS:
        path = package_root / relative
        if not path.is_file():
            missing.append(str(relative))
        else:
            files.append(relative)
    if missing:
        raise RuntimeError("video-proof tool package is incomplete; missing: " + ", ".join(missing))
    return sorted(files, key=lambda path: path.as_posix())


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_zip(package_root: Path, output_zip: Path, files: list[Path]) -> None:
    output_zip.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for relative in files:
            source = package_root / relative
            zip_info = zipfile.ZipInfo(str(PACKAGE_ROOT_RELATIVE / relative), ZIP_TIMESTAMP)
            zip_info.compress_type = zipfile.ZIP_DEFLATED
            zip_info.external_attr = 0o644 << 16
            archive.writestr(zip_info, source.read_bytes())


def generated_at() -> str:
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch:
        try:
            timestamp = int(epoch)
        except ValueError:
            timestamp = 0
    else:
        timestamp = int(time.time())
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(timestamp))


def create_manifest(
    *,
    repo_root: Path,
    package_root: Path,
    output_zip: Path,
    files: list[Path],
    package_json: dict,
) -> dict:
    return {
        "schema": "pulp.video-proof-tool-package.v1",
        "tool_id": "video-proof",
        "distribution_lane": "tool_addon",
        "package_format": "not_pulp_add",
        "install_command": "pulp tool install video-proof",
        "source_tree_iteration_command": "npm --prefix tools/local-ci install",
        "artifact_status": "packed_source_artifact",
        "artifact": {
            "path": str(output_zip),
            "name": output_zip.name,
            "size_bytes": output_zip.stat().st_size,
            "sha256": sha256_file(output_zip),
        },
        "npm_package": {
            "name": package_json.get("name"),
            "version": package_json.get("version"),
            "private": package_json.get("private") is True,
            "scripts": package_json.get("scripts", {}),
            "dev_dependencies": package_json.get("devDependencies", {}),
        },
        "source": {
            "repo_root": str(repo_root),
            "branch": git_value(repo_root, "branch", "--show-current"),
            "sha": git_value(repo_root, "rev-parse", "HEAD"),
        },
        "included_files": [str(PACKAGE_ROOT_RELATIVE / file) for file in files],
        "excluded_paths": list(EXCLUDED_PATHS),
        "policy": {
            "core_pulp": False,
            "pulp_add_package": False,
            "machine_scoped_tool": True,
            "bundles_node_modules": False,
            "bundles_generated_videos": False,
            "license_note": (
                "The archive contains Pulp-owned tool source only. Remotion and "
                "ffmpeg-static are installed by npm under their own licenses."
            ),
        },
        "generated_at": generated_at(),
    }


def pack_video_proof_tool(
    *,
    repo_root: Path,
    output_dir: Path,
    version: str | None = None,
) -> dict:
    repo_root = repo_root.resolve()
    package_root = repo_root / PACKAGE_ROOT_RELATIVE
    package_json = load_package_json(package_root)
    tool_version = version or str(package_json.get("version") or "0.0.0")
    files = included_files(package_root)
    output_zip = output_dir / f"pulp-video-proof-tool-{tool_version}.zip"
    write_zip(package_root, output_zip, files)
    manifest = create_manifest(
        repo_root=repo_root,
        package_root=package_root,
        output_zip=output_zip,
        files=files,
        package_json=package_json,
    )
    manifest_path = output_zip.with_suffix(".manifest.json")
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    manifest["manifest_path"] = str(manifest_path)
    return manifest


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=None,
        help="Repository root. Defaults to auto-discovery from this script.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Directory for the zip and manifest. Defaults to build/video-proof-tool.",
    )
    parser.add_argument("--version", help="Override the artifact version label.")
    parser.add_argument("--json", action="store_true", help="Emit the full manifest as JSON.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve() if args.repo_root else repo_root_from(Path(__file__))
    output_dir = args.output_dir or (repo_root / DEFAULT_OUTPUT_ROOT_RELATIVE)
    manifest = pack_video_proof_tool(repo_root=repo_root, output_dir=output_dir, version=args.version)
    if args.json:
        print(json.dumps(manifest, indent=2, sort_keys=True))
    else:
        artifact = manifest["artifact"]
        print(f"Packed video-proof tool: {artifact['path']}")
        print(f"  size: {artifact['size_bytes']} bytes")
        print(f"  sha256: {artifact['sha256']}")
        print(f"  manifest: {manifest['manifest_path']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
