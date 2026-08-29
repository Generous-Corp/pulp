#!/usr/bin/env python3
"""Record exact-head A2 all-four installed CLI/MCP and Forge acceptance."""

from __future__ import annotations

import argparse
import ctypes
import errno
import hashlib
import json
import os
import re
import secrets
import select
import shutil
import stat as stat_module
import subprocess
import sys
import tempfile
import time
import types
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent


def _load_local_source(module_name: str, filename: str) -> types.ModuleType:
    """Load a trusted sibling from source bytes without consulting ``.pyc``."""
    path = SCRIPT_DIR / filename
    data = path.read_bytes()
    if len(data) > 2 * 1024 * 1024:
        raise RuntimeError(f"local source module is unbounded: {filename}")
    module = types.ModuleType(module_name)
    module.__file__ = str(path)
    module.__package__ = ""
    sys.modules[module_name] = module
    exec(compile(data, str(path), "exec", dont_inherit=True), module.__dict__)
    return module


_load_local_source("json_schema_lite", "json_schema_lite.py")
_load_local_source("sdk_capability_handoff", "sdk_capability_handoff.py")
_load_local_source("sdk_provenance", "sdk_provenance.py")
provenance = _load_local_source(
    "gpu_trace_overhead_acceptance", "gpu_trace_overhead_acceptance.py"
)
verifier = _load_local_source(
    "verify_gpu_probe_acceptance", "verify_gpu_probe_acceptance.py"
)


RECIPES = {
    "compute": "gpu-compute.magnitude.v1",
    "stft": "gpu-audio.stft.v1",
    "renderer": "renderer3d.hardcoded-cube.v1",
    "threejs": "threejs.multi-pass.v1",
}
ADDITIONAL_PULP_PATH_CANARIES = {
    "gpu_audio": "stft",
    "threejs": "threejs",
}
SYSTEM_PATH = "/usr/bin:/bin:/usr/sbin:/sbin"
MAX_COMMAND_OUTPUT = 4 * 1024 * 1024
MCP_RESPONSE_TIMEOUT_SECONDS = 310
BINARY_PATHS = {
    "installed_rust_cli": (Path("pulp"), Path("bin/pulp")),
    "installed_cpp_delegate": (
        Path("tools/cli/pulp-cpp"), Path("bin/pulp-cpp")
    ),
    "installed_mcp": (Path("tools/mcp/pulp-mcp"), Path("bin/pulp-mcp")),
}


class AcceptanceError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_descriptor(descriptor: int) -> str:
    digest = hashlib.sha256()
    metadata = os.fstat(descriptor)
    offset = 0
    while offset < metadata.st_size:
        chunk = os.pread(descriptor, min(1024 * 1024, metadata.st_size - offset), offset)
        if not chunk:
            raise AcceptanceError("executable descriptor ended before its claimed size")
        digest.update(chunk)
        offset += len(chunk)
    if os.fstat(descriptor).st_size != metadata.st_size:
        raise AcceptanceError("executable size changed while hashing its descriptor")
    return digest.hexdigest()


def run_bounded(
    command: list[str], *, cwd: Path, environment: dict[str, str], timeout: int,
    directory_claim: Any | None = None,
) -> subprocess.CompletedProcess[str]:
    if directory_claim is not None:
        directory_claim.assert_current()
    try:
        completed = subprocess.run(
            command, cwd=cwd, env=environment, check=False,
            capture_output=True, text=True, timeout=timeout,
        )
    finally:
        if directory_claim is not None:
            directory_claim.assert_current()
    if len(completed.stdout.encode()) + len(completed.stderr.encode()) > MAX_COMMAND_OUTPUT:
        raise AcceptanceError(f"command exceeded bounded output: {command[0]}")
    return completed


