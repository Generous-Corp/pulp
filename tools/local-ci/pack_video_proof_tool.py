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
VERIFY_SCHEMA = "pulp.video-proof-tool-package-verify.v1"
MANIFEST_SCHEMA = "pulp.video-proof-tool-package.v1"
REQUIRED_NPM_DEV_DEPENDENCIES = ("remotion", "ffmpeg-static")
REQUIRED_NPM_SCRIPTS = ("smoke-video-proof", "compose-video-proof")


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


def check_result(name: str, ok: bool, detail: str) -> dict:
    return {"name": name, "ok": ok, "detail": detail}


def manifest_artifact_path(manifest_path: Path, artifact: dict) -> Path:
    artifact_path = Path(str(artifact.get("path") or ""))
    if not artifact_path.is_absolute():
        artifact_path = manifest_path.parent / artifact_path
    return artifact_path


def has_excluded_path(zip_name: str, excluded_paths: list[str]) -> str | None:
    normalized = zip_name.replace("\\", "/")
    for excluded in excluded_paths:
        marker = excluded.rstrip("/")
        if normalized == marker or normalized.startswith(f"{marker}/") or f"/{marker}/" in normalized:
            return excluded
    return None


def safe_zip_name(zip_name: str) -> bool:
    path = Path(zip_name)
    return not path.is_absolute() and ".." not in path.parts and zip_name.startswith(
        f"{PACKAGE_ROOT_RELATIVE.as_posix()}/"
    )


