#!/usr/bin/env python3
"""Create and verify the fail-closed provenance marker for official Pulp SDKs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from sdk_capability_handoff import (
    HandoffError,
    HANDOFF_PATH,
    build_handoff,
    verify_handoff,
    write_atomically as write_handoff_atomically,
)


SCHEMA = "pulp.sdk-provenance.v1"
PROFILE = "official-release"
SHA_RE = re.compile(r"[0-9a-f]{40}")
VERSION_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
PLATFORM_RE = re.compile(r"(?:darwin|linux|windows)-(?:arm64|x64)")
INSPECTOR_SDK_FLOOR = (0, 772, 0)
PRODUCT_MATRIX = Path(__file__).with_name("release_product_matrix.json")
BUILD_INFO_PATH = Path("include/pulp/runtime/build_info.hpp")
BUILD_INFO_MAX_BYTES = 64 * 1024
INTEGRITY_SCHEMA = "pulp.sdk-integrity.v1"
INTEGRITY_SDK_FLOOR = (0, 807, 0)
BUILD_INFO_CANONICAL_RE = re.compile(
    r"""\A\s*
    \#pragma[^\S\r\n]+once[^\S\r\n]*\r?\n\s*
    \#include[^\S\r\n]+<string_view>[^\S\r\n]*\r?\n\s*
    namespace\s+pulp::runtime\s*\{\s*
    inline\s+constexpr\s+std::string_view\s+kBuildType\s*=\s*
        "(?P<build_type>[^"\\\r\n]*)"\s*;\s*
    inline\s+constexpr\s+std::string_view\s+kBuildIso8601\s*=\s*
        "(?:\\[^\r\n]|[^"\\\r\n])*"\s*;\s*
    inline\s+constexpr\s+std::string_view\s+kGitSha\s*=\s*
        "(?P<git_sha>[^"\\\r\n]*)"\s*;\s*
    inline\s+constexpr\s+bool\s+kGitDirty\s*=\s*
        (?P<git_dirty>true|false)\s*;\s*
    inline\s+constexpr\s+std::string_view\s+kSdkVersion\s*=\s*
        "(?P<sdk_version>[^"\\\r\n]*)"\s*;\s*
    inline\s+constexpr\s+std::string_view\s+kStampLabel\s*=\s*
        "(?:\\[^\r\n]|[^"\\\r\n])*"\s*;\s*
    \}\s*\Z""",
    re.VERBOSE,
)


class ProvenanceError(RuntimeError):
    pass


def _coherence_paths(platform: str) -> tuple[Path, Path]:
    archive = (
        Path("lib/pulp-view-script.lib")
        if platform.startswith("windows-")
        else Path("lib/libpulp-view-script.a")
    )
    return Path("include/pulp/view/widget_bridge.hpp"), archive


def _sha256(path: Path) -> str:
    try:
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError as exc:
        raise ProvenanceError(f"cannot hash SDK coherence member {path}: {exc}") from exc


def build_integrity(prefix: Path, platform: str) -> dict[str, object]:
    files = {
        path.as_posix(): _sha256(prefix / path)
        for path in _coherence_paths(platform)
    }
    return {"schema": INTEGRITY_SCHEMA, "algorithm": "sha256", "files": files}


def verify_integrity(prefix: Path, platform: str, value: object) -> None:
    expected = build_integrity(prefix, platform)
    if value != expected:
        raise ProvenanceError(
            "SDK coherence integrity mismatch: widget_bridge.hpp and "
            "pulp-view-script must come from the same stamped SDK"
        )


def _version_tuple(value: str) -> tuple[int, int, int]:
    if not VERSION_RE.fullmatch(value):
        raise ProvenanceError(f"invalid SDK version: {value!r}")
    return tuple(int(part) for part in value.split("."))  # type: ignore[return-value]


def _capability_handoff_required(prefix: Path) -> bool:
    try:
        matrix = json.loads(PRODUCT_MATRIX.read_text(encoding="utf-8"))
        floor = str(matrix["capability_handoff_floor"])
    except (KeyError, TypeError, OSError, json.JSONDecodeError) as exc:
        raise ProvenanceError(
            f"cannot determine capability handoff floor from {PRODUCT_MATRIX}: {exc}"
        ) from exc
    return _version_tuple(_read_text(prefix / "version.txt")) >= _version_tuple(floor)


def _importer_runtime_paths() -> set[str]:
    try:
        matrix = json.loads(PRODUCT_MATRIX.read_text(encoding="utf-8"))
        members = matrix["common_cli_members"]
    except (KeyError, TypeError, OSError, json.JSONDecodeError) as exc:
        raise ProvenanceError(
            f"cannot determine importer runtime contract from {PRODUCT_MATRIX}: {exc}"
        ) from exc
    prefix = "browser_capture/"
    paths = {
        "bin/browser_capture-v1/" + str(member).removeprefix(prefix)
        for member in members
        if isinstance(member, str) and member.startswith(prefix)
    }
    if not paths:
        raise ProvenanceError(f"empty importer runtime contract in {PRODUCT_MATRIX}")
    return paths


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError as exc:
        raise ProvenanceError(f"cannot read {path}: {exc}") from exc


def _read_bounded_text(path: Path, *, limit: int) -> str:
    try:
        with path.open("rb") as handle:
            data = handle.read(limit + 1)
    except OSError as exc:
        raise ProvenanceError(f"cannot read {path}: {exc}") from exc
    if len(data) > limit:
        raise ProvenanceError(f"{path} exceeds the {limit}-byte limit")
    try:
        return data.decode("utf-8").strip()
    except UnicodeDecodeError as exc:
        raise ProvenanceError(f"{path} is not valid UTF-8: {exc}") from exc


def _strip_cpp_comments(text: str) -> str:
    if re.search(r"\\\r?\n", text):
        raise ProvenanceError(
            "installed build_info.hpp contains unsupported C++ line splicing"
        )
    output: list[str] = []
    index = 0
    state = "code"
    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and following == "/":
                output.extend("  ")
                index += 2
                state = "line-comment"
                continue
            if char == "/" and following == "*":
                output.extend("  ")
                index += 2
                state = "block-comment"
                continue
            output.append(char)
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            index += 1
            continue
        if state == "line-comment":
            output.append("\n" if char == "\n" else " ")
            index += 1
            if char == "\n":
                state = "code"
            continue
        if state == "block-comment":
            if char == "*" and following == "/":
                output.extend("  ")
                index += 2
                state = "code"
            else:
                output.append("\n" if char == "\n" else " ")
                index += 1
            continue
        output.append(char)
        index += 1
        if char == "\\" and index < len(text):
            output.append(text[index])
            index += 1
        elif (state == "string" and char == '"') or (
            state == "character" and char == "'"
        ):
            state = "code"
    if state in {"block-comment", "string", "character"}:
        raise ProvenanceError("installed build_info.hpp has unterminated C++ syntax")
    return "".join(output)


def parse_build_info(text: str) -> dict[str, str | bool]:
    text = _strip_cpp_comments(text)
    if 'R"' in text:
        raise ProvenanceError(
            "installed build_info.hpp contains unsupported C++ raw strings"
        )
    if "%:" in text:
        raise ProvenanceError(
            "installed build_info.hpp contains unsupported preprocessing digraphs"
        )
    match = BUILD_INFO_CANONICAL_RE.fullmatch(text)
    if match is None:
        raise ProvenanceError(
            "installed build_info.hpp does not match the canonical generated structure"
        )
    return {
        "kBuildType": match.group("build_type"),
        "kGitSha": match.group("git_sha"),
        "kGitDirty": match.group("git_dirty") == "true",
        "kSdkVersion": match.group("sdk_version"),
    }


def verify_build_info_text(
    text: str, *, expected_version: str, expected_source_sha: str
) -> dict[str, str | bool]:
    build_info = parse_build_info(text)
    if build_info["kBuildType"] != "Release":
        raise ProvenanceError(
            "installed build_info.hpp is not a Release build "
            f"(kBuildType={build_info['kBuildType']!r})"
        )
    if build_info["kGitDirty"] is not False:
        raise ProvenanceError("installed build_info.hpp reports tracked source changes")
    if build_info["kSdkVersion"] != expected_version:
        raise ProvenanceError(
            "installed build_info.hpp SDK version does not match provenance "
            f"({build_info['kSdkVersion']!r} != {expected_version!r})"
        )
    short_sha = build_info["kGitSha"]
    if (
        not isinstance(short_sha, str)
        or re.fullmatch(r"[0-9a-f]{7,40}", short_sha) is None
        or not expected_source_sha.startswith(short_sha)
    ):
        raise ProvenanceError(
            "installed build_info.hpp source SHA does not match provenance "
            f"({short_sha!r} is not a prefix of {expected_source_sha!r})"
        )
    return build_info


def verify_installed_build_info(
    prefix: Path, *, expected_version: str, expected_source_sha: str
) -> dict[str, str | bool]:
    return verify_build_info_text(
        _read_bounded_text(
            prefix / BUILD_INFO_PATH,
            limit=BUILD_INFO_MAX_BYTES,
        ),
        expected_version=expected_version,
        expected_source_sha=expected_source_sha,
    )


def _cache_bool(build_dir: Path, name: str) -> bool:
    cache = _read_text(build_dir / "CMakeCache.txt")
    match = re.search(rf"(?m)^{re.escape(name)}:BOOL=(.+)$", cache)
    if not match:
        raise ProvenanceError(f"{name} is missing from {build_dir / 'CMakeCache.txt'}")
    value = match.group(1).strip().upper()
    if value not in {"ON", "OFF"}:
        raise ProvenanceError(f"{name} has non-boolean cache value {value!r}")
    return value == "ON"


def _git(repo: Path, *args: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise ProvenanceError(f"git {' '.join(args)} failed: {detail}")
    return completed.stdout.strip()


def build_release_marker(
    *,
    prefix: Path,
    build_dir: Path,
    source_dir: Path,
    release_tag: str,
    source_sha: str,
    platform: str,
) -> dict[str, object]:
    version = _read_text(prefix / "version.txt")
    build_type = _read_text(prefix / "sdk_build_type.txt")
    if not VERSION_RE.fullmatch(version):
        raise ProvenanceError(f"installed SDK version is invalid: {version!r}")
    if release_tag != f"v{version}":
        raise ProvenanceError(
            f"release tag {release_tag!r} does not match installed SDK version {version!r}"
        )
    if build_type != "Release":
        raise ProvenanceError(
            f"installed SDK build type is {build_type!r}, expected 'Release'"
        )
    if not SHA_RE.fullmatch(source_sha):
        raise ProvenanceError(f"source SHA must be 40 lowercase hex characters: {source_sha!r}")

    head = _git(source_dir, "rev-parse", "HEAD")
    tagged = _git(source_dir, "rev-parse", f"{release_tag}^{{commit}}")
    if head != source_sha or tagged != source_sha:
        raise ProvenanceError(
            "official provenance requires HEAD, the release tag, and source SHA "
            f"to identify one commit (HEAD={head}, tag={tagged}, requested={source_sha})"
        )
    dirty = _git(
        source_dir,
        "status",
        "--porcelain",
        "--untracked-files=no",
        "--ignore-submodules=untracked",
    )
    if dirty:
        raise ProvenanceError(
            "official provenance requires a clean tracked source tree; "
            f"git status reported: {dirty!r}"
        )

    audio_probes = _cache_bool(build_dir, "PULP_ENABLE_AUDIO_PROBES")
    inspector = _cache_bool(build_dir, "PULP_ENABLE_INSPECTOR")
    expected_inspector = _version_tuple(version) >= INSPECTOR_SDK_FLOOR
    if audio_probes or inspector != expected_inspector:
        raise ProvenanceError(
            "official release SDK feature contract requires "
            f"audio_probes=OFF and inspector={'ON' if expected_inspector else 'OFF'}"
        )

    marker = {
        "schema": SCHEMA,
        "kind": "release",
        "profile": PROFILE,
        "distribution_eligible": True,
        "sdk_version": version,
        "source_git_ref": release_tag,
        "source_git_sha": source_sha,
        "source_git_dirty": False,
        "platform": platform,
        "build_type": build_type,
        "features": {
            "audio_probes": False,
            "inspector": expected_inspector,
        },
    }
    if _version_tuple(version) >= INTEGRITY_SDK_FLOOR:
        marker["integrity"] = build_integrity(prefix, platform)
    verify_installed_build_info(
        prefix,
        expected_version=version,
        expected_source_sha=source_sha,
    )
    return marker


def write_atomically(path: Path, marker: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(marker, indent=2, sort_keys=True) + "\n"
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary_path = Path(temporary)
    try:
        fchmod = getattr(os, "fchmod", None)
        if fchmod is not None:
            fchmod(fd, 0o644)
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)


def verify_release_marker(
    prefix: Path, *, expected_platform: str, expected_source_sha: str
) -> dict[str, object]:
    path = prefix / "sdk-provenance.json"
    try:
        marker = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProvenanceError(f"cannot read valid provenance marker {path}: {exc}") from exc
    expected = {
        "schema": SCHEMA,
        "kind": "release",
        "profile": PROFILE,
        "distribution_eligible": True,
        "source_git_dirty": False,
        "build_type": "Release",
    }
    for key, value in expected.items():
        if marker.get(key) != value:
            raise ProvenanceError(f"{path}: {key} is {marker.get(key)!r}, expected {value!r}")
    version = marker.get("sdk_version")
    source_sha = marker.get("source_git_sha")
    if not isinstance(version, str) or not VERSION_RE.fullmatch(version):
        raise ProvenanceError(f"{path}: sdk_version is invalid")
    if marker.get("source_git_ref") != f"v{version}":
        raise ProvenanceError(f"{path}: source_git_ref does not match sdk_version")
    if not isinstance(source_sha, str) or not SHA_RE.fullmatch(source_sha):
        raise ProvenanceError(f"{path}: source_git_sha is invalid")
    if source_sha != expected_source_sha:
        raise ProvenanceError(
            f"{path}: source_git_sha is {source_sha!r}, expected {expected_source_sha!r}"
        )
    if not PLATFORM_RE.fullmatch(expected_platform):
        raise ProvenanceError(f"expected platform is invalid: {expected_platform!r}")
    if marker.get("platform") != expected_platform:
        raise ProvenanceError(
            f"{path}: platform is {marker.get('platform')!r}, expected {expected_platform!r}"
        )
    expected_features = {
        "audio_probes": False,
        "inspector": _version_tuple(version) >= INSPECTOR_SDK_FLOOR,
    }
    if marker.get("features") != expected_features:
        raise ProvenanceError(f"{path}: release feature contract is unsafe")
    if _version_tuple(version) >= INTEGRITY_SDK_FLOOR:
        verify_integrity(prefix, expected_platform, marker.get("integrity"))
    if _read_text(prefix / "version.txt") != version:
        raise ProvenanceError(f"{path}: marker version does not match selected SDK prefix")
    if _read_text(prefix / "sdk_build_type.txt") != "Release":
        raise ProvenanceError(f"{path}: selected SDK prefix is not Release")
    verify_installed_build_info(
        prefix,
        expected_version=version,
        expected_source_sha=source_sha,
    )
    return marker


def verify_release_sdk(
    prefix: Path, *, expected_platform: str, expected_source_sha: str
) -> dict[str, object]:
    """Verify the complete release contract for one selected SDK prefix."""
    marker = verify_release_marker(
        prefix,
        expected_platform=expected_platform,
        expected_source_sha=expected_source_sha,
    )
    if _capability_handoff_required(prefix):
        verify_handoff(
            prefix,
            expected_platform=expected_platform,
            expected_sdk_source_sha=expected_source_sha,
            expected_importer_runtime_paths=_importer_runtime_paths(),
        )
    return marker


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    stamp = subparsers.add_parser("stamp")
    stamp.add_argument("--prefix", type=Path, required=True)
    stamp.add_argument("--build-dir", type=Path, required=True)
    stamp.add_argument("--source-dir", type=Path, required=True)
    stamp.add_argument("--release-tag", required=True)
    stamp.add_argument("--source-sha", required=True)
    stamp.add_argument("--platform", required=True)
    verify = subparsers.add_parser("verify")
    verify.add_argument("--prefix", type=Path, required=True)
    verify.add_argument("--platform", required=True)
    verify.add_argument("--source-sha", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "stamp":
            marker = build_release_marker(
                prefix=args.prefix,
                build_dir=args.build_dir,
                source_dir=args.source_dir,
                release_tag=args.release_tag,
                source_sha=args.source_sha,
                platform=args.platform,
            )
            handoff = None
            if _capability_handoff_required(args.prefix):
                runtime_paths = _importer_runtime_paths()
                handoff = build_handoff(
                    args.prefix,
                    sdk_source_sha=args.source_sha,
                    platform=args.platform,
                    expected_importer_runtime_paths=runtime_paths,
                )
            write_atomically(args.prefix / "sdk-provenance.json", marker)
            if handoff is not None:
                write_handoff_atomically(args.prefix / HANDOFF_PATH, handoff)
            verify_release_sdk(
                args.prefix,
                expected_platform=args.platform,
                expected_source_sha=args.source_sha,
            )
            suffix = " and capability handoff" if handoff is not None else ""
            print(f"OK: stamped official SDK provenance{suffix} in {args.prefix}")
        else:
            verify_release_sdk(
                args.prefix,
                expected_platform=args.platform,
                expected_source_sha=args.source_sha,
            )
            required = _capability_handoff_required(args.prefix)
            suffix = " and capability handoff" if required else ""
            print(f"OK: verified official SDK provenance{suffix} in {args.prefix}")
    except (ProvenanceError, HandoffError, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
