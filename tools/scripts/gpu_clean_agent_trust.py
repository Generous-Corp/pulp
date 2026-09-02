#!/usr/bin/env python3
"""Fail-closed trust primitives for the A5 clean-agent journey.

This module intentionally contains no acceptance decision.  It binds inputs to
clean Git/CMake/install provenance, identifies the official macOS Codex binary,
constructs the single outer Seatbelt policy, and signs a recorder-produced
session with a one-use key.  ``gpu_clean_agent_journey.py`` remains the only
surface that composes these primitives into the two-party gate.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import pathlib
import re
import socket
import stat
import subprocess
import sys
import tempfile
from typing import Any, Iterable

import gpu_clean_agent_isolation as isolation
import sdk_provenance


ExactHostConnectProxy = isolation.ExactHostConnectProxy


MAX_COMMAND_OUTPUT_BYTES = 4 * 1024 * 1024
MAX_FILE_BYTES = 32 * 1024 * 1024
MAX_BINARY_BYTES = 512 * 1024 * 1024
MAX_SESSION_EVENTS = 4096
OPENAI_TEAM_ID = "2DC432GLL2"
OPENAI_AUTHORITY = "Developer ID Application: OpenAI OpCo, LLC (2DC432GLL2)"
AUTHORIZED_GIT_SIGNING_FINGERPRINT = "SHA256:rxAigIze6ikPj3KR1cyL81fmaT0k2qVpwoBxC74gYMc"
CODEX_VERSION_RE = re.compile(r"codex-cli ([0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?)")
SHA40_RE = re.compile(r"[0-9a-f]{40}")
UUID_RE = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
)
PUBLIC_MATERIAL_MAPPINGS = (
    ("docs/status/gpu-recipes.yaml", "share/pulp/gpu-recipes.yaml", "catalog"),
    ("docs/status/gpu-recipes.schema.json", "share/pulp/gpu-recipes.schema.json", "catalog-schema"),
    (
        "docs/guides/gpu-validation-checklist.md",
        "share/pulp/docs/guides/gpu-validation-checklist.md",
        "guide",
    ),
    ("docs/reference/cli.md", "share/pulp/docs/reference/cli.md", "reference"),
)
SYSTEM_READ_ROOTS = (
    pathlib.Path("/System"),
    pathlib.Path("/usr/bin"),
    pathlib.Path("/usr/lib"),
    pathlib.Path("/usr/libexec"),
    pathlib.Path("/usr/sbin"),
    pathlib.Path("/usr/share"),
    pathlib.Path("/bin"),
    pathlib.Path("/sbin"),
    pathlib.Path("/Library/Apple"),
    pathlib.Path("/Library/Fonts"),
    pathlib.Path("/Library/Frameworks"),
    pathlib.Path("/Library/Keychains"),
    pathlib.Path("/Library/Preferences"),
    pathlib.Path("/Library/Security"),
    pathlib.Path("/private/etc/group"),
    pathlib.Path("/private/etc/hosts"),
    pathlib.Path("/private/etc/localtime"),
    pathlib.Path("/private/etc/passwd"),
    pathlib.Path("/private/etc/paths"),
    pathlib.Path("/private/etc/paths.d"),
    pathlib.Path("/private/etc/protocols"),
    pathlib.Path("/private/etc/services"),
    pathlib.Path("/private/etc/ssl"),
    pathlib.Path("/private/var/db/timezone"),
    pathlib.Path("/dev/fd"),
    pathlib.Path("/dev/null"),
    pathlib.Path("/dev/random"),
    pathlib.Path("/dev/stderr"),
    pathlib.Path("/dev/stdin"),
    pathlib.Path("/dev/stdout"),
    pathlib.Path("/dev/urandom"),
)
CODEX_KEYCHAIN_SERVICE = "Codex Auth"


class TrustError(RuntimeError):
    """A trust-boundary check failed closed."""


def json_bytes(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def read_regular(path: pathlib.Path, limit: int = MAX_FILE_BYTES) -> bytes:
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except OSError as exc:
        raise TrustError(f"required trust input is unavailable: {path}") from exc
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            raise TrustError(f"trust input is not one confined regular file: {path}")
        if metadata.st_size > limit:
            raise TrustError(f"trust input exceeds its {limit}-byte bound: {path}")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(descriptor, min(1024 * 1024, limit + 1 - total))
            if not chunk:
                break
            total += len(chunk)
            if total > limit:
                raise TrustError(f"trust input exceeds its {limit}-byte bound: {path}")
            chunks.append(chunk)
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def sha256_file(path: pathlib.Path, limit: int = MAX_FILE_BYTES) -> str:
    return sha256_bytes(read_regular(path, limit))


def require_real_directory(path: pathlib.Path) -> pathlib.Path:
    if not path.is_absolute() or any(part in {".", ".."} for part in path.parts):
        raise TrustError(f"directory must be an absolute normalized path: {path}")
    try:
        metadata = path.lstat()
        resolved = path.resolve(strict=True)
    except OSError as exc:
        raise TrustError(f"directory is unavailable: {path}") from exc
    if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode) or resolved != path:
        raise TrustError(f"directory or an ancestor is not a real path: {path}")
    return path


def _run_bytes(
    argv: list[str], *, cwd: pathlib.Path | None = None, input_bytes: bytes | None = None,
    timeout: float = 30.0, check: bool = True, env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[bytes]:
    try:
        result = isolation.bounded_run(
            argv,
            cwd=str(cwd) if cwd is not None else None,
            input_bytes=input_bytes,
            timeout=timeout,
            env={**(os.environ if env is None else env), "NO_COLOR": "1"},
        )
    except isolation.IsolationError as exc:
        raise TrustError(f"trust command could not complete: {argv[0]}: {exc}") from exc
    if check and result.returncode != 0:
        detail = (result.stderr or result.stdout).decode("utf-8", errors="replace").strip()
        raise TrustError(f"trust command failed ({result.returncode}): {' '.join(argv)}: {detail}")
    return result


def _text(result: subprocess.CompletedProcess[bytes]) -> str:
    try:
        return result.stdout.decode("utf-8").strip()
    except UnicodeDecodeError as exc:
        raise TrustError("trust command output is not UTF-8") from exc


def _git(root: pathlib.Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    return _run_bytes(
        [
            "/usr/bin/git", "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false",
            "-C", str(root), *args,
        ],
        check=check,
    )


def _relative_repo_path(root: pathlib.Path, path: pathlib.Path) -> pathlib.PurePosixPath:
    try:
        relative = path.relative_to(root)
    except ValueError as exc:
        raise TrustError(f"required document is outside its repository: {path}") from exc
    if not relative.parts or any(part in {"", ".", ".."} for part in relative.parts):
        raise TrustError(f"required document path is not normalized: {path}")
    return pathlib.PurePosixPath(relative.as_posix())


def tracked_file_identity(root: pathlib.Path, path: pathlib.Path) -> dict[str, Any]:
    relative = _relative_repo_path(root, path)
    _git(root, "ls-files", "--error-unmatch", "--", relative.as_posix())
    blob = _text(_git(root, "rev-parse", f"HEAD:{relative.as_posix()}"))
    if SHA40_RE.fullmatch(blob) is None:
        raise TrustError(f"Git did not return a blob identity for {relative}")
    committed = _git(root, "show", f"HEAD:{relative.as_posix()}").stdout
    if len(committed) > MAX_FILE_BYTES:
        raise TrustError(f"tracked document exceeds its byte bound: {relative}")
    working = read_regular(path)
    if working != committed:
        raise TrustError(f"working document differs from HEAD: {relative}")
    return {
        "path": relative.as_posix(),
        "git_blob": blob,
        "bytes": len(committed),
        "sha256": sha256_bytes(committed),
    }


def _github_repository(url: str) -> str:
    match = re.fullmatch(
        r"(?:git@github\.com:|https://github\.com/)([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(?:\.git)?",
        url,
    )
    if match is None:
        raise TrustError("Git origin is not one canonical GitHub SSH or HTTPS URL")
    return match.group(1)


def _github_event_protected_base(
    root: pathlib.Path, *, expected_repository: str,
) -> str:
    """Resolve an immutable protected base when CI has no origin/main ref.

    GitHub merge-group checkouts do not consistently materialize the
    ``refs/remotes/origin/main`` remote-tracking ref.  The event payload is the
    only accepted substitute: it is outside the candidate checkout, names the
    canonical repository, and pins the base by object ID.  Deliberately do not
    infer a base from HEAD or its parents.
    """

    event_name = os.environ.get("GITHUB_EVENT_NAME", "")
    event_path_text = os.environ.get("GITHUB_EVENT_PATH", "")
    if event_name not in {"pull_request", "merge_group"} or not event_path_text:
        raise TrustError(
            "Git repository has no exact origin/main identity or supported "
            "GitHub event-pinned protected base"
        )
    event_path = pathlib.Path(event_path_text)
    try:
        resolved_event_path = event_path.resolve(strict=True)
    except OSError as exc:
        raise TrustError("GitHub event path is unavailable") from exc
    if (
        not event_path.is_absolute()
        or resolved_event_path != event_path
        or event_path.is_relative_to(root)
    ):
        raise TrustError("GitHub event path is not one exact absolute file")
    try:
        payload = json.loads(read_regular(event_path, 1024 * 1024))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise TrustError("GitHub event payload is not bounded valid JSON") from exc
    if not isinstance(payload, dict):
        raise TrustError("GitHub event payload is not one JSON object")
    repository = payload.get("repository")
    if not isinstance(repository, dict) or repository.get("full_name") != expected_repository:
        raise TrustError("GitHub event repository does not match the canonical Git origin")
    event = payload.get(event_name)
    if not isinstance(event, dict):
        raise TrustError(f"GitHub {event_name} payload is unavailable")
    if event_name == "pull_request":
        base = event.get("base")
        candidate = base.get("sha") if isinstance(base, dict) else None
    else:
        candidate = event.get("base_sha")
    if not isinstance(candidate, str) or SHA40_RE.fullmatch(candidate) is None:
        raise TrustError("GitHub event has no exact protected-base commit identity")
    event_head = os.environ.get("GITHUB_SHA", "")
    revision = _text(_git(root, "rev-parse", "HEAD"))
    if SHA40_RE.fullmatch(event_head) is None or event_head != revision:
        raise TrustError("GitHub event is not bound to the exact checkout HEAD")
    resolved = _text(_git(root, "rev-parse", "--verify", f"{candidate}^{{commit}}"))
    if resolved != candidate:
        raise TrustError("GitHub event protected base is not the exact local commit object")
    return candidate


def _commit_signature(root: pathlib.Path) -> dict[str, str]:
    payload = _git(root, "log", "-1", "--format=%G?%x00%GS%x00%GF", "HEAD").stdout
    fields = payload.rstrip(b"\n").split(b"\x00")
    if len(fields) != 3:
        raise TrustError("Git did not return a closed commit-signature identity")
    try:
        status, signer, fingerprint = (field.decode("utf-8") for field in fields)
    except UnicodeDecodeError as exc:
        raise TrustError("Git commit-signature identity is not UTF-8") from exc
    if len(status) != 1 or len(signer) > 256 or len(fingerprint) > 256:
        raise TrustError("Git commit-signature identity is malformed")
    return {"status": status, "signer": signer, "fingerprint": fingerprint}


def git_repository_identity(
    root: pathlib.Path, *, expected_repository: str,
    required_document: pathlib.Path | None = None, require_origin_main: bool = False,
    allowed_untracked: Iterable[pathlib.Path] = (),
) -> dict[str, Any]:
    root = require_real_directory(root)
    top = pathlib.Path(_text(_git(root, "rev-parse", "--show-toplevel"))).resolve(strict=True)
    if top != root:
        raise TrustError(f"repository root is not exact: {root} resolves to {top}")
    revision = _text(_git(root, "rev-parse", "HEAD"))
    if SHA40_RE.fullmatch(revision) is None:
        raise TrustError("repository HEAD is not an exact lowercase 40-hex commit")
    status = _git(root, "status", "--porcelain=v1", "-z", "--untracked-files=all").stdout
    allowed_records: set[bytes] = set()
    for allowed_path in allowed_untracked:
        relative = _relative_repo_path(root, allowed_path)
        allowed_records.add(b"?? " + relative.as_posix().encode("utf-8") + b"\x00")
    for record in allowed_records:
        if record in status:
            status = status.replace(record, b"", 1)
    if status:
        raise TrustError(f"repository must be completely clean at {revision}: {root}")
    origin_url = _text(_git(root, "config", "--get", "remote.origin.url"))
    origin_repository = _github_repository(origin_url)
    if origin_repository != expected_repository:
        raise TrustError(
            f"Git origin is {origin_repository}, expected canonical {expected_repository}"
        )
    origin_main_result = _git(
        root, "rev-parse", "--verify", "refs/remotes/origin/main^{commit}", check=False
    )
    origin_main = _text(origin_main_result) if origin_main_result.returncode == 0 else ""
    if not origin_main:
        origin_main = _github_event_protected_base(
            root, expected_repository=expected_repository
        )
    if SHA40_RE.fullmatch(origin_main) is None:
        raise TrustError("Git repository has no exact origin/main identity")
    if require_origin_main and revision != origin_main:
        raise TrustError("canonical plan HEAD must equal its origin/main authority")
    head_ref = _text(_git(root, "symbolic-ref", "--quiet", "--short", "HEAD", check=False))
    if not head_ref:
        head_ref = "detached"
    identity: dict[str, Any] = {
        "root": str(root),
        "revision": revision,
        "clean": True,
        "status_sha256": sha256_bytes(status),
        "origin": {"url": origin_url, "repository": origin_repository},
        "origin_main": origin_main,
        "head_ref": head_ref,
        "commit_signature": _commit_signature(root),
    }
    if required_document is not None:
        identity["document"] = tracked_file_identity(root, required_document)
    return identity


def _cache_value(payload: str, name: str) -> str | None:
    match = re.search(rf"(?m)^{re.escape(name)}:[^=]*=(.*)$", payload)
    return match.group(1).strip() if match else None


def _find_release_build_info(build_root: pathlib.Path) -> pathlib.Path:
    candidates = sorted(
        build_root.glob("core/runtime/generated/*/pulp/runtime/build_info.hpp")
    )
    release: list[pathlib.Path] = []
    for candidate in candidates:
        try:
            parsed = sdk_provenance.parse_build_info(read_regular(candidate, 64 * 1024).decode("utf-8"))
        except (UnicodeDecodeError, sdk_provenance.ProvenanceError, TrustError):
            continue
        if parsed.get("kBuildType") == "Release":
            release.append(candidate.resolve(strict=True))
    if len(release) != 1:
        raise TrustError("build tree must contain exactly one generated Release build_info.hpp")
    return release[0]


def require_release_build_configuration(
    *, build_root: pathlib.Path, source_root: pathlib.Path
) -> tuple[pathlib.Path, pathlib.Path, str, str | None, str | None]:
    """Validate and return the exact build-configuration trust inputs.

    This deliberately runs before repository-cleanliness validation in the
    clean-agent preparer so the always-Debug required macOS lane can prove the
    Release-only boundary without weakening source provenance.
    """

    build_root = require_real_directory(build_root)
    cache_path = build_root / "CMakeCache.txt"
    cache_payload = read_regular(cache_path, MAX_FILE_BYTES).decode("utf-8")
    configured_source = _cache_value(cache_payload, "CMAKE_HOME_DIRECTORY")
    build_type = _cache_value(cache_payload, "CMAKE_BUILD_TYPE")
    configurations = _cache_value(cache_payload, "CMAKE_CONFIGURATION_TYPES")
    configured_cmake = _cache_value(cache_payload, "CMAKE_COMMAND")
    if configured_source != str(source_root):
        raise TrustError("build CMake home does not match the exact source repository")
    if build_type != "Release" and (
        configurations is None or "Release" not in configurations.split(";")
    ):
        raise TrustError("build tree does not expose an exact Release configuration")
    return (
        build_root,
        cache_path,
        configured_source,
        build_type,
        configured_cmake,
    )


def build_install_identity(
    *, source: dict[str, Any], build_root: pathlib.Path, install_script: pathlib.Path,
    installed_prefix: pathlib.Path, installed_pulp: pathlib.Path, timeout: float,
) -> tuple[dict[str, Any], dict[str, Any]]:
    (
        build_root,
        cache_path,
        configured_source,
        build_type,
        configured_cmake,
    ) = require_release_build_configuration(
        build_root=build_root, source_root=pathlib.Path(source["root"])
    )
    installed_prefix = require_real_directory(installed_prefix)
    install_script = install_script.resolve(strict=True)
    if install_script != build_root / "tools/cli/cmake_install.cmake":
        raise TrustError("CLI install script must be the generated script inside the build tree")
    if not configured_cmake or not pathlib.Path(configured_cmake).is_absolute():
        raise TrustError("build tree does not record the configuring CMake executable")
    cmake = pathlib.Path(configured_cmake).resolve(strict=True)
    cmake_metadata = cmake.lstat()
    if not stat.S_ISREG(cmake_metadata.st_mode) or not os.access(cmake, os.X_OK):
        raise TrustError("configured CMake executable is not one regular executable")
    cmake_version = _text(_run_bytes([str(cmake), "--version"], timeout=10.0))
    if re.match(r"^cmake version [0-9]+\.[0-9]+\.[0-9]+", cmake_version) is None:
        raise TrustError("configured CMake executable did not return its canonical version")
    build_info_path = _find_release_build_info(build_root)
    build_info_text = read_regular(build_info_path, 64 * 1024).decode("utf-8")
    source_cmake = read_regular(pathlib.Path(source["root"]) / "CMakeLists.txt", 4 * 1024 * 1024)
    try:
        source_cmake_text = source_cmake.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise TrustError("source CMakeLists.txt is not UTF-8") from exc
    version_match = re.search(
        r"(?s)\bproject\s*\(\s*Pulp\b.*?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        source_cmake_text,
    )
    if version_match is None:
        raise TrustError("source CMakeLists.txt has no canonical Pulp project version")
    try:
        build_info = sdk_provenance.verify_build_info_text(
            build_info_text,
            expected_version=version_match.group(1),
            expected_source_sha=source["revision"],
        )
    except sdk_provenance.ProvenanceError as exc:
        raise TrustError(str(exc)) from exc

    installed_pulp = installed_pulp.resolve(strict=True)
    if installed_pulp != installed_prefix / "bin" / "pulp":
        raise TrustError("--pulp must be the installed prefix's bin/pulp executable")
    installed_cpp = installed_prefix / "bin" / "pulp-cpp"
    if not installed_pulp.is_file() or not os.access(installed_pulp, os.X_OK):
        raise TrustError("installed pulp is not executable")
    if not installed_cpp.is_file() or installed_cpp.is_symlink():
        raise TrustError("installed prefix does not contain a regular bin/pulp-cpp")
    installed_sha = sha256_file(installed_pulp, MAX_BINARY_BYTES)
    if sha256_file(installed_cpp, MAX_BINARY_BYTES) != installed_sha:
        raise TrustError("installed bin/pulp is not an exact copy of installed bin/pulp-cpp")

    with tempfile.TemporaryDirectory(prefix="pulp-a5-install-replay-") as raw_stage:
        stage = pathlib.Path(raw_stage).resolve()
        _run_bytes(
            [
                str(cmake),
                f"-DCMAKE_INSTALL_PREFIX={stage}",
                "-DCMAKE_INSTALL_CONFIG_NAME=Release",
                "-P",
                str(install_script),
            ],
            timeout=timeout,
        )
        staged_cpp = stage / "bin" / "pulp-cpp"
        replay_sha = sha256_file(staged_cpp, MAX_BINARY_BYTES)
        if replay_sha != installed_sha:
            raise TrustError("installed CLI bytes differ from a fresh replay of the build install script")
        replayed_public: dict[str, str] = {}
        for source_relative, installed_relative, role in PUBLIC_MATERIAL_MAPPINGS:
            source_record = tracked_file_identity(
                pathlib.Path(source["root"]), pathlib.Path(source["root"]) / source_relative
            )
            staged_payload = read_regular(stage / installed_relative)
            staged_sha = sha256_bytes(staged_payload)
            if (
                len(staged_payload) != source_record["bytes"]
                or staged_sha != source_record["sha256"]
            ):
                raise TrustError(f"fresh CLI install omitted or altered public {role} material")
            replayed_public[installed_relative] = staged_sha

    return (
        {
            "root": str(build_root),
            "cmake_cache": {
                "path": str(cache_path),
                "sha256": sha256_file(cache_path, MAX_FILE_BYTES),
                "cmake_home_directory": configured_source,
                "build_type": build_type or "multi-config",
            },
            "cmake": {
                "configured_path": configured_cmake,
                "resolved_path": str(cmake),
                "bytes": cmake.stat().st_size,
                "sha256": sha256_file(cmake, MAX_BINARY_BYTES),
                "version_output": cmake_version,
            },
            "install_script": {
                "path": str(install_script),
                "sha256": sha256_file(install_script, 4 * 1024 * 1024),
            },
            "build_info": {
                "path": str(build_info_path),
                "sha256": sha256_bytes(build_info_text.encode("utf-8")),
                **build_info,
            },
            "install_replay": {
                "pulp_cpp_sha256": replay_sha,
                "public_material_sha256": replayed_public,
            },
        },
        {
            "prefix": str(installed_prefix),
            "path": str(installed_pulp),
            "cpp_path": str(installed_cpp),
            "bytes": installed_pulp.stat().st_size,
            "sha256": installed_sha,
            "fresh_install_replay_sha256": replay_sha,
        },
    )


def installed_public_material(
    source_root: pathlib.Path, installed_prefix: pathlib.Path,
) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for source_relative, installed_relative, role in PUBLIC_MATERIAL_MAPPINGS:
        source_path = source_root / source_relative
        source_record = tracked_file_identity(source_root, source_path)
        installed_path = installed_prefix / installed_relative
        payload = read_regular(installed_path)
        if len(payload) != source_record["bytes"] or sha256_bytes(payload) != source_record["sha256"]:
            raise TrustError(f"installed public {role} bytes do not match source HEAD")
        records.append({
            "role": role,
            "source_path": source_relative,
            "installed_path": str(installed_path),
            "installed_relative_path": installed_relative,
            "git_blob": source_record["git_blob"],
            "bytes": len(payload),
            "sha256": sha256_bytes(payload),
        })
    return records


def official_codex_identity(agent_bin: pathlib.Path) -> dict[str, Any]:
    if sys.platform != "darwin":
        raise TrustError("terminal A5 recording requires macOS Seatbelt and signed Codex")
    if not agent_bin.is_absolute():
        raise TrustError("Codex executable path must be absolute")
    try:
        resolved = agent_bin.resolve(strict=True)
        metadata = resolved.lstat()
    except OSError as exc:
        raise TrustError("Codex executable is unavailable") from exc
    if (
        resolved.name != "codex" or not stat.S_ISREG(metadata.st_mode)
        or stat.S_ISLNK(metadata.st_mode) or not os.access(resolved, os.X_OK)
    ):
        raise TrustError("agent executable must resolve to one regular native codex binary")
    file_output = _text(_run_bytes(["/usr/bin/file", "-b", str(resolved)]))
    if "Mach-O 64-bit executable" not in file_output:
        raise TrustError("agent executable is not a native Mach-O Codex binary")
    _run_bytes(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(resolved)])
    details_result = _run_bytes(
        ["/usr/bin/codesign", "-d", "--verbose=6", str(resolved)], check=False
    )
    if details_result.returncode != 0:
        raise TrustError("Codex code-signing details are unavailable")
    details = details_result.stderr.decode("utf-8", errors="strict")
    fields: dict[str, list[str]] = {}
    for line in details.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            fields.setdefault(key, []).append(value)
    identifier = fields.get("Identifier", [""])[0]
    team_id = fields.get("TeamIdentifier", [""])[0]
    authorities = fields.get("Authority", [])
    cdhash = fields.get("CDHash", [""])[0]
    runtime = fields.get("Runtime Version", [""])[0]
    if (
        identifier != "codex" or team_id != OPENAI_TEAM_ID
        or OPENAI_AUTHORITY not in authorities
        or re.fullmatch(r"[0-9a-f]{40}", cdhash) is None
        or not runtime
    ):
        raise TrustError("Codex is not the hardened OpenAI Developer ID executable")
    requirement_result = _run_bytes(
        ["/usr/bin/codesign", "-d", "-r-", str(resolved)], check=False
    )
    if requirement_result.returncode != 0:
        raise TrustError("Codex designated requirement is unavailable")
    requirement = (
        requirement_result.stdout + requirement_result.stderr
    ).decode("utf-8", errors="strict")
    if (
        re.search(r'\bidentifier\s+(?:"codex"|codex)(?:\s|$)', requirement) is None
        or OPENAI_TEAM_ID not in requirement
    ):
        raise TrustError("Codex designated requirement does not bind OpenAI identity")
    version_result = _run_bytes([str(resolved), "--version"], timeout=10.0)
    version_output = _text(version_result)
    version_match = CODEX_VERSION_RE.fullmatch(version_output)
    if version_match is None:
        raise TrustError("Codex executable returned an unsupported version identity")
    help_result = _run_bytes([str(resolved), "exec", "--help"], timeout=10.0)
    help_text = help_result.stdout.decode("utf-8", errors="strict")
    required = {
        "--json", "--ignore-user-config", "--ignore-rules", "--skip-git-repo-check",
        "--sandbox", "--cd", "--output-last-message", "--output-schema",
    }
    missing = sorted(flag for flag in required if flag not in help_text)
    if missing:
        raise TrustError(f"signed Codex executable lacks required flags: {missing}")
    return {
        "path": str(resolved),
        "bytes": resolved.stat().st_size,
        "sha256": sha256_file(resolved, MAX_BINARY_BYTES),
        "version": version_output,
        "cli_version": version_match.group(1),
        "identifier": identifier,
        "team_identifier": team_id,
        "authority": OPENAI_AUTHORITY,
        "cdhash": cdhash,
        "runtime_version": runtime,
        "designated_requirement_sha256": sha256_bytes(requirement.encode("utf-8")),
        "file_identity": file_output,
    }


def codex_keychain_account(codex_home: pathlib.Path) -> str:
    codex_home = require_real_directory(codex_home)
    return f"cli|{hashlib.sha256(str(codex_home).encode('utf-8')).hexdigest()[:16]}"


def install_codex_keychain_auth(
    *, codex_home: pathlib.Path, agent_bin: pathlib.Path, auth_payload: bytes,
) -> dict[str, Any]:
    """Install one CODEX_HOME-scoped credential readable only by signed Codex."""

    if sys.platform != "darwin" or not pathlib.Path("/usr/bin/security").is_file():
        raise TrustError("Codex keychain staging requires macOS security(1)")
    codex_home = require_real_directory(codex_home)
    agent_bin = agent_bin.resolve(strict=True)
    if (codex_home / "auth.json").exists() or (codex_home / "auth.json").is_symlink():
        raise TrustError("keychain-backed Codex home must not contain auth.json")
    if (
        not auth_payload or len(auth_payload) > 1024 * 1024
        or any(item in auth_payload for item in (b"\x00", b"\n", b"\r"))
    ):
        raise TrustError("Codex keychain authentication payload is invalid or oversized")
    try:
        auth = json.loads(auth_payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise TrustError("Codex keychain authentication is not UTF-8 JSON") from exc
    if not isinstance(auth, dict) or not isinstance(auth.get("auth_mode"), str):
        raise TrustError("Codex keychain authentication has no typed auth mode")
    account = codex_keychain_account(codex_home)
    lookup = [
        "/usr/bin/security", "find-generic-password", "-s", CODEX_KEYCHAIN_SERVICE,
        "-a", account,
    ]
    if _run_bytes(lookup, check=False).returncode == 0:
        raise TrustError("CODEX_HOME keychain credential already exists")
    command = [
        "/usr/bin/security", "add-generic-password",
        "-s", CODEX_KEYCHAIN_SERVICE, "-a", account,
        "-l", f"Pulp A5 one-use {account}",
        "-T", str(agent_bin), "-T", "", "-w",
    ]
    installed = False
    try:
        _run_bytes(command, input_bytes=auth_payload + b"\n")
        installed = True
        if _run_bytes(lookup, check=False).returncode != 0:
            raise TrustError("one-use Codex keychain credential was not installed")
    except BaseException:
        if installed:
            _run_bytes([
                "/usr/bin/security", "delete-generic-password",
                "-s", CODEX_KEYCHAIN_SERVICE, "-a", account,
            ], check=False)
        raise
    return {
        "backend": "macos-keychain-direct",
        "service": CODEX_KEYCHAIN_SERVICE,
        "account": account,
        "auth_mode": auth["auth_mode"],
        "payload_bytes": len(auth_payload),
        "trusted_application": str(agent_bin),
        "creator_access_removed": True,
        "fallback_auth_file_absent": True,
    }


def remove_codex_keychain_auth(record: dict[str, Any]) -> None:
    if set(record) != {
        "backend", "service", "account", "auth_mode", "payload_bytes",
        "trusted_application", "creator_access_removed", "fallback_auth_file_absent",
    }:
        raise TrustError("Codex keychain record does not have its closed field set")
    lookup = [
        "/usr/bin/security", "find-generic-password", "-s", record["service"],
        "-a", record["account"],
    ]
    deleted = _run_bytes([
        "/usr/bin/security", "delete-generic-password", "-s", record["service"],
        "-a", record["account"],
    ], check=False)
    if deleted.returncode != 0 or _run_bytes(lookup, check=False).returncode == 0:
        raise TrustError("one-use Codex keychain credential could not be deleted")


def _sbpl_string(path: pathlib.Path) -> str:
    text = str(path)
    if "\x00" in text or "\n" in text or "\r" in text:
        raise TrustError("Seatbelt path contains unsupported control characters")
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def seatbelt_profile_for_paths(
    *, workspace: pathlib.Path, runtime_root: pathlib.Path,
    installed_prefix: pathlib.Path, agent_bin: pathlib.Path,
    denied_roots: Iterable[pathlib.Path], network_proxy_port: int,
) -> str:
    paths = [workspace, runtime_root, installed_prefix, agent_bin, *denied_roots]
    if (
        type(network_proxy_port) is not int or not 1 <= network_proxy_port <= 65535
        or any(
        not path.is_absolute() or any(part in {".", ".."} for part in path.parts)
        for path in paths
        )
    ):
        raise TrustError("Seatbelt paths must be absolute and normalized")
    denied = sorted({str(path) for path in paths[4:]})
    if not denied:
        raise TrustError("Seatbelt profile needs explicit denied read roots")
    readable_roots = [*SYSTEM_READ_ROOTS, workspace, runtime_root, installed_prefix, agent_bin]
    for denied_text in denied:
        denied_path = pathlib.Path(denied_text)
        if any(
            denied_path == readable
            or denied_path.is_relative_to(readable)
            or readable.is_relative_to(denied_path)
            for readable in readable_roots
        ):
            raise TrustError("Seatbelt readable roots overlap denied read roots")
    lines = [
        "(version 1)",
        "(allow default)",
        "(deny network-inbound network-bind network-outbound)",
        "(allow network-outbound",
        f'    (remote tcp "localhost:{network_proxy_port}")',
        ")",
        "(deny file-read*)",
        "(allow file-read*",
        '    (literal "/")',
        *(f"    (subpath {_sbpl_string(path)})" for path in readable_roots),
        ")",
        "(deny file-read*",
        *(f"    (subpath {_sbpl_string(pathlib.Path(item))})" for item in denied),
        ")",
        "(deny file-write*)",
        "(allow file-write*",
        f"    (subpath {_sbpl_string(workspace)})",
        f"    (subpath {_sbpl_string(runtime_root)})",
        '    (literal "/dev/null")',
        '    (literal "/dev/dtracehelper")',
        ")",
        "",
    ]
    return "\n".join(lines)


def seatbelt_profile(
    *, workspace: pathlib.Path, runtime_root: pathlib.Path,
    installed_prefix: pathlib.Path, agent_bin: pathlib.Path,
    denied_roots: Iterable[pathlib.Path], network_proxy_port: int,
) -> str:
    if sys.platform != "darwin" or not pathlib.Path("/usr/bin/sandbox-exec").is_file():
        raise TrustError("terminal A5 recording requires /usr/bin/sandbox-exec")
    workspace = require_real_directory(workspace)
    runtime_root = require_real_directory(runtime_root)
    installed_prefix = require_real_directory(installed_prefix)
    agent_bin = agent_bin.resolve(strict=True)
    denied = [require_real_directory(path) for path in denied_roots]
    return seatbelt_profile_for_paths(
        workspace=workspace,
        runtime_root=runtime_root,
        installed_prefix=installed_prefix,
        agent_bin=agent_bin,
        denied_roots=denied,
        network_proxy_port=network_proxy_port,
    )


def seatbelt_preflight(
    *, profile_path: pathlib.Path, workspace: pathlib.Path,
    denied_probes: dict[str, pathlib.Path], agent_bin: pathlib.Path,
    agent_version: str, installed_pulp: pathlib.Path, network_proxy_port: int,
    codex_home: pathlib.Path, agent_environment: dict[str, str],
) -> dict[str, Any]:
    profile_path = profile_path.resolve(strict=True)
    workspace = require_real_directory(workspace)
    if not denied_probes:
        raise TrustError("Seatbelt preflight requires denied read controls")
    prefix = ["/usr/bin/sandbox-exec", "-f", str(profile_path)]
    positive_read = _run_bytes(
        [*prefix, "/bin/cat", "TASK.md"], cwd=workspace, check=False,
        env=agent_environment,
    )
    if positive_read.returncode != 0:
        raise TrustError("Seatbelt positive workspace read control failed")
    positive_write = _run_bytes(
        [
            *prefix, "/bin/sh", "-c",
            "printf preflight > .a5-seatbelt-write && rm .a5-seatbelt-write",
        ],
        cwd=workspace,
        check=False,
        env=agent_environment,
    )
    if positive_write.returncode != 0 or (workspace / ".a5-seatbelt-write").exists():
        raise TrustError("Seatbelt positive workspace write/remove control failed")
    controls: list[dict[str, Any]] = [
        {"label": "workspace-read", "kind": "positive", "exit_code": 0},
        {"label": "workspace-write-remove", "kind": "positive", "exit_code": 0},
    ]
    codex_version = _run_bytes(
        [*prefix, str(agent_bin), "--version"], cwd=workspace, check=False,
        env=agent_environment,
    )
    codex_help = _run_bytes(
        [*prefix, str(agent_bin), "exec", "--help"], cwd=workspace, check=False,
        env=agent_environment,
    )
    pulp_version = _run_bytes(
        [*prefix, str(installed_pulp), "version"], cwd=workspace, check=False,
        env=agent_environment,
    )
    if (
        codex_version.returncode != 0
        or _text(codex_version) != agent_version
        or codex_help.returncode != 0
        or pulp_version.returncode != 0
    ):
        raise TrustError(
            "Seatbelt positive signed-agent or installed-CLI control failed "
            f"(codex-version={codex_version.returncode}, "
            f"version-match={_text(codex_version) == agent_version}, "
            f"codex-help={codex_help.returncode}, pulp-version={pulp_version.returncode})"
        )
    controls.extend([
        {"label": "signed-codex-version", "kind": "positive", "exit_code": 0},
        {"label": "signed-codex-exec-help", "kind": "positive", "exit_code": 0},
        {"label": "installed-pulp-version", "kind": "positive", "exit_code": 0},
    ])
    allowed_network = _run_bytes(
        [*prefix, "/usr/bin/nc", "-z", "-w", "1", "127.0.0.1", str(network_proxy_port)],
        cwd=workspace, check=False, env=agent_environment,
    )
    denied_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        denied_listener.bind(("127.0.0.1", 0))
        denied_listener.listen(1)
        denied_port = int(denied_listener.getsockname()[1])
        denied_network = _run_bytes(
            [*prefix, "/usr/bin/nc", "-z", "-w", "1", "127.0.0.1", str(denied_port)],
            cwd=workspace, check=False, env=agent_environment,
        )
    finally:
        denied_listener.close()
    if allowed_network.returncode != 0 or denied_network.returncode == 0:
        raise TrustError("Seatbelt exact loopback egress controls failed")
    controls.extend([
        {"label": "connect-proxy", "kind": "positive", "exit_code": 0},
        {
            "label": "non-allowlisted-loopback", "kind": "network-denied",
            "exit_code": denied_network.returncode,
        },
    ])
    login_status = _run_bytes(
        [
            *prefix, str(agent_bin), "-c", 'cli_auth_credentials_store="keyring"',
            "-c", "features.secret_auth_storage=false",
            "login", "status",
        ],
        cwd=workspace,
        check=False,
        env=agent_environment,
    )
    if (
        login_status.returncode != 0 or b"Logged in" not in login_status.stderr
        or (codex_home / "auth.json").exists() or (codex_home / "auth.json").is_symlink()
    ):
        raise TrustError("signed Codex could not read keychain-only authentication")
    controls.append({
        "label": "signed-codex-keychain-auth", "kind": "positive", "exit_code": 0,
    })
    for label, ambient_root in (
        ("ambient-usr-local", pathlib.Path("/usr/local")),
        ("ambient-site-library", pathlib.Path("/Library/Application Support")),
    ):
        ambient_root = require_real_directory(ambient_root)
        readable = _run_bytes(
            ["/bin/ls", str(ambient_root)], cwd=workspace, check=False,
            env=agent_environment,
        )
        denied_ambient = _run_bytes(
            [*prefix, "/bin/ls", str(ambient_root)], cwd=workspace, check=False,
            env=agent_environment,
        )
        if readable.returncode != 0 or denied_ambient.returncode == 0:
            raise TrustError(f"Seatbelt ambient read exclusion failed: {label}")
        controls.append({
            "label": label, "kind": "directory-read-denied",
            "exit_code": denied_ambient.returncode,
        })
    outside_read = workspace.parent / ".a5-seatbelt-outside-read"
    if outside_read.exists() or outside_read.is_symlink():
        raise TrustError("Seatbelt outside-read negative-control path already exists")
    try:
        descriptor = os.open(
            outside_read,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            0o600,
        )
        try:
            os.write(descriptor, b"non-allowlisted-read-control\n")
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        for kind, operand in (
            ("absolute-denied", str(outside_read)),
            ("relative-denied", os.path.relpath(outside_read, workspace)),
        ):
            denied_read = _run_bytes(
                [*prefix, "/bin/cat", operand], cwd=workspace, check=False
            )
            if denied_read.returncode == 0:
                raise TrustError("Seatbelt non-allowlisted read control unexpectedly succeeded")
            controls.append({
                "label": "non-allowlisted-read", "kind": kind,
                "exit_code": denied_read.returncode,
            })
    finally:
        try:
            outside_read.unlink()
        except FileNotFoundError:
            pass
    outside_write = workspace.parent / ".a5-seatbelt-outside-write"
    if outside_write.exists() or outside_write.is_symlink():
        raise TrustError("Seatbelt outside-write negative-control path already exists")
    denied_write = _run_bytes(
        [*prefix, "/usr/bin/touch", str(outside_write)], cwd=workspace, check=False
    )
    if denied_write.returncode == 0 or outside_write.exists() or outside_write.is_symlink():
        raise TrustError("Seatbelt non-allowlisted write control unexpectedly succeeded")
    controls.append({
        "label": "non-allowlisted-write", "kind": "write-denied",
        "exit_code": denied_write.returncode,
    })
    for label, probe in sorted(denied_probes.items()):
        probe = probe.resolve(strict=True)
        direct = _run_bytes([*prefix, "/bin/cat", str(probe)], cwd=workspace, check=False)
        if direct.returncode == 0:
            raise TrustError(f"Seatbelt direct denied-read control unexpectedly succeeded: {label}")
        relative_text = os.path.relpath(probe, workspace)
        relative = _run_bytes([*prefix, "/bin/cat", relative_text], cwd=workspace, check=False)
        if relative.returncode == 0:
            raise TrustError(f"Seatbelt relative traversal control unexpectedly succeeded: {label}")
        controls.extend([
            {"label": label, "kind": "absolute-denied", "exit_code": direct.returncode},
            {"label": label, "kind": "relative-denied", "exit_code": relative.returncode},
        ])
    symlink_path = workspace / ".a5-denied-read-symlink"
    if symlink_path.exists() or symlink_path.is_symlink():
        raise TrustError("Seatbelt symlink negative-control path already exists")
    target = next(iter(denied_probes.values())).resolve(strict=True)
    try:
        os.symlink(target, symlink_path)
        symlink = _run_bytes(
            [*prefix, "/bin/cat", symlink_path.name], cwd=workspace, check=False
        )
        if symlink.returncode == 0:
            raise TrustError("Seatbelt symlink traversal control unexpectedly succeeded")
        controls.append({
            "label": "denied-symlink", "kind": "symlink-denied",
            "exit_code": symlink.returncode,
        })
    finally:
        try:
            symlink_path.unlink()
        except FileNotFoundError:
            pass
    return {
        "schema": "pulp.gpu-clean-agent-seatbelt-preflight.v1",
        "profile_sha256": sha256_file(profile_path, 256 * 1024),
        "controls": controls,
        "passed": True,
    }


def create_record_keypair(case_dir: pathlib.Path) -> dict[str, Any]:
    case_dir = require_real_directory(case_dir)
    if sys.platform != "darwin" or not pathlib.Path("/usr/bin/openssl").is_file():
        raise TrustError("one-use A5 record signing requires /usr/bin/openssl on macOS")
    private_path = case_dir / "record-signing-key.pem"
    public_path = case_dir / "record-signing-public.pem"
    if any(path.exists() or path.is_symlink() for path in (private_path, public_path)):
        raise TrustError("record signing material already exists")
    _run_bytes([
        "/usr/bin/openssl", "genpkey", "-algorithm", "RSA",
        "-pkeyopt", "rsa_keygen_bits:3072", "-out", str(private_path),
    ])
    os.chmod(private_path, 0o600)
    _run_bytes([
        "/usr/bin/openssl", "pkey", "-in", str(private_path),
        "-pubout", "-out", str(public_path),
    ])
    os.chmod(public_path, 0o600)
    return {
        "algorithm": "rsa-3072-sha256",
        "private_key_path": str(private_path),
        "public_key_path": str(public_path),
        "public_key_sha256": sha256_file(public_path, 64 * 1024),
    }


def sign_record(core: dict[str, Any], private_path: pathlib.Path) -> dict[str, Any]:
    payload = json_bytes(core)
    result = _run_bytes(
        ["/usr/bin/openssl", "dgst", "-sha256", "-sign", str(private_path)],
        input_bytes=payload,
    )
    signature = result.stdout
    if not signature or len(signature) > 1024:
        raise TrustError("record signer returned an invalid signature")
    return {
        "algorithm": "rsa-3072-sha256",
        "signed_sha256": sha256_bytes(payload),
        "signature_base64": base64.b64encode(signature).decode("ascii"),
    }


def verify_record_signature(
    core: dict[str, Any], attestation: dict[str, Any], public_path: pathlib.Path,
) -> None:
    if set(attestation) != {"algorithm", "signed_sha256", "signature_base64"}:
        raise TrustError("record attestation does not have its closed field set")
    payload = json_bytes(core)
    if (
        attestation["algorithm"] != "rsa-3072-sha256"
        or attestation["signed_sha256"] != sha256_bytes(payload)
    ):
        raise TrustError("record attestation does not bind the canonical session")
    try:
        signature = base64.b64decode(attestation["signature_base64"], validate=True)
    except (ValueError, TypeError) as exc:
        raise TrustError("record signature is not canonical base64") from exc
    with tempfile.TemporaryDirectory(prefix="pulp-a5-signature-verify-") as raw:
        root = pathlib.Path(raw).resolve()
        signature_path = root / "signature.bin"
        payload_path = root / "session.json"
        signature_path.write_bytes(signature)
        payload_path.write_bytes(payload)
        verified = _run_bytes([
            "/usr/bin/openssl", "dgst", "-sha256", "-verify", str(public_path),
            "-signature", str(signature_path), str(payload_path),
        ], check=False)
    if verified.returncode != 0:
        raise TrustError("record signature verification failed")


def parse_codex_rollout(
    payload: bytes, *, thread_id: str, cli_version: str, workspace: pathlib.Path,
    model: str, run_nonce: str,
) -> dict[str, Any]:
    events: list[dict[str, Any]] = []
    for number, line in enumerate(payload.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            raise TrustError(f"Codex session JSONL line {number} is invalid") from exc
        if not isinstance(event, dict) or set(event) - {"timestamp", "type", "payload", "ordinal"}:
            raise TrustError("Codex session JSONL event has an unexpected envelope")
        if not isinstance(event.get("type"), str) or not isinstance(event.get("payload"), dict):
            raise TrustError("Codex session JSONL event is missing its typed payload")
        if (
            not isinstance(event.get("timestamp"), str) or not event["timestamp"]
            or type(event.get("ordinal")) is not int or event["ordinal"] != len(events)
        ):
            raise TrustError("Codex session JSONL timestamp or ordinal sequence is invalid")
        events.append(event)
        if len(events) > MAX_SESSION_EVENTS:
            raise TrustError("Codex session JSONL exceeds its event-count cap")
    if not events or events[0]["type"] != "session_meta":
        raise TrustError("Codex session JSONL must start with session_meta")
    if sum(event["type"] == "session_meta" for event in events) != 1:
        raise TrustError("Codex session JSONL must contain exactly one session_meta")
    meta = events[0]["payload"]
    if (
        meta.get("id") != thread_id or meta.get("session_id") not in {None, thread_id}
        or meta.get("cwd") != str(workspace) or meta.get("originator") != "codex_exec"
        or meta.get("source") != "exec" or meta.get("cli_version") != cli_version
    ):
        raise TrustError("Codex session metadata does not bind executable, thread, and cwd")
    contexts = [event["payload"] for event in events if event["type"] == "turn_context"]
    if len(contexts) != 1:
        raise TrustError("Codex session JSONL must contain exactly one turn context")
    context = contexts[0]
    if (
        context.get("cwd") != str(workspace) or context.get("model") != model
        or context.get("approval_policy") != "never"
        or context.get("sandbox_policy") != {"type": "danger-full-access"}
        or UUID_RE.fullmatch(str(context.get("turn_id", ""))) is None
    ):
        raise TrustError("Codex turn context does not bind model and outer-sandbox launch")
    event_messages = [event for event in events if event["type"] == "event_msg"]
    event_types = [event["payload"].get("type") for event in event_messages]
    task_started = [
        index for index, event in enumerate(events)
        if event["type"] == "event_msg" and event["payload"].get("type") == "task_started"
    ]
    task_completed = [
        index for index, event in enumerate(events)
        if event["type"] == "event_msg" and event["payload"].get("type") == "task_complete"
    ]
    task_turn_ids = {
        event["payload"].get("turn_id")
        for event in event_messages
        if event["payload"].get("type") in {"task_started", "task_complete"}
    }
    user_messages = [
        event["payload"]
        for event in events
        if event["type"] == "response_item"
        and event["payload"].get("type") == "message"
        and event["payload"].get("role") == "user"
    ]
    if (
        event_types.count("task_started") != 1
        or event_types.count("task_complete") != 1
        or task_started[0] >= task_completed[0]
        or task_turn_ids != {context["turn_id"]}
        or any(item in {"turn_aborted", "stream_error", "error"} for item in event_types)
    ):
        raise TrustError("Codex session JSONL does not contain one completed task")
    if (
        len(user_messages) != 1
        or run_nonce not in json.dumps(user_messages, sort_keys=True, separators=(",", ":"))
    ):
        raise TrustError("Codex session JSONL user prompt does not bind the fresh run nonce")
    corpus = json.dumps(events, sort_keys=True, separators=(",", ":"))
    if run_nonce not in corpus:
        raise TrustError("Codex session JSONL does not bind the fresh run nonce")
    return {
        "event_count": len(events),
        "turn_id": context["turn_id"],
        "sha256": sha256_bytes(payload),
        "bytes": len(payload),
    }