def verify_video_proof_tool_package(manifest_path: Path) -> dict:
    manifest_path = manifest_path.resolve()
    checks: list[dict] = []
    manifest: dict = {}
    archive_names: list[str] = []
    artifact_path: Path | None = None

    try:
        manifest = json.loads(manifest_path.read_text())
        checks.append(check_result("manifest_json", True, f"read {manifest_path}"))
    except (OSError, json.JSONDecodeError) as exc:
        checks.append(check_result("manifest_json", False, str(exc)))
        return {
            "schema": VERIFY_SCHEMA,
            "ok": False,
            "manifest": str(manifest_path),
            "artifact": None,
            "checks": checks,
        }

    checks.extend(
        [
            check_result(
                "manifest_schema",
                manifest.get("schema") == MANIFEST_SCHEMA,
                str(manifest.get("schema")),
            ),
            check_result("tool_id", manifest.get("tool_id") == "video-proof", str(manifest.get("tool_id"))),
            check_result(
                "distribution_lane",
                manifest.get("distribution_lane") == "tool_addon",
                str(manifest.get("distribution_lane")),
            ),
            check_result(
                "package_format",
                manifest.get("package_format") == "not_pulp_add",
                str(manifest.get("package_format")),
            ),
        ]
    )

    policy = manifest.get("policy") if isinstance(manifest.get("policy"), dict) else {}
    expected_policy = {
        "core_pulp": False,
        "pulp_add_package": False,
        "machine_scoped_tool": True,
        "bundles_node_modules": False,
        "bundles_generated_videos": False,
    }
    for key, expected in expected_policy.items():
        checks.append(check_result(f"policy.{key}", policy.get(key) is expected, str(policy.get(key))))

    artifact = manifest.get("artifact") if isinstance(manifest.get("artifact"), dict) else {}
    artifact_path = manifest_artifact_path(manifest_path, artifact)
    if artifact_path.is_file():
        checks.append(check_result("artifact_exists", True, str(artifact_path)))
        actual_size = artifact_path.stat().st_size
        expected_size = artifact.get("size_bytes")
        checks.append(
            check_result("artifact_size", actual_size == expected_size, f"actual={actual_size} expected={expected_size}")
        )
        actual_sha = sha256_file(artifact_path)
        expected_sha = artifact.get("sha256")
        checks.append(
            check_result("artifact_sha256", actual_sha == expected_sha, f"actual={actual_sha} expected={expected_sha}")
        )
        try:
            with zipfile.ZipFile(artifact_path) as archive:
                archive_names = sorted(archive.namelist())
            checks.append(check_result("artifact_zip_readable", True, f"{len(archive_names)} entries"))
        except zipfile.BadZipFile as exc:
            checks.append(check_result("artifact_zip_readable", False, str(exc)))
    else:
        checks.append(check_result("artifact_exists", False, str(artifact_path)))

    expected_files = sorted(str(name) for name in manifest.get("included_files", []))
    checks.append(
        check_result(
            "included_files_match_zip",
            bool(archive_names) and archive_names == expected_files,
            f"zip={len(archive_names)} manifest={len(expected_files)}",
        )
    )

    required_files = sorted(str(PACKAGE_ROOT_RELATIVE / file) for file in INCLUDED_PATHS)
    missing_required = [name for name in required_files if name not in archive_names]
    checks.append(
        check_result(
            "required_files_present",
            not missing_required,
            "missing=" + ",".join(missing_required) if missing_required else f"{len(required_files)} required files",
        )
    )

    unsafe_names = [name for name in archive_names if not safe_zip_name(name)]
    checks.append(
        check_result(
            "zip_paths_safe",
            not unsafe_names,
            "unsafe=" + ",".join(unsafe_names) if unsafe_names else "all entries are relative package paths",
        )
    )

    excluded_paths = list(manifest.get("excluded_paths", EXCLUDED_PATHS))
    excluded_hits = [
        f"{name}:{excluded}"
        for name in archive_names
        for excluded in [has_excluded_path(name, excluded_paths)]
        if excluded
    ]
    checks.append(
        check_result(
            "excluded_paths_absent",
            not excluded_hits,
            "hits=" + ",".join(excluded_hits) if excluded_hits else ",".join(excluded_paths),
        )
    )

    npm_package = manifest.get("npm_package") if isinstance(manifest.get("npm_package"), dict) else {}
    scripts = npm_package.get("scripts") if isinstance(npm_package.get("scripts"), dict) else {}
    dev_dependencies = (
        npm_package.get("dev_dependencies") if isinstance(npm_package.get("dev_dependencies"), dict) else {}
    )
    missing_scripts = [name for name in REQUIRED_NPM_SCRIPTS if name not in scripts]
    missing_deps = [name for name in REQUIRED_NPM_DEV_DEPENDENCIES if name not in dev_dependencies]
    checks.append(
        check_result(
            "npm_scripts_present",
            not missing_scripts,
            "missing=" + ",".join(missing_scripts) if missing_scripts else ",".join(REQUIRED_NPM_SCRIPTS),
        )
    )
    checks.append(
        check_result(
            "npm_dependency_pins_present",
            not missing_deps,
            "missing=" + ",".join(missing_deps) if missing_deps else ",".join(REQUIRED_NPM_DEV_DEPENDENCIES),
        )
    )

    ok = all(check["ok"] for check in checks)
    return {
        "schema": VERIFY_SCHEMA,
        "ok": ok,
        "manifest": str(manifest_path),
        "artifact": str(artifact_path) if artifact_path else None,
        "checks": checks,
    }


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
        "schema": MANIFEST_SCHEMA,
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
    parser.add_argument(
        "--verify",
        type=Path,
        default=None,
        metavar="MANIFEST",
        help="Verify an existing video-proof package manifest and artifact instead of packing.",
    )
    parser.add_argument("--json", action="store_true", help="Emit the full manifest as JSON.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.verify:
        result = verify_video_proof_tool_package(args.verify)
        if args.json:
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            status = "PASS" if result["ok"] else "FAIL"
            print(f"{status} video-proof tool package: {result['manifest']}")
            for check in result["checks"]:
                check_status = "PASS" if check["ok"] else "FAIL"
                print(f"  {check_status} {check['name']}: {check['detail']}")
        return 0 if result["ok"] else 1

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