def cmake_cache(build_dir: Path) -> dict[str, str]:
    path = build_dir / "CMakeCache.txt"
    try:
        payload = path.read_text(encoding="utf-8")
    except OSError as error:
        raise AcceptanceError(f"cannot read CMake cache {path}: {error}") from error
    if len(payload.encode()) > 4 * 1024 * 1024:
        raise AcceptanceError("CMake cache exceeds 4 MiB")
    values: dict[str, str] = {}
    for line in payload.splitlines():
        match = re.match(r"^([^#/:=]+)(?::[^=]+)?=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def bounded_descriptor_bytes(descriptor: int, limit: int, label: str) -> bytes:
    metadata = os.fstat(descriptor)
    if metadata.st_size > limit:
        raise AcceptanceError(f"{label} exceeds its byte limit")
    chunks: list[bytes] = []
    offset = 0
    while offset < metadata.st_size:
        chunk = os.pread(
            descriptor, min(1024 * 1024, metadata.st_size - offset), offset
        )
        if not chunk:
            raise AcceptanceError(f"{label} ended before its claimed size")
        chunks.append(chunk)
        offset += len(chunk)
    if os.fstat(descriptor).st_size != metadata.st_size:
        raise AcceptanceError(f"{label} size changed while reading")
    return b"".join(chunks)


def parse_cmake_cache_bytes(data: bytes, label: str) -> dict[str, str]:
    try:
        payload = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise AcceptanceError(f"{label} is not UTF-8") from error
    values: dict[str, str] = {}
    for line in payload.splitlines():
        match = re.match(r"^([^#/:=]+)(?::[^=]+)?=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def require_release_pulp_build(build_dir: Path, repository: Path) -> dict[str, str]:
    values = cmake_cache(build_dir)
    try:
        home = Path(values["CMAKE_HOME_DIRECTORY"]).resolve()
    except KeyError as error:
        raise AcceptanceError("Pulp CMake cache lacks CMAKE_HOME_DIRECTORY") from error
    if home != repository.resolve() or values.get("CMAKE_BUILD_TYPE") != "Release":
        raise AcceptanceError("Pulp build must be a single-config Release of the exact source")
    requirements = {
        "PULP_ENABLE_GPU": "ON",
        "PULP_ENABLE_SCENE3D": "ON",
        "PULP_ENABLE_THREEJS_RUNTIME": "ON",
        "PULP_ENABLE_JS": "ON",
        "PULP_JS_ENGINE": "v8",
        "PULP_BUILD_RUST_CLI": "ON",
        "PULP_RUST_CLI_PROFILE": "release",
        "PULP_HAS_THREEJS": "TRUE",
    }
    for key, expected in requirements.items():
        if values.get(key) != expected:
            raise AcceptanceError(f"Pulp build requires {key}={expected}")
    return values


def directory_open_flags() -> int:
    return (
        os.O_RDONLY
        | getattr(os, "O_DIRECTORY", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )


def directory_identity(value: os.stat_result, label: str) -> dict[str, int]:
    if not stat_module.S_ISDIR(value.st_mode):
        raise AcceptanceError(f"{label} identity is not a directory")
    return {"device": value.st_dev, "inode": value.st_ino}


def assert_directory_claim(
    path: Path, expected: dict[str, int], label: str, descriptor: int | None = None
) -> None:
    try:
        path_identity = directory_identity(
            os.stat(path, follow_symlinks=False), label
        )
        descriptor_identity = (
            directory_identity(os.fstat(descriptor), label)
            if descriptor is not None else expected
        )
    except OSError as error:
        raise AcceptanceError(f"{label} claim is no longer reachable") from error
    if path_identity != expected or descriptor_identity != expected:
        raise AcceptanceError(f"{label} path no longer names its claimed directory")


class RetainedDirectoryClaim:
    def __init__(
        self,
        path: Path,
        descriptor: int,
        identity: dict[str, int],
        label: str,
        *,
        watch_links: bool = True,
    ):
        self.path = path
        self.descriptor = descriptor
        self.identity = identity
        self.label = label
        self.closed = False
        self.watch_links = watch_links
        self.file_claims: list[dict[str, Any]] = []
        self.ancestor_claims: list[dict[str, Any]] = []
        self.claimed_directory_identities = {
            (identity["device"], identity["inode"])
        }
        self.monitor: Any | None = None
        self._bind_ancestor_chain(path)

    def _file_vnode_flags(self) -> int:
        required = [
            "KQ_NOTE_DELETE", "KQ_NOTE_WRITE", "KQ_NOTE_EXTEND",
            "KQ_NOTE_ATTRIB", "KQ_NOTE_RENAME", "KQ_NOTE_REVOKE",
        ]
        if self.watch_links:
            required.append("KQ_NOTE_LINK")
        if not hasattr(select, "kqueue") or any(
            not hasattr(select, name) for name in required
        ):
            raise AcceptanceError("exact file sealing requires macOS kqueue")
        return sum(getattr(select, name) for name in required)

    @staticmethod
    def _directory_vnode_flags() -> int:
        required = ("KQ_NOTE_DELETE", "KQ_NOTE_RENAME", "KQ_NOTE_REVOKE")
        if any(not hasattr(select, name) for name in required):
            raise AcceptanceError("exact path sealing requires macOS kqueue")
        return sum(getattr(select, name) for name in required)

    def _register_monitor(self, descriptor: int, *, is_file: bool) -> None:
        assert self.monitor is not None
        event = select.kevent(
            descriptor,
            filter=select.KQ_FILTER_VNODE,
            flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
            fflags=(
                self._file_vnode_flags() if is_file
                else self._directory_vnode_flags()
            ),
        )
        self.monitor.control([event], 0, 0)

    def _bind_ancestor_chain(self, path: Path) -> None:
        current = path.resolve().parent
        while True:
            descriptor = os.open(current, directory_open_flags())
            identity = directory_identity(os.fstat(descriptor), "path-ancestor")
            key = (identity["device"], identity["inode"])
            if key in self.claimed_directory_identities:
                os.close(descriptor)
            else:
                self.claimed_directory_identities.add(key)
                claim = {
                    "path": current,
                    "descriptor": descriptor,
                    "identity": identity,
                }
                self.ancestor_claims.append(claim)
                if self.monitor is not None:
                    self._register_monitor(descriptor, is_file=False)
            if current.parent == current:
                break
            current = current.parent

    def seal(self) -> None:
        if self.monitor is not None:
            raise AcceptanceError(f"{self.label} claim is already sealed")
        self.monitor = select.kqueue()
        self._register_monitor(self.descriptor, is_file=False)
        for claim in self.ancestor_claims:
            self._register_monitor(claim["descriptor"], is_file=False)
        for claim in self.file_claims:
            self._register_monitor(claim["descriptor"], is_file=True)
        self._assert_no_mutation_events()

    def _assert_no_mutation_events(self) -> None:
        if self.monitor is not None:
            events = self.monitor.control(None, 64, 0)
            if events:
                raise AcceptanceError(
                    f"{self.label} or a retained file had a mutation event"
                )

    def bind_file(
        self, path: Path, label: str, expected_sha256: str | None = None
    ) -> str:
        self._bind_ancestor_chain(path)
        descriptor = os.open(
            path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
        )
        claim: dict[str, Any] | None = None
        try:
            metadata = os.fstat(descriptor)
            named = os.stat(path, follow_symlinks=False)
            identity = {"device": metadata.st_dev, "inode": metadata.st_ino}
            if (
                not stat_module.S_ISREG(metadata.st_mode)
                or {"device": named.st_dev, "inode": named.st_ino} != identity
            ):
                raise AcceptanceError(f"{label} does not match its exact file claim")
            claim = {
                "path": path,
                "descriptor": descriptor,
                "identity": identity,
                "sha256": "",
                "bytes": metadata.st_size,
                "label": label,
            }
            self.file_claims.append(claim)
            if self.monitor is not None:
                self._register_monitor(descriptor, is_file=True)
                self._assert_no_mutation_events()
            actual_sha256 = sha256_descriptor(descriptor)
            if expected_sha256 is not None and actual_sha256 != expected_sha256:
                raise AcceptanceError(f"{label} does not match its exact file claim")
            claim["sha256"] = actual_sha256
            named = os.stat(path, follow_symlinks=False)
            if {"device": named.st_dev, "inode": named.st_ino} != identity:
                raise AcceptanceError(f"{label} does not match its exact file claim")
            self._assert_no_mutation_events()
        except BaseException:
            if claim is not None:
                self.file_claims.remove(claim)
            os.close(descriptor)
            raise
        return actual_sha256

    def assert_current(self) -> None:
        if self.closed:
            raise AcceptanceError(f"{self.label} claim closed before proof completion")
        self._assert_no_mutation_events()
        assert_directory_claim(self.path, self.identity, self.label, self.descriptor)
        for claim in self.ancestor_claims:
            assert_directory_claim(
                claim["path"], claim["identity"], "path-ancestor",
                claim["descriptor"],
            )
        for claim in self.file_claims:
            metadata = os.fstat(claim["descriptor"])
            named = os.stat(claim["path"], follow_symlinks=False)
            identity = {"device": metadata.st_dev, "inode": metadata.st_ino}
            if (
                not stat_module.S_ISREG(metadata.st_mode)
                or identity != claim["identity"]
                or {"device": named.st_dev, "inode": named.st_ino} != identity
                or sha256_descriptor(claim["descriptor"]) != claim["sha256"]
            ):
                raise AcceptanceError(
                    f"{claim['label']} no longer matches its retained file claim"
                )
        self._assert_no_mutation_events()

    def close(self) -> None:
        if not self.closed:
            if self.monitor is not None:
                self.monitor.close()
            for claim in self.file_claims:
                os.close(claim["descriptor"])
            for claim in self.ancestor_claims:
                os.close(claim["descriptor"])
            os.close(self.descriptor)
            self.closed = True

    def __del__(self) -> None:
        self.close()


def renameat_no_replace(
    source_parent: int, source_name: str, destination_parent: int,
    destination_name: str,
) -> None:
    """Publish one relative path with macOS RENAME_EXCL semantics."""
    libc = ctypes.CDLL(None, use_errno=True)
    renameatx = getattr(libc, "renameatx_np", None)
    if renameatx is None:
        raise AcceptanceError("fresh directory claims require macOS renameatx_np")
    renameatx.argtypes = (
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
        ctypes.c_uint,
    )
    renameatx.restype = ctypes.c_int
    if renameatx(
        source_parent, os.fsencode(source_name), destination_parent,
        os.fsencode(destination_name), 0x00000004,
    ) != 0:
        value = ctypes.get_errno()
        raise OSError(value, os.strerror(value), destination_name)


def claim_fresh_directory(path: Path, label: str) -> RetainedDirectoryClaim:
    """Create, retain, and no-replace publish the exact directory inode."""
    if path.name in {"", ".", ".."}:
        raise AcceptanceError(f"{label} must have a safe final path component")
    parent_descriptor = os.open(path.parent, directory_open_flags())
    temporary_name = f".{path.name}.{secrets.token_hex(16)}.claim"
    temporary_descriptor: int | None = None
    temporary_identity: dict[str, int] | None = None
    temporary_exists = False
    try:
        parent_identity = directory_identity(
            os.fstat(parent_descriptor), f"{label}-parent"
        )
        assert_directory_claim(
            path.parent, parent_identity, f"{label}-parent", parent_descriptor
        )
        os.mkdir(temporary_name, mode=0o700, dir_fd=parent_descriptor)
        temporary_exists = True
        temporary_descriptor = os.open(
            temporary_name, directory_open_flags(), dir_fd=parent_descriptor
        )
        temporary_identity = directory_identity(
            os.fstat(temporary_descriptor), label
        )
        named = directory_identity(
            os.stat(
                temporary_name,
                dir_fd=parent_descriptor,
                follow_symlinks=False,
            ),
            label,
        )
        if named != temporary_identity:
            raise AcceptanceError(f"{label} staging inode changed before publication")
        renameat_no_replace(
            parent_descriptor, temporary_name,
            parent_descriptor, path.name,
        )
        temporary_exists = False
        assert_directory_claim(
            path.parent, parent_identity, f"{label}-parent", parent_descriptor
        )
        published = directory_identity(
            os.stat(path.name, dir_fd=parent_descriptor, follow_symlinks=False),
            label,
        )
        if published != temporary_identity:
            raise AcceptanceError(
                f"{label} publication did not preserve its created inode"
            )
        os.fsync(parent_descriptor)
        claim = RetainedDirectoryClaim(
            path, temporary_descriptor, temporary_identity, label
        )
        temporary_descriptor = None
        try:
            claim.seal()
            claim.assert_current()
            return claim
        except BaseException:
            claim.close()
            raise
    except OSError as error:
        if error.errno == errno.EEXIST:
            raise AcceptanceError(
                f"{label} was not fresh at no-replace publication"
            ) from error
        raise AcceptanceError(
            f"{label} could not be atomically claimed: {error}"
        ) from error
    finally:
        if temporary_descriptor is not None:
            os.close(temporary_descriptor)
        if temporary_exists and temporary_identity is not None:
            try:
                named = directory_identity(
                    os.stat(
                        temporary_name,
                        dir_fd=parent_descriptor,
                        follow_symlinks=False,
                    ),
                    label,
                )
                if named == temporary_identity:
                    os.rmdir(temporary_name, dir_fd=parent_descriptor)
                    os.fsync(parent_descriptor)
            except OSError:
                pass
        os.close(parent_descriptor)


def retain_existing_directory(path: Path, label: str) -> RetainedDirectoryClaim:
    descriptor = os.open(path, directory_open_flags())
    claim: RetainedDirectoryClaim | None = None
    try:
        claim = RetainedDirectoryClaim(
            path, descriptor,
            directory_identity(os.fstat(descriptor), label), label,
        )
        descriptor = -1
        claim.seal()
        claim.assert_current()
        return claim
    except BaseException:
        if claim is not None:
            claim.close()
        elif descriptor >= 0:
            os.close(descriptor)
        raise


def bind_build_outputs(
    build_dir: Path, claim: RetainedDirectoryClaim
) -> dict[str, dict[str, Any]]:
    outputs: dict[str, dict[str, Any]] = {}
    for role, (build_relative, _installed_relative) in BINARY_PATHS.items():
        path = build_dir / build_relative
        if not path.is_file() or path.is_symlink() or not os.access(path, os.X_OK):
            raise AcceptanceError(f"{role} build output is not a regular executable")
        digest = claim.bind_file(path, f"{role} build output")
        retained = claim.file_claims[-1]
        outputs[role] = {
            "sha256": digest,
            "bytes": retained["bytes"],
            "identity": retained["identity"],
        }
    claim.assert_current()
    return outputs


def bind_installed_outputs(
    prefix: Path,
    build_outputs: dict[str, dict[str, Any]],
    claim: RetainedDirectoryClaim,
) -> dict[str, dict[str, Any]]:
    binaries: dict[str, dict[str, Any]] = {}
    for role, (_build_relative, installed_relative) in BINARY_PATHS.items():
        path = prefix / installed_relative
        if not path.is_file() or path.is_symlink() or not os.access(path, os.X_OK):
            raise AcceptanceError(f"{role} is not a regular refreshed executable")
        try:
            installed_digest = claim.bind_file(
                path, role, build_outputs[role]["sha256"]
            )
        except AcceptanceError as error:
            raise AcceptanceError(
                f"{role} installed bytes differ from the retained build output"
            ) from error
        retained = claim.file_claims[-1]
        binaries[role] = {
            "sha256": installed_digest,
            "bytes": retained["bytes"],
            "build_output_sha256": build_outputs[role]["sha256"],
            "build_output_bytes": build_outputs[role]["bytes"],
        }
    if binaries["installed_rust_cli"]["sha256"] == binaries[
        "installed_cpp_delegate"
    ]["sha256"]:
        raise AcceptanceError("installed pulp is a C++ copy, not the required Rust front")
    claim.assert_current()
    return binaries


def refresh_install(
    repository: Path, build_dir: Path, prefix: Path, build_environment: dict[str, str]
) -> tuple[
    dict[str, Any], dict[str, dict[str, Any]], RetainedDirectoryClaim,
    provenance.RetainedTreeClaim, provenance.RetainedTreeClaim,
    provenance.RetainedClaimSet,
]:
    require_release_pulp_build(build_dir, repository)
    retained_claim = claim_fresh_directory(prefix, "install-prefix")
    prefix_claim = retained_claim.identity
    source_claim: provenance.RetainedTreeClaim | None = None
    build_input_claim: provenance.RetainedTreeClaim | None = None
    provider_input_claim: provenance.RetainedClaimSet | None = None
    try:
        revision = provenance._git_text(repository, "rev-parse", "HEAD")
        source_claim, source_tree_evidence = provenance.retain_git_source_tree(
            repository, revision, "Pulp exact source"
        )
        prebuild_claim = provenance.CombinedClaims(retained_claim, source_claim)
        reconfigure = run_bounded(
            ["cmake", "-S", str(repository), "-B", str(build_dir)],
            cwd=repository, environment=build_environment, timeout=1800,
            directory_claim=prebuild_claim,
        )
        if reconfigure.returncode != 0:
            raise AcceptanceError(
                f"Pulp exact-head reconfigure failed: {reconfigure.stderr[-2000:]}"
            )
        require_release_pulp_build(build_dir, repository)
        try:
            build_input_claim, build_input_evidence = (
                provenance.retain_generated_build_inputs(
                    build_dir, "Pulp regenerated build inputs"
                )
            )
        except ValueError as error:
            raise AcceptanceError(str(error)) from error
        try:
            provider_input_claim, provider_input_evidence = (
                provenance.retain_resolved_render_provider_inputs(
                    build_dir, "Pulp resolved render providers"
                )
            )
        except ValueError as error:
            raise AcceptanceError(str(error)) from error
        compilation_claim = provenance.CombinedClaims(
            retained_claim, source_claim, build_input_claim, provider_input_claim
        )
        clean = run_bounded(
            ["cmake", "--build", str(build_dir), "--target", "clean"],
            cwd=repository, environment=build_environment, timeout=1800,
            directory_claim=compilation_claim,
        )
        if clean.returncode != 0:
            raise AcceptanceError(
                f"Pulp forced-clean refresh failed: {clean.stderr[-2000:]}"
            )
        cargo_target = build_dir / "experimental/pulp-rs/cargo-target"
        if cargo_target.is_symlink():
            raise AcceptanceError("Pulp Cargo target cache must not be a symlink")
        if cargo_target.exists():
            cargo_clean = run_bounded(
                ["cmake", "-E", "remove_directory", str(cargo_target)],
                cwd=repository, environment=build_environment, timeout=1800,
                directory_claim=compilation_claim,
            )
            if cargo_clean.returncode != 0:
                raise AcceptanceError(
                    f"Pulp Cargo forced-clean failed: {cargo_clean.stderr[-2000:]}"
                )
        if cargo_target.exists() or any(
            (build_dir / relative).exists()
            for relative, _installed in BINARY_PATHS.values()
        ):
            raise AcceptanceError(
                "forced-clean Pulp build retained a measured target output"
            )
        build = run_bounded(
            ["cmake", "--build", str(build_dir), "--target",
             "pulp-rust-cli", "pulp-cli", "pulp-mcp", "--parallel"],
            cwd=repository, environment=build_environment, timeout=3600,
            directory_claim=compilation_claim,
        )
        if build.returncode != 0:
            raise AcceptanceError(f"Pulp CLI/MCP refresh failed: {build.stderr[-2000:]}")
        source_claim.assert_full()
        build_input_claim.assert_full()
        provider_input_claim.assert_full()
        cache_digest = retained_claim.bind_file(
            build_dir / "CMakeCache.txt", "refreshed CMake cache"
        )
        cache = require_release_pulp_build(build_dir, repository)
        build_outputs = bind_build_outputs(build_dir, retained_claim)
        install = run_bounded(
            ["cmake", "--install", str(build_dir), "--prefix", str(prefix)],
            cwd=repository, environment=build_environment, timeout=1800,
            directory_claim=compilation_claim,
        )
        if install.returncode != 0:
            raise AcceptanceError(f"Pulp install refresh failed: {install.stderr[-2000:]}")
        build_info_digest = retained_claim.bind_file(
            prefix / "include/pulp/runtime/build_info.hpp",
            "installed build_info.hpp",
        )
        binaries = bind_installed_outputs(prefix, build_outputs, retained_claim)
        installed_identity = provenance.installed_source_identity(
            repository, revision, prefix / "bin/pulp", prefix / "bin/pulp-mcp"
        )
        if installed_identity["build_info_sha256"] != build_info_digest:
            raise AcceptanceError("installed build_info differs from its retained claim")
        retained_claim.assert_current()
        return ({
            "cmake_cache_sha256": cache_digest,
            "cmake_home_revision": revision,
            "cmake_build_type": cache["CMAKE_BUILD_TYPE"],
            "rust_profile": cache["PULP_RUST_CLI_PROFILE"],
            "feature_contract": {
                key: cache[key] for key in (
                    "PULP_ENABLE_GPU", "PULP_ENABLE_SCENE3D",
                    "PULP_ENABLE_THREEJS_RUNTIME", "PULP_ENABLE_JS",
                    "PULP_JS_ENGINE", "PULP_BUILD_RUST_CLI", "PULP_HAS_THREEJS",
                )
            },
            "source_tree_claim": source_tree_evidence,
            "build_input_claim": build_input_evidence,
            "render_provider_input_claim": provider_input_evidence,
            "build_info": installed_identity["build_info"],
            "build_info_sha256": build_info_digest,
            "install_prefix_initial_state": "absent-and-atomically-claimed",
            "install_prefix_claim_method": (
                "unpredictable-staging-directory-renameatx-noreplace-retained-fd-v1"
            ),
            "install_prefix_claim": prefix_claim,
            "build_install_binary_identity": "pass",
        }, binaries, retained_claim, source_claim, build_input_claim,
            provider_input_claim)
    except BaseException:
        if provider_input_claim is not None:
            provider_input_claim.close()
        if build_input_claim is not None:
            build_input_claim.close()
        if source_claim is not None:
            source_claim.close()
        retained_claim.close()
        raise


def canonical_result(value: dict[str, Any]) -> dict[str, Any]:
    copied = json.loads(json.dumps(value))
    copied.pop("gpu_evidence_id", None)
    return copied


def bind_additional_pulp_path_canaries(
    cli_results: dict[str, dict[str, dict[str, Any]]],
    mcp_results: dict[str, dict[str, dict[str, Any]]],
) -> dict[str, dict[str, Any]]:
    """Bind plan-allowed Pulp path canaries to the A2 rows that execute them."""
    canaries: dict[str, dict[str, Any]] = {}
    for canary, group in ADDITIONAL_PULP_PATH_CANARIES.items():
        cli = cli_results.get(group, {})
        mcp = mcp_results.get(group, {})
        recipe = RECIPES[group]
        if (
            cli.get("run1", {}).get("verdict") != "pass"
            or cli.get("run2", {}).get("verdict") != "pass"
            or cli.get("negative", {}).get("verdict") != "fail"
            or mcp.get("positive", {}).get("verdict") != "pass"
            or mcp.get("negative", {}).get("verdict") != "fail"
            or canonical_result(cli.get("run1", {}))
            != canonical_result(mcp.get("positive", {}))
            or canonical_result(cli.get("negative", {}))
            != canonical_result(mcp.get("negative", {}))
        ):
            raise AcceptanceError(f"{canary} Pulp path canary was not actually exercised")
        group_index = list(RECIPES).index(group)
        canaries[canary] = {
            "status": "pass",
            "recipe": recipe,
            "cli_positive_files": [f"{group}-run1.json", f"{group}-run2.json"],
            "cli_negative_file": f"{group}-negative.json",
            "mcp_positive_response_id": 2 + (2 * group_index),
            "mcp_negative_response_id": 3 + (2 * group_index),
        }
    return canaries


def run_cli_probe(
    cli: Path,
    recipe: str,
    artifacts: Path,
    *,
    negative: bool,
    cwd: Path,
    environment: dict[str, str],
    directory_claim: RetainedDirectoryClaim,
) -> dict[str, Any]:
    command = [str(cli), "gpu", "probe", "--recipe", recipe,
               "--artifacts", str(artifacts), "--json"]
    if negative:
        command.append("--negative-control")
    completed = run_bounded(
        command, cwd=cwd, environment=environment, timeout=300,
        directory_claim=directory_claim,
    )
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise AcceptanceError(f"installed CLI returned malformed probe JSON: {error}") from error
    expected_exit = 1 if negative else 0
    expected_verdict = "fail" if negative else "pass"
    if (
        completed.returncode != expected_exit or result.get("verdict") != expected_verdict
        or result.get("recipe_id") != recipe
        or bool(result.get("mutation")) != negative
    ):
        raise AcceptanceError(f"installed CLI did not preserve {recipe} typed status")
    return result


class McpSession:
    def __init__(
        self, executable: Path, cwd: Path, environment: dict[str, str],
        directory_claim: RetainedDirectoryClaim,
    ):
        self.directory_claim = directory_claim
        self.directory_claim.assert_current()
        self.process = subprocess.Popen(
            [str(executable)], cwd=cwd, env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self.directory_claim.assert_current()
        self.next_id = 1
        self.transcript: list[dict[str, Any]] = []
        self._stdout_buffer = bytearray()
        self._terminated_for_timeout = False

    def _terminate_bounded(self) -> None:
        if self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)

    def request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        self.directory_claim.assert_current()
        request_id = self.next_id
        self.next_id += 1
        request: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            request["params"] = params
        assert self.process.stdin is not None and self.process.stdout is not None
        self.process.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        try:
            line = provenance.read_bounded_process_line(
                self.process,
                self.process.stdout,
                self._stdout_buffer,
                timeout_seconds=MCP_RESPONSE_TIMEOUT_SECONDS,
                maximum_bytes=provenance.MCP_RESPONSE_LIMIT_BYTES,
                label="installed MCP response",
            )
        except (TimeoutError, ValueError) as error:
            self._terminated_for_timeout = isinstance(error, TimeoutError)
            self._terminate_bounded()
            raise AcceptanceError(str(error)) from error
        try:
            response = json.loads(line)
        except json.JSONDecodeError as error:
            raise AcceptanceError("installed MCP returned malformed JSON") from error
        self.directory_claim.assert_current()
        if (
            not isinstance(response, dict)
            or type(response.get("id")) is not int
            or response["id"] != request_id
            or not isinstance(response.get("result"), dict)
        ):
            raise AcceptanceError("installed MCP returned an incoherent response")
        self.transcript.append(response)
        return response

    def initialize(self) -> None:
        response = self.request("initialize")
        if response["result"].get("protocolVersion") != "2024-11-05":
            raise AcceptanceError("installed MCP protocol version drifted")
        assert self.process.stdin is not None
        self.process.stdin.write(
            '{"jsonrpc":"2.0","method":"notifications/initialized"}\n'
        )
        self.process.stdin.flush()

    def probe(self, recipe: str, artifacts: Path, *, negative: bool) -> dict[str, Any]:
        response = self.request("tools/call", {
            "name": "pulp_gpu_probe",
            "arguments": {
                "recipe": recipe, "artifacts": str(artifacts),
                "negative_control": negative,
            },
        })
        result = response["result"]
        structured = result.get("structuredContent")
        if not isinstance(structured, dict) or not isinstance(structured.get("evidence"), dict):
            raise AcceptanceError("installed MCP lacks structured probe evidence")
        evidence = structured["evidence"]
        expected_exit = 1 if negative else 0
        if structured.get("exit_code") != expected_exit or bool(result.get("isError", False)) != negative:
            raise AcceptanceError("installed MCP did not preserve typed probe status")
        try:
            text_evidence = json.loads(result["content"][0]["text"])
        except (IndexError, KeyError, TypeError, json.JSONDecodeError) as error:
            raise AcceptanceError("installed MCP text evidence is malformed") from error
        if text_evidence != evidence:
            raise AcceptanceError("installed MCP text and structured probe evidence differ")
        return evidence

    def close(self) -> None:
        if self.process.stdin:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._terminate_bounded()
        stderr = self.process.stderr.read() if self.process.stderr else ""
        if self.process.stdout:
            self.process.stdout.close()
        if self.process.stderr:
            self.process.stderr.close()
        if (
            (self.process.returncode != 0 and not self._terminated_for_timeout)
            or len(stderr.encode()) > MAX_COMMAND_OUTPUT
        ):
            raise AcceptanceError(f"installed MCP exited abnormally: {stderr[-1000:]}")
        self.directory_claim.assert_current()


def forge_source_identity(repository: Path, pulp_revision: str) -> dict[str, Any]:
    if provenance._git_text(repository, "rev-parse", "HEAD") != verifier.EXPECTED_FORGE_REVISION:
        raise AcceptanceError("Forge checkout is not the accepted exact revision")
    origin = provenance._git_text(repository, "config", "--get", "remote.origin.url")
    if not re.fullmatch(
        r"(?:git@github\.com:|https://github\.com/)Generous-Corp/forge(?:\.git)?", origin
    ):
        raise AcceptanceError("Forge checkout does not use the canonical origin")
    staged = provenance._git_text(repository, "diff", "--cached", "--name-only")
    changed = provenance._git_text(repository, "diff", "--name-only")
    untracked = provenance._git_text(repository, "ls-files", "--others", "--exclude-standard")
    if staged or changed.splitlines() != ["PULP_SDK_REF"] or untracked:
        raise AcceptanceError("Forge may differ only by an unstaged PULP_SDK_REF overlay")
    ref = (repository / "PULP_SDK_REF").read_text(encoding="utf-8").strip()
    if ref != pulp_revision:
        raise AcceptanceError("Forge PULP_SDK_REF overlay does not name this Pulp revision")
    original_blob = provenance._git_text(
        repository, "rev-parse", f"{verifier.EXPECTED_FORGE_REVISION}:PULP_SDK_REF"
    )
    return {
        "repository": "Generous-Corp/forge",
        "revision": verifier.EXPECTED_FORGE_REVISION,
        "pulp_sdk_ref_overlay": {
            "path": "PULP_SDK_REF", "content": ref, "original_blob": original_blob,
        },
        "all_other_tracked_files_clean": True,
    }


def parse_forge_stamp_bytes(data: bytes) -> dict[str, str]:
    if len(data) > 16 * 1024:
        raise AcceptanceError("FORGE_BUILD_INFO exceeds 16 KiB")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise AcceptanceError("FORGE_BUILD_INFO is not UTF-8") from error
    values: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line:
            raise AcceptanceError("FORGE_BUILD_INFO contains a malformed line")
        key, value = line.split("=", 1)
        if not re.fullmatch(r"[a-z_]+", key) or key in values:
            raise AcceptanceError("FORGE_BUILD_INFO contains duplicate/invalid keys")
        values[key] = value
    required = {"schema", "version", "packaged", "product", "product_id",
                "role", "format", "build", "pulp_sdk"}
    if not required.issubset(values):
        raise AcceptanceError("FORGE_BUILD_INFO lacks required identity fields")
    return values


def parse_forge_stamp(path: Path) -> dict[str, str]:
    return parse_forge_stamp_bytes(path.read_bytes())


def prove_forge(
    repository: Path,
    build_dir: Path,
    prefix: Path,
    pulp_revision: str,
    staging: Path,
    build_environment: dict[str, str],
    runtime_environment: dict[str, str],
    directory_claim: Any,
) -> dict[str, Any]:
    identity = forge_source_identity(repository, pulp_revision)
    source_claim: provenance.RetainedTreeClaim | None = None
    forge_build_claim: RetainedDirectoryClaim | None = None
    sdk_input_claim: provenance.RetainedTreeClaim | None = None
    forge_input_claim: provenance.RetainedTreeClaim | None = None
    bundle_claim: provenance.RetainedTreeClaim | None = None
    try:
        try:
            source_claim, source_evidence = provenance.retain_git_source_tree(
                repository,
                verifier.EXPECTED_FORGE_REVISION,
                "Forge exact source",
                overlays={"PULP_SDK_REF": f"{pulp_revision}\n".encode()},
            )
        except ValueError as error:
            raise AcceptanceError(str(error)) from error
        if source_evidence["excluded_gitlinks"]:
            raise AcceptanceError("Forge source contains unsealed Git links")
        try:
            provenance._raise_descriptor_limit(262144)
            sdk_input_claim, sdk_input_evidence = provenance.retain_current_regular_tree(
                prefix,
                "Forge consumed installed Pulp SDK",
                maximum_files=65536,
                maximum_bytes=16 * 1024 * 1024 * 1024,
            )
        except ValueError as error:
            raise AcceptanceError(str(error)) from error
        forge_build_claim = claim_fresh_directory(
            build_dir, "Forge build directory"
        )
        configure_claim = provenance.CombinedClaims(
            directory_claim, source_claim, sdk_input_claim, forge_build_claim
        )
        expected_pulp_dir = (prefix / "lib/cmake/Pulp").resolve()
        configure = run_bounded(
            [
                "cmake", "-S", str(repository), "-B", str(build_dir),
                "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_PREFIX_PATH={prefix}",
                f"-DPulp_DIR={expected_pulp_dir}",
            ],
            cwd=repository, environment=build_environment, timeout=1800,
            directory_claim=configure_claim,
        )
        if configure.returncode != 0:
            raise AcceptanceError(
                f"Forge Modular configure failed: {configure.stderr[-2000:]}"
            )
        cache_digest = forge_build_claim.bind_file(
            build_dir / "CMakeCache.txt", "Forge CMake cache"
        )
        cache_descriptor = forge_build_claim.file_claims[-1]["descriptor"]
        cache = parse_cmake_cache_bytes(
            bounded_descriptor_bytes(
                cache_descriptor, 4 * 1024 * 1024, "Forge CMake cache"
            ),
            "Forge CMake cache",
        )
        if Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != repository.resolve():
            raise AcceptanceError(
                "Forge build is not configured from the exact Forge checkout"
            )
        if cache.get("CMAKE_BUILD_TYPE") != "Release":
            raise AcceptanceError("Forge build must be single-config Release")
        if Path(cache.get("Pulp_DIR", "")).resolve() != expected_pulp_dir:
            raise AcceptanceError(
                "Forge build does not consume the accepted Pulp install"
            )
        try:
            forge_input_claim, forge_input_evidence = (
                provenance.retain_generated_build_inputs(
                    build_dir, "Forge regenerated build inputs",
                    targets=("ForgeModular_Standalone",),
                    forced_clean_before_build=False,
                )
            )
        except ValueError as error:
            raise AcceptanceError(str(error)) from error
        build_claim = provenance.CombinedClaims(
            directory_claim, source_claim, sdk_input_claim,
            forge_build_claim, forge_input_claim,
        )
        build = run_bounded(
            [
                "cmake", "--build", str(build_dir), "--target",
                "ForgeModular_Standalone", "--parallel",
            ],
            cwd=repository, environment=build_environment, timeout=3600,
            directory_claim=build_claim,
        )
        if build.returncode != 0:
            raise AcceptanceError(
                f"Forge Modular refresh failed: {build.stderr[-2000:]}"
            )
        source_claim.assert_full()
        sdk_input_claim.assert_full()
        forge_input_claim.assert_full()
        forge_build_claim.assert_current()
        stamps: list[tuple[Path, dict[str, str], int]] = []
        candidates = list(build_dir.rglob("FORGE_BUILD_INFO"))
        if len(candidates) > 64:
            raise AcceptanceError("Forge build contains too many build-info candidates")
        for candidate in candidates:
            if not candidate.is_file() or candidate.is_symlink():
                continue
            forge_build_claim.bind_file(candidate, "Forge bundle build-info")
            retained = forge_build_claim.file_claims[-1]
            values = parse_forge_stamp_bytes(bounded_descriptor_bytes(
                retained["descriptor"], 16 * 1024, "FORGE_BUILD_INFO"
            ))
            if (
                values.get("product") == "Forge Modular"
                and values.get("format") == "Standalone application"
            ):
                stamps.append((candidate, values, retained["descriptor"]))
        if len(stamps) != 1:
            raise AcceptanceError(
                "Forge build must contain one exact Modular standalone stamp"
            )
        stamp_path, stamp, stamp_descriptor = stamps[0]
        bundle = stamp_path.parents[2]
        if bundle.suffix != ".app":
            raise AcceptanceError("Forge Modular stamp is not inside an app bundle")
        try:
            bundle_claim, bundle_evidence = provenance.retain_current_regular_tree(
                bundle, "Forge Modular bundle"
            )
        except ValueError as error:
            raise AcceptanceError(str(error)) from error
        executable_rows = [
            row for row in bundle_claim.file_claims
            if Path(row["relative"]).parent.as_posix() == "Contents/MacOS"
            and row["mode"] == "100755"
        ]
        if len(executable_rows) != 1:
            raise AcceptanceError("Forge Modular bundle lacks one exact executable")
        binary_row = executable_rows[0]
        binary = bundle / binary_row["relative"]
        binary_sha256 = provenance.sha256_descriptor(binary_row["descriptor"])
        bundle_stamp = next(
            (
                row for row in bundle_claim.file_claims
                if bundle / row["relative"] == stamp_path
            ),
            None,
        )
        stamp_metadata = os.fstat(stamp_descriptor)
        stamp_identity = {
            "device": stamp_metadata.st_dev, "inode": stamp_metadata.st_ino
        }
        if (
            bundle_stamp is None
            or bundle_stamp["identity"] != stamp_identity
            or provenance.sha256_descriptor(bundle_stamp["descriptor"])
            != provenance.sha256_descriptor(stamp_descriptor)
        ):
            raise AcceptanceError("Forge bundle stamp differs from its retained claim")
        proof_claim = provenance.CombinedClaims(
            directory_claim, source_claim, sdk_input_claim,
            forge_build_claim, forge_input_claim, bundle_claim
        )
        signed = run_bounded(
            ["/usr/bin/codesign", "--verify", "--deep", "--strict", str(bundle)],
            cwd=repository, environment=runtime_environment, timeout=60,
            directory_claim=proof_claim,
        )
        if signed.returncode != 0:
            raise AcceptanceError(
                f"Forge Modular signature verification failed: {signed.stderr}"
            )
        screenshot = staging / "forge-modular-screenshot.png"
        capture = run_bounded(
            [str(binary), "--screenshot", str(screenshot)],
            cwd=repository, environment=runtime_environment, timeout=300,
            directory_claim=proof_claim,
        )
        if capture.returncode != 0:
            raise AcceptanceError(
                f"Forge Modular screenshot smoke failed: {capture.stderr[-2000:]}"
            )
        metrics = verifier._png_metrics(screenshot)
        doctor_path = staging / "forge-gpu-doctor.json"
        doctor = run_bounded(
            [str(prefix / "bin/pulp"), "doctor", "gpu", "--json"],
            cwd=repository, environment=runtime_environment, timeout=300,
            directory_claim=proof_claim,
        )
        if doctor.returncode != 0:
            raise AcceptanceError(
                f"Forge-cwd GPU doctor failed: {doctor.stderr[-2000:]}"
            )
        try:
            doctor_payload = json.loads(doctor.stdout)
        except json.JSONDecodeError as error:
            raise AcceptanceError(
                "Forge-cwd GPU doctor returned malformed JSON"
            ) from error
        doctor_path.write_text(
            json.dumps(doctor_payload, indent=2, sort_keys=True) + "\n"
        )
        source_claim.assert_full()
        sdk_input_claim.assert_full()
        forge_input_claim.assert_full()
        forge_build_claim.assert_current()
        bundle_claim.assert_full()
        if forge_source_identity(repository, pulp_revision) != identity:
            raise AcceptanceError(
                "Forge source identity changed during downstream proof"
            )
        return {
            **identity,
            "source_tree_claim": source_evidence,
            "pulp_sdk_tree_claim": sdk_input_evidence,
            "build_input_claim": forge_input_evidence,
            "build_directory_claim_method": (
                "unpredictable-staging-directory-renameatx-noreplace-retained-fd-v1"
            ),
            "build_directory_claim": forge_build_claim.identity,
            "cmake_cache_sha256": cache_digest,
            "build_target": "ForgeModular_Standalone",
            "bundle_build_info": stamp,
            "bundle_build_info_sha256": provenance.sha256_descriptor(
                stamp_descriptor
            ),
            "bundle_binary_sha256": binary_sha256,
            "bundle_tree_claim": bundle_evidence,
            "codesign_verify": "pass",
            "screenshot_metrics": metrics,
        }
    finally:
        if bundle_claim is not None:
            bundle_claim.close()
        if forge_input_claim is not None:
            forge_input_claim.close()
        if forge_build_claim is not None:
            forge_build_claim.close()
        if sdk_input_claim is not None:
            sdk_input_claim.close()
        if source_claim is not None:
            source_claim.close()


def source_blobs(repository: Path, revision: str) -> dict[str, str]:
    paths = verifier.EXPECTED_SOURCE_BLOBS_V2
    historical = verifier._git_blobs(revision, paths)
    head = verifier._git_blobs("HEAD", paths)
    checkout = verifier._checkout_blobs(paths)
    if set(historical) != paths or historical != head or historical != checkout:
        raise AcceptanceError("A2 recorder/verifier/recipe source is not exact-head clean")
    return historical


def ensure_outside_checkout(path: Path, repositories: tuple[Path, ...]) -> None:
    resolved = path.resolve()
    for repository in repositories:
        try:
            resolved.relative_to(repository.resolve())
        except ValueError:
            continue
        raise AcceptanceError("A2 output/execution path must be outside every checkout")


def paths_overlap(first: Path, second: Path) -> bool:
    first = first.resolve()
    second = second.resolve()
    for child, parent in ((first, second), (second, first)):
        try:
            child.relative_to(parent)
        except ValueError:
            continue
        return True
    return False


def validate_generated_paths(
    build_dir: Path, prefix: Path, forge_build: Path, output: Path
) -> None:
    if not build_dir.is_dir() or build_dir.is_symlink():
        raise AcceptanceError("Pulp build-dir must be an existing external directory")
    for label, path in (
        ("install-prefix", prefix),
        ("forge-build-dir", forge_build),
        ("output-dir", output),
    ):
        if path.exists() or path.is_symlink() or not path.parent.is_dir():
            raise AcceptanceError(
                f"{label} must be a new path under an existing external directory"
            )
    generated = {
        "build-dir": build_dir,
        "install-prefix": prefix,
        "forge-build-dir": forge_build,
        "output-dir": output,
    }
    names = list(generated)
    for index, first_name in enumerate(names):
        for second_name in names[index + 1:]:
            if paths_overlap(generated[first_name], generated[second_name]):
                raise AcceptanceError(
                    f"{first_name} and {second_name} must not overlap"
                )


def retain_staged_evidence(staging: Path) -> RetainedDirectoryClaim:
    """Bind staged bytes before self-verification and retain that claim to publication."""
    descriptor: int | None = None
    claim: RetainedDirectoryClaim | None = None
    try:
        descriptor = os.open(staging, directory_open_flags())
        identity = directory_identity(os.fstat(descriptor), "staging")
        claim = RetainedDirectoryClaim(
            staging, descriptor, identity, "staging", watch_links=False
        )
        descriptor = None
        claim.seal()
        names = sorted(os.listdir(claim.descriptor))
        if "receipt.json" not in names:
            raise AcceptanceError("staging must contain receipt.json")
        for name in names:
            if name in {"", ".", ".."} or Path(name).name != name:
                raise AcceptanceError("staging contains an unsafe evidence filename")
            claim.bind_file(staging / name, f"staged evidence {name}")
        claim.assert_current()
        return claim
    except BaseException:
        if claim is not None:
            claim.close()
        elif descriptor is not None:
            os.close(descriptor)
        raise


def publish_receipt_directory_no_replace(
    staging: Path, output: Path, staging_claim: RetainedDirectoryClaim,
    output_parent_claim: RetainedDirectoryClaim | None = None,
) -> None:
    if output.name in {"", ".", ".."}:
        raise AcceptanceError("output-dir must have a safe final path component")
    if staging_claim.path != staging or staging_claim.label != "staging":
        raise AcceptanceError("publication requires the exact retained staging claim")
    staging_claim.assert_current()
    owned_parent_claim: RetainedDirectoryClaim | None = None
    if output_parent_claim is None:
        owned_parent_claim = retain_existing_directory(
            output.parent, "output-parent"
        )
        output_parent_claim = owned_parent_claim
    if output_parent_claim.path != output.parent:
        raise AcceptanceError(
            "publication requires the exact retained output parent"
        )
    output_parent_claim.assert_current()
    parent_descriptor = output_parent_claim.descriptor
    staging_descriptor: int | None = None
    output_descriptor: int | None = None
    try:
        staging_descriptor = os.open(staging, directory_open_flags())
    except OSError as error:
        if staging_descriptor is not None:
            os.close(staging_descriptor)
        if owned_parent_claim is not None:
            owned_parent_claim.close()
        raise AcceptanceError("publication directories must be real, reachable directories") from error
    try:
        parent_claim = output_parent_claim.identity
        assert_directory_claim(
            output.parent, parent_claim, "output-parent", parent_descriptor
        )
        output_parent_claim.assert_current()
        try:
            names = sorted(os.listdir(staging_descriptor))
        except OSError as error:
            raise AcceptanceError("cannot enumerate the staged evidence directory") from error
        if "receipt.json" not in names:
            raise AcceptanceError("staging must contain receipt.json")
        for name in names:
            if name in {"", ".", ".."} or Path(name).name != name:
                raise AcceptanceError("staging contains an unsafe evidence filename")
            try:
                metadata = os.stat(
                    name, dir_fd=staging_descriptor, follow_symlinks=False
                )
            except OSError as error:
                raise AcceptanceError(f"cannot stat staged evidence {name}") from error
            if not stat_module.S_ISREG(metadata.st_mode):
                raise AcceptanceError("staging must contain only regular evidence files")

        retained_files: dict[str, dict[str, Any]] = {}
        for retained in staging_claim.file_claims:
            try:
                relative = retained["path"].relative_to(staging)
            except ValueError as error:
                raise AcceptanceError(
                    "retained staging claim contains an out-of-tree file"
                ) from error
            if len(relative.parts) != 1 or relative.name in retained_files:
                raise AcceptanceError("retained staging claim is ambiguous")
            retained_files[relative.name] = retained
        if set(retained_files) != set(names):
            raise AcceptanceError(
                "staged evidence inventory changed after pre-verification binding"
            )

        try:
            os.mkdir(output.name, mode=0o700, dir_fd=parent_descriptor)
            output_descriptor = os.open(
                output.name, directory_open_flags(), dir_fd=parent_descriptor
            )
        except OSError as error:
            raise AcceptanceError(
                "output-dir appeared before no-replace publication"
            ) from error
        output_claim = directory_identity(os.fstat(output_descriptor), "output-dir")

        def assert_named_output_claim() -> None:
            assert_directory_claim(
                output.parent, parent_claim, "output-parent", parent_descriptor
            )
            output_parent_claim.assert_current()
            try:
                path_identity = directory_identity(
                    os.stat(
                        output.name,
                        dir_fd=parent_descriptor,
                        follow_symlinks=False,
                    ),
                    "output-dir",
                )
                descriptor_identity = directory_identity(
                    os.fstat(output_descriptor), "output-dir"
                )
            except OSError as error:
                raise AcceptanceError(
                    "output-dir claim is no longer reachable"
                ) from error
            if path_identity != output_claim or descriptor_identity != output_claim:
                raise AcceptanceError(
                    "output-dir path no longer names its claimed directory"
                )

        expected_digests = {
            name: retained_files[name]["sha256"] for name in names
        }

        def link_verified_staged_file(name: str) -> None:
            source_descriptor = os.open(
                name,
                os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=staging_descriptor,
            )
            linked = False
            try:
                source_metadata = os.fstat(source_descriptor)
                if not stat_module.S_ISREG(source_metadata.st_mode):
                    raise AcceptanceError(
                        "staging must contain only regular evidence files"
                    )
                source_identity = {
                    "device": source_metadata.st_dev,
                    "inode": source_metadata.st_ino,
                }
                if (
                    source_identity != retained_files[name]["identity"]
                    or sha256_descriptor(source_descriptor) != expected_digests[name]
                ):
                    raise AcceptanceError(
                        f"staged evidence changed after self-verification: {name}"
                    )
                named_source = os.stat(
                    name, dir_fd=staging_descriptor, follow_symlinks=False
                )
                if {
                    "device": named_source.st_dev,
                    "inode": named_source.st_ino,
                } != source_identity:
                    raise AcceptanceError(
                        f"staged evidence identity changed before publication: {name}"
                    )
                os.fsync(source_descriptor)
                try:
                    os.link(
                        name,
                        name,
                        src_dir_fd=staging_descriptor,
                        dst_dir_fd=output_descriptor,
                        follow_symlinks=False,
                    )
                    linked = True
                except OSError as error:
                    raise AcceptanceError(
                        f"no-replace publication failed for {name}"
                    ) from error
                published = os.stat(
                    name, dir_fd=output_descriptor, follow_symlinks=False
                )
                named_source = os.stat(
                    name, dir_fd=staging_descriptor, follow_symlinks=False
                )
                identities = (
                    {"device": published.st_dev, "inode": published.st_ino},
                    {"device": named_source.st_dev, "inode": named_source.st_ino},
                )
                if any(identity != source_identity for identity in identities):
                    raise AcceptanceError(
                        f"staged evidence identity changed during publication: {name}"
                    )
            except BaseException:
                if linked:
                    try:
                        os.unlink(name, dir_fd=output_descriptor)
                        os.fsync(output_descriptor)
                    except OSError:
                        pass
                raise
            finally:
                os.close(source_descriptor)

        for name in (candidate for candidate in names if candidate != "receipt.json"):
            link_verified_staged_file(name)

        os.fsync(output_descriptor)
        staging_claim.assert_current()
        assert_named_output_claim()

        try:
            link_verified_staged_file("receipt.json")
            os.fsync(output_descriptor)
            os.fsync(parent_descriptor)
            staging_claim.assert_current()
            assert_named_output_claim()
            for name, expected_digest in expected_digests.items():
                published_descriptor = os.open(
                    name,
                    os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
                    dir_fd=output_descriptor,
                )
                try:
                    metadata = os.fstat(published_descriptor)
                    identity = {"device": metadata.st_dev, "inode": metadata.st_ino}
                    named = os.stat(
                        name, dir_fd=output_descriptor, follow_symlinks=False
                    )
                    if (
                        not stat_module.S_ISREG(metadata.st_mode)
                        or {"device": named.st_dev, "inode": named.st_ino} != identity
                        or sha256_descriptor(published_descriptor) != expected_digest
                    ):
                        raise AcceptanceError(
                            f"published evidence bytes differ from staging: {name}"
                        )
                finally:
                    os.close(published_descriptor)
            os.fsync(output_descriptor)
            staging_claim.assert_current()
            assert_named_output_claim()
        except BaseException:
            try:
                os.unlink("receipt.json", dir_fd=output_descriptor)
                os.fsync(output_descriptor)
                os.fsync(parent_descriptor)
            except OSError:
                pass
            raise
    finally:
        if output_descriptor is not None:
            os.close(output_descriptor)
        os.close(staging_descriptor)
        if owned_parent_claim is not None:
            owned_parent_claim.close()


def certify_fresh_recording(
    repository: Path,
    revision: str,
    output: Path,
    *,
    staging_claim: RetainedDirectoryClaim,
    output_parent_claim: RetainedDirectoryClaim,
    execution_claim: Any,
    source_tree_claim: Any,
    build_input_claim: Any,
    provider_input_claim: Any,
) -> dict[str, str]:
    """Certify only the live recorder run with non-serializable retained claims."""
    if (
        type(staging_claim) is not RetainedDirectoryClaim
        or type(output_parent_claim) is not RetainedDirectoryClaim
        or type(execution_claim) is not provenance.CombinedClaims
        or type(source_tree_claim) is not provenance.RetainedTreeClaim
        or type(build_input_claim) is not provenance.RetainedTreeClaim
        or type(provider_input_claim) is not provenance.RetainedClaimSet
    ):
        raise AcceptanceError(
            "fresh A2 certification requires recorder-owned retained claims"
        )
    provenance.assert_exact_live_head(repository, revision)
    staging_claim.assert_current()
    output_parent_claim.assert_current()
    execution_claim.assert_current()
    source_tree_claim.assert_full()
    build_input_claim.assert_full()
    provider_input_claim.assert_full()

    problems = verifier.verify(output, require_terminal=False)
    if problems:
        raise AcceptanceError(
            "published structural self-verification failed: " + "; ".join(problems)
        )
    output_descriptor = os.open(output, directory_open_flags())
    try:
        published_names = sorted(os.listdir(output_descriptor))
        claimed_by_name = {
            Path(claim["path"]).name: claim for claim in staging_claim.file_claims
        }
        if set(published_names) != set(claimed_by_name):
            raise AcceptanceError(
                "fresh A2 certification requires the exact retained evidence set"
            )
        for name in published_names:
            claim = claimed_by_name[name]
            descriptor = os.open(
                name,
                os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=output_descriptor,
            )
            try:
                metadata = os.fstat(descriptor)
                named = os.stat(
                    name, dir_fd=output_descriptor, follow_symlinks=False
                )
                identity = {"device": metadata.st_dev, "inode": metadata.st_ino}
                if (
                    not stat_module.S_ISREG(metadata.st_mode)
                    or identity != claim["identity"]
                    or {"device": named.st_dev, "inode": named.st_ino} != identity
                    or metadata.st_size != claim["bytes"]
                    or sha256_descriptor(descriptor) != claim["sha256"]
                ):
                    raise AcceptanceError(
                        f"published evidence differs from retained recorder artifact: {name}"
                    )
            finally:
                os.close(descriptor)
    finally:
        os.close(output_descriptor)

    provenance.assert_exact_live_head(repository, revision)
    staging_claim.assert_current()
    output_parent_claim.assert_current()
    execution_claim.assert_current()
    source_tree_claim.assert_full()
    build_input_claim.assert_full()
    provider_input_claim.assert_full()
    return {
        "fresh_recorder_certification": "pass",
        "integration_head": revision,
        "durable_receipt_status": verifier.OFFLINE_STRUCTURAL_STATUS,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--install-prefix", type=Path, required=True)
    parser.add_argument("--planning-repository", type=Path, required=True)
    parser.add_argument("--plan-revision", required=True)
    parser.add_argument("--plan-sha256", required=True)
    parser.add_argument("--forge-repository", type=Path, required=True)
    parser.add_argument("--forge-build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args(argv)

    repository = args.repository.resolve()
    build_dir = args.build_dir.resolve()
    prefix = args.install_prefix.resolve()
    planning = args.planning_repository.resolve()
    forge_repository = args.forge_repository.resolve()
    forge_build = args.forge_build_dir.resolve()
    output = args.output_dir.absolute()
    try:
        script_repository = SCRIPT_DIR.parents[1].resolve()
        if repository != script_repository:
            raise AcceptanceError(
                "repository must be the exact checkout containing this recorder/verifier"
            )
        for generated_path in (build_dir, prefix, forge_build):
            ensure_outside_checkout(
                generated_path, (repository, planning, forge_repository)
            )
        ensure_outside_checkout(output, (repository, planning, forge_repository))
        validate_generated_paths(build_dir, prefix, forge_build, output)
        output_parent_claim = retain_existing_directory(
            output.parent, "output-parent"
        )
        revision = provenance._git_text(repository, "rev-parse", "HEAD")
        source_identity = provenance.clean_source_identity(repository, revision)
        accepted_plan = provenance.plan_identity(
            planning, args.plan_revision, args.plan_sha256
        )
        blobs = source_blobs(repository, revision)
        build_environment = os.environ.copy()
        for inherited_override in (
            "PULP_SKIP_RUST_CLI",
            "PULP_USE_CPP",
            "PULP_RS_CPP_BINARY",
            "PULP_RS_FALLTHROUGH",
            "PULP_RS_NO_FALLTHROUGH",
        ):
            build_environment.pop(inherited_override, None)
        runtime_environment = build_environment.copy()
        runtime_environment["PATH"] = SYSTEM_PATH
        (
            install_provenance, binaries, install_claim,
            source_tree_claim, build_input_claim, provider_input_claim,
        ) = refresh_install(repository, build_dir, prefix, build_environment)
        execution_claim = provenance.CombinedClaims(
            install_claim, source_tree_claim, build_input_claim,
            provider_input_claim,
        )
        cli = prefix / "bin/pulp"
        mcp = prefix / "bin/pulp-mcp"
        with tempfile.TemporaryDirectory(prefix="pulp-a2-execution-") as execution_name, \
             tempfile.TemporaryDirectory(prefix="pulp-a2-receipt-", dir=output.parent) as staging_name:
            execution = Path(execution_name).resolve()
            staging = Path(staging_name).resolve()
            ensure_outside_checkout(execution, (repository, planning, forge_repository))
            git_probe = subprocess.run(
                ["git", "-C", str(execution), "rev-parse", "--is-inside-work-tree"],
                check=False, capture_output=True, text=True,
            )
            if git_probe.returncode == 0:
                raise AcceptanceError("fresh execution directory unexpectedly belongs to Git")
            cli_results: dict[str, dict[str, dict[str, Any]]] = {}
            for group, recipe in RECIPES.items():
                cli_results[group] = {}
                for suffix, negative in (("run1", False), ("run2", False), ("negative", True)):
                    result = run_cli_probe(
                        cli, recipe, execution / f"{group}-cli-{suffix}-artifacts",
                        negative=negative, cwd=execution,
                        environment=runtime_environment,
                        directory_claim=execution_claim,
                    )
                    cli_results[group][suffix] = result
                    (staging / f"{group}-{suffix}.json").write_text(
                        json.dumps(result, indent=2, sort_keys=True) + "\n"
                    )
                if canonical_result(cli_results[group]["run1"]) != canonical_result(cli_results[group]["run2"]):
                    raise AcceptanceError(f"{recipe} installed CLI rerun was nondeterministic")
            mcp_results: dict[str, dict[str, dict[str, Any]]] = {
                group: {} for group in RECIPES
            }
            session = McpSession(
                mcp, execution, runtime_environment, execution_claim
            )
            try:
                session.initialize()
                for group, recipe in RECIPES.items():
                    for suffix, negative in (("positive", False), ("negative", True)):
                        evidence = session.probe(
                            recipe, execution / f"{group}-mcp-{suffix}-artifacts",
                            negative=negative,
                        )
                        mcp_results[group][suffix] = evidence
                        cli_key = "negative" if negative else "run1"
                        if canonical_result(evidence) != canonical_result(cli_results[group][cli_key]):
                            raise AcceptanceError(f"{recipe} installed CLI/MCP parity failed")
            finally:
                session.close()
            (staging / "mcp-transcript.jsonl").write_text(
                "".join(json.dumps(row, sort_keys=True) + "\n" for row in session.transcript)
            )
            additional_pulp_path_canaries = bind_additional_pulp_path_canaries(
                cli_results, mcp_results
            )
            forge = prove_forge(
                forge_repository, forge_build, prefix, revision, staging,
                build_environment, runtime_environment, execution_claim,
            )
            raw_names = {
                *(f"{group}-{suffix}.json" for group in RECIPES
                  for suffix in ("run1", "run2", "negative")),
                "mcp-transcript.jsonl", "forge-modular-screenshot.png",
                "forge-gpu-doctor.json",
            }
            receipt = {
                "schema": "pulp.gpu-probe-acceptance-receipt.v2",
                "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "integration_head": revision,
                "source_identity": source_identity,
                "source_blobs": blobs,
                "accepted_plan": accepted_plan,
                "execution_context": {
                    "cwd_role": "fresh-temporary-directory-outside-any-checkout",
                    "path": SYSTEM_PATH,
                    "checkout_discovery": "explicitly-rejected-by-git-rev-parse",
                },
                "install_provenance": install_provenance,
                "binaries": binaries,
                "run_groups": {
                    group: {"recipe": recipe, "binary_role": "installed_rust_cli"}
                    for group, recipe in RECIPES.items()
                },
                "raw_sha256": {name: sha256(staging / name) for name in sorted(raw_names)},
                "forge_downstream": forge,
                "additional_pulp_path_canaries": additional_pulp_path_canaries,
                "acceptance": {
                    "terminal_status": verifier.OFFLINE_STRUCTURAL_STATUS,
                    "all_four_installed_cli": "pass",
                    "all_four_installed_mcp": "pass",
                    "seeded_negative_controls": "pass",
                    "forge_modular_and_additional_pulp_path_canaries": "pass",
                },
            }
            if provenance.clean_source_identity(repository, revision) != source_identity:
                raise AcceptanceError("Pulp source identity changed during A2 recording")
            if source_blobs(repository, revision) != blobs:
                raise AcceptanceError("Pulp source binding changed during A2 recording")
            assert_directory_claim(
                prefix,
                install_provenance["install_prefix_claim"],
                "install-prefix",
                install_claim.descriptor,
            )
            (staging / "receipt.json").write_text(
                json.dumps(receipt, indent=2, sort_keys=True) + "\n"
            )
            staging_claim = retain_staged_evidence(staging)
            try:
                problems = verifier.verify(staging)
                staging_claim.assert_current()
                if problems:
                    raise AcceptanceError(
                        "self-verification failed: " + "; ".join(problems)
                    )
                publish_receipt_directory_no_replace(
                    staging, output, staging_claim, output_parent_claim
                )
                staging_claim.assert_current()
                certification = certify_fresh_recording(
                    repository,
                    revision,
                    output,
                    staging_claim=staging_claim,
                    output_parent_claim=output_parent_claim,
                    execution_claim=execution_claim,
                    source_tree_claim=source_tree_claim,
                    build_input_claim=build_input_claim,
                    provider_input_claim=provider_input_claim,
                )
            finally:
                staging_claim.close()
            install_claim.assert_current()
            source_tree_claim.assert_full()
            build_input_claim.assert_full()
            provider_input_claim.assert_full()
            install_claim.close()
            provider_input_claim.close()
            build_input_claim.close()
            source_tree_claim.close()
            output_parent_claim.assert_current()
            output_parent_claim.close()
        print(json.dumps({
            "output": str(output),
            **certification,
        }, sort_keys=True))
        return 0
    except (AcceptanceError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
