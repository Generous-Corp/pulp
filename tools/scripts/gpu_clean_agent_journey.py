#!/usr/bin/env python3
"""Prepare, record, and independently verify a GPU recipe repair session.

The subcommands deliberately run in separate processes. ``prepare`` creates an
agent-visible workspace and a private reference case. ``record`` starts one
fresh Codex session with only the installed CLI, symptom, workspace, and public
documentation in its prompt. ``verify`` checks the transcript and filesystem
against the private reference and emits structural, nonterminal evidence for
the separate protected planning acceptance process.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import math
import os
import pathlib
import queue
import re
import secrets
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from typing import Any, Iterable

import gpu_clean_agent_trust as trust


RESULT_SCHEMA = "pulp.gpu-probe-result.v1"
DISCOVERY_SCHEMA = "pulp.gpu-recipes-discovery.v1"
CATALOG_SCHEMA = "pulp.gpu-recipes.v1"
CASE_SCHEMA = "pulp.gpu-clean-agent-case.v4"
SESSION_SCHEMA = "pulp.gpu-clean-agent-session.v4"
VERIFICATION_SCHEMA = "pulp.gpu-clean-agent-verification.v4"
LEGACY_V3_SCHEMAS = frozenset({
    "pulp.gpu-clean-agent-case.v3",
    "pulp.gpu-clean-agent-session.v3",
    "pulp.gpu-clean-agent-verification.v3",
})
SUPERSEDED_SCHEMA = "pulp.gpu-clean-agent-disposition.v1"

MAX_ARTIFACTS = 16
MAX_TOTAL_ARTIFACT_BYTES = 16 * 1024 * 1024
MAX_WORKSPACE_BYTES = 64 * 1024 * 1024
MAX_WORKSPACE_ENTRIES = 128
MAX_STDOUT_BYTES = 8 * 1024 * 1024
MAX_STDERR_BYTES = 4 * 1024 * 1024
MAX_METADATA_BYTES = 16 * 1024 * 1024
MAX_BINARY_BYTES = 512 * 1024 * 1024
MAX_BUNDLE_BYTES = 8 * 1024 * 1024
MAX_BUNDLE_EVENTS = 256
MAX_BUNDLE_STRING_BYTES = 512 * 1024
MAX_BUNDLE_DEPTH = 32
MAX_AGENT_WALL_CLOCK_SECONDS = 900.0
MAX_DESCENDANT_PROCESSES = 64
MONITOR_INTERVAL_SECONDS = 0.01
PROCESS_GROUP_GRACE_SECONDS = 0.5
ARTIFACT_KINDS = {"json", "image", "numeric-samples", "trace"}
AGENT_INHERITED_ENV_KEYS = {
    "LANG", "LC_ALL", "USER", "LOGNAME", "SHELL", "TERM",
}
AGENT_FIXED_ENV_KEYS = {
    "HOME", "XDG_CONFIG_HOME", "CODEX_HOME", "TMPDIR", "PATH", "PWD",
    "PULP_UPDATE_CHECK_DISABLED", "NO_COLOR", "HTTPS_PROXY", "HTTP_PROXY", "NO_PROXY",
}
RESULT_FIELDS = {
    "schema", "version", "gpu_evidence_id", "recipe_id", "source_digest",
    "signature_digest", "dimensions", "seed", "clock", "input_format",
    "output_format", "encoding", "tolerance", "adapter_policy", "adapter",
    "numeric_sample_count", "mutation", "verdict", "passes", "artifacts",
    "recommendations",
}
SEED_LINE = 'seeded_option="--negative-control"'
REPAIRED_LINE = 'seeded_option=""'


class JourneyError(RuntimeError):
    """A contract violation that means the acceptance is not proven."""


def _require_v4_structural_nonterminal(value: Any, label: str) -> None:
    """Reject the superseded in-process authority vocabulary from v4 outputs."""
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = str(key).replace("-", "_").casefold()
            if (
                normalized == "terminal"
                or normalized.startswith("terminal_")
                or "accepted" in normalized
                or "acceptance_gate" in normalized
            ):
                raise JourneyError(
                    f"{label} contains forbidden in-process authority field: {key}"
                )
            _require_v4_structural_nonterminal(child, label)
    elif isinstance(value, list):
        for child in value:
            _require_v4_structural_nonterminal(child, label)
    elif isinstance(value, str) and value == "independent-agent-accepted":
        raise JourneyError(f"{label} contains a superseded acceptance value")


def classify_legacy_v3_document(value: Any) -> dict[str, str]:
    """Read a historical v3 document only as retained nonterminal input."""
    if not isinstance(value, dict) or value.get("schema") not in LEGACY_V3_SCHEMAS:
        raise JourneyError("legacy clean-agent document is not a recognized v3 schema")
    return {
        "schema": str(value["schema"]),
        "status": "historical-v3-nonterminal",
        "migration_use": "retained-input-only",
    }


class JourneyUnavailable(JourneyError):
    """The requested recipe evidence is unavailable or unverified."""


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


@dataclass(frozen=True)
class ResourceRoot:
    path: pathlib.Path
    total_bytes: int = MAX_WORKSPACE_BYTES
    file_bytes: int = MAX_TOTAL_ARTIFACT_BYTES
    entries: int = MAX_WORKSPACE_ENTRIES


def _v4_limits() -> dict[str, Any]:
    return {
        "wall_clock_seconds": int(MAX_AGENT_WALL_CLOCK_SECONDS),
        "max_descendant_processes": MAX_DESCENDANT_PROCESSES,
        "max_transcript_events": MAX_BUNDLE_EVENTS,
        "roots": {
            "workspace": {
                "total_bytes": MAX_WORKSPACE_BYTES,
                "file_bytes": MAX_TOTAL_ARTIFACT_BYTES,
                "entries": MAX_WORKSPACE_ENTRIES,
            },
            "runtime": {
                "total_bytes": MAX_WORKSPACE_BYTES,
                "file_bytes": MAX_TOTAL_ARTIFACT_BYTES,
                "entries": MAX_WORKSPACE_ENTRIES,
            },
        },
    }


def _json_bytes(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _read_regular_bytes(path: pathlib.Path, limit: int = MAX_METADATA_BYTES) -> bytes:
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except OSError as exc:
        raise JourneyError(f"required file is unavailable: {path}") from exc
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            raise JourneyError(f"required path is not one confined regular file: {path}")
        if metadata.st_size > limit:
            raise JourneyError(f"required file exceeds its {limit}-byte bound: {path}")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(descriptor, min(1024 * 1024, limit + 1 - total))
            if not chunk:
                break
            total += len(chunk)
            if total > limit:
                raise JourneyError(f"required file exceeds its {limit}-byte bound: {path}")
            chunks.append(chunk)
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def _sha256(path: pathlib.Path, limit: int = MAX_METADATA_BYTES) -> str:
    return _sha256_bytes(_read_regular_bytes(path, limit))


def _require_real_directory(path: pathlib.Path) -> None:
    if not path.is_absolute() or any(part in {".", ".."} for part in path.parts):
        raise JourneyError(f"directory must be an absolute normalized path: {path}")
    try:
        metadata = path.lstat()
        resolved = path.resolve(strict=True)
    except OSError as exc:
        raise JourneyError(f"directory is unavailable: {path}") from exc
    if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode) or resolved != path:
        raise JourneyError(f"directory or an ancestor is not a real path: {path}")


def _write_bytes(path: pathlib.Path, payload: bytes, *, replace: bool = False) -> None:
    if not path.is_absolute():
        raise JourneyError(f"output path must be absolute: {path}")
    _require_real_directory(path.parent)
    if not replace and (path.exists() or path.is_symlink()):
        raise JourneyError(f"output already exists: {path}")
    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    try:
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            0o600,
        )
        try:
            view = memoryview(payload)
            while view:
                written = os.write(descriptor, view)
                view = view[written:]
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        if replace:
            os.replace(temporary, path)
        else:
            try:
                os.link(temporary, path, follow_symlinks=False)
            except OSError as exc:
                raise JourneyError(f"output appeared or could not be published: {path}") from exc
            temporary.unlink()
        _fsync_directory(path.parent)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _fsync_directory(path: pathlib.Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _publish_structural_pair(
    *, bundle_path: pathlib.Path, bundle_payload: bytes,
    receipt_path: pathlib.Path, receipt_payload: bytes,
) -> None:
    """Publish bundle then receipt, recovering either exact staged state after a crash."""

    if receipt_path.exists() or receipt_path.is_symlink():
        raise JourneyError("structural verification receipt already exists")
    if bundle_path.exists() or bundle_path.is_symlink():
        if bundle_path.is_symlink() or _read_regular_bytes(bundle_path) != bundle_payload:
            raise JourneyError("pre-existing audit bundle is not this exact staged transaction")
    else:
        _write_bytes(bundle_path, bundle_payload)
    try:
        _write_bytes(receipt_path, receipt_payload)
    except BaseException:
        # _write_bytes can fail after atomically linking the receipt but before
        # its parent-directory fsync returns.  Never remove the bundle while a
        # receipt may be visible.  If the complete receipt was linked, retry
        # the durability barrier and accept the exact pair; otherwise leave the
        # exact bundle staged so a later invocation can safely recover it.
        try:
            receipt_complete = (
                not receipt_path.is_symlink()
                and _read_regular_bytes(receipt_path) == receipt_payload
            )
        except (FileNotFoundError, JourneyError, OSError):
            receipt_complete = False
        if receipt_complete:
            _fsync_directory(receipt_path.parent)
            return
        raise


def _write_json(path: pathlib.Path, value: dict[str, Any], *, replace: bool = False) -> None:
    _write_bytes(path, _json_bytes(value), replace=replace)


def _load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(_read_regular_bytes(path).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise JourneyError(f"file does not contain one JSON object: {path}") from exc
    if not isinstance(value, dict):
        raise JourneyError(f"JSON root must be an object: {path}")
    return value


def _require_new_directory(path: pathlib.Path) -> None:
    if not path.is_absolute() or any(part in {".", ".."} for part in path.parts):
        raise JourneyError(f"new directory must be an absolute normalized path: {path}")
    if path.exists() or path.is_symlink():
        raise JourneyError(f"new directory must not exist: {path}")
    _require_real_directory(path.parent)


def _is_within(path: pathlib.Path, root: pathlib.Path) -> bool:
    return path == root or path.is_relative_to(root)


def _validate_isolation(
    workspace: pathlib.Path,
    case_dir: pathlib.Path,
    forbidden_roots: Iterable[pathlib.Path],
) -> None:
    _require_new_directory(workspace)
    _require_new_directory(case_dir)
    if _is_within(workspace, case_dir) or _is_within(case_dir, workspace):
        raise JourneyError("agent workspace and private verifier directory must not overlap")
    for raw_root in forbidden_roots:
        root = raw_root.resolve(strict=True)
        _require_real_directory(root)
        if _is_within(workspace, root) or _is_within(case_dir, root):
            raise JourneyError("workspace and private verifier directory must be outside source trees")


def _validate_install_isolation(
    workspace: pathlib.Path, case_dir: pathlib.Path, installed_prefix: pathlib.Path,
) -> None:
    if any(
        _is_within(first, second) or _is_within(second, first)
        for first, second in (
            (workspace, installed_prefix),
            (case_dir, installed_prefix),
        )
    ):
        raise JourneyError("installed prefix must be separate from public and private evidence trees")


def _resource_usage(root: ResourceRoot) -> None:
    path = root.path
    if not path.exists() and not path.is_symlink():
        return
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise JourneyError(f"monitored root is unavailable: {path}") from exc
    if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        raise JourneyError(f"monitored root is not a real directory: {path}")
    total = 0
    count = 0

    def inspect(directory: pathlib.Path) -> None:
        nonlocal total, count
        try:
            entries = list(os.scandir(directory))
        except OSError as exc:
            raise JourneyError(f"monitored directory cannot be enumerated: {directory}") from exc
        for entry in entries:
            count += 1
            if count > root.entries:
                raise JourneyError(f"workspace exceeded its {root.entries}-entry production bound")
            try:
                item = entry.stat(follow_symlinks=False)
            except OSError as exc:
                raise JourneyError(f"monitored entry became unavailable: {entry.path}") from exc
            if stat.S_ISDIR(item.st_mode):
                inspect(pathlib.Path(entry.path))
            elif stat.S_ISREG(item.st_mode):
                if item.st_nlink != 1:
                    raise JourneyError(f"workspace file has an external hard link: {entry.path}")
                if item.st_size > root.file_bytes:
                    raise JourneyError(
                        f"workspace file exceeded its {root.file_bytes}-byte production bound"
                    )
                total += item.st_size
                if total > root.total_bytes:
                    raise JourneyError(
                        f"workspace exceeded its {root.total_bytes}-byte production bound"
                    )
            else:
                raise JourneyError(f"workspace entry is not a real file or directory: {entry.path}")

    inspect(path)


def _process_group_exists(process: subprocess.Popen[bytes]) -> bool:
    process.poll()
    if os.name != "posix":
        return process.poll() is None
    try:
        os.killpg(process.pid, 0)
        return True
    except (ProcessLookupError, PermissionError):
        return False


def _descendant_process_count(process_group: int) -> int | None:
    """Return descendants in the isolated POSIX process group, excluding leader."""
    if os.name != "posix":
        return None
    try:
        completed = subprocess.run(
            ["/bin/ps", "-axo", "pgid=,pid="],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=2.0,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise JourneyError("cannot enforce descendant-process bound") from error
    if completed.returncode != 0:
        raise JourneyError("cannot enforce descendant-process bound")
    members = 0
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) != 2:
            continue
        try:
            pgid, pid = (int(field) for field in fields)
        except ValueError:
            continue
        if pgid == process_group and pid != process_group:
            members += 1
    return members


def _terminate_process_group(process: subprocess.Popen[bytes]) -> None:
    if os.name == "posix":
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            return
        deadline = time.monotonic() + PROCESS_GROUP_GRACE_SECONDS
        while _process_group_exists(process) and time.monotonic() < deadline:
            time.sleep(0.01)
        if _process_group_exists(process):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
    else:
        if process.poll() is not None:
            return
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if process.poll() is None:
            process.kill()


def _run(
    argv: list[str],
    timeout_seconds: float,
    *,
    cwd: pathlib.Path | None = None,
    env: dict[str, str] | None = None,
    monitor_roots: Iterable[ResourceRoot] = (),
    stdout_limit: int = MAX_STDOUT_BYTES,
    stderr_limit: int = MAX_STDERR_BYTES,
) -> CommandResult:
    """Capture a process while enforcing output, time, and filesystem caps."""

    if (
        not argv or not math.isfinite(timeout_seconds) or timeout_seconds <= 0
        or stdout_limit < 1 or stderr_limit < 1
    ):
        raise JourneyError("invalid bounded subprocess request")
    child_env = dict(os.environ if env is None else env)
    child_env["NO_COLOR"] = "1"
    creationflags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) if os.name == "nt" else 0
    try:
        process = subprocess.Popen(
            argv,
            cwd=str(cwd) if cwd is not None else None,
            env=child_env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=os.name == "posix",
            creationflags=creationflags,
        )
    except OSError as exc:
        raise JourneyError(f"could not start command: {argv[0]}") from exc
    assert process.stdout is not None and process.stderr is not None
    buffers = {"stdout": bytearray(), "stderr": bytearray()}
    failures: queue.Queue[str] = queue.Queue(maxsize=1)

    def fail_once(message: str) -> None:
        try:
            failures.put_nowait(message)
        except queue.Full:
            pass

    def consume(label: str, stream: Any, limit: int) -> None:
        try:
            while True:
                chunk = stream.read(64 * 1024)
                if not chunk:
                    return
                remaining = limit - len(buffers[label])
                if remaining > 0:
                    buffers[label].extend(chunk[:remaining])
                if len(chunk) > remaining:
                    fail_once(f"{label} exceeded its {limit}-byte production bound")
                    return
        except OSError as exc:
            fail_once(f"could not read bounded {label}: {exc}")

    threads = [
        threading.Thread(target=consume, args=("stdout", process.stdout, stdout_limit), daemon=True),
        threading.Thread(target=consume, args=("stderr", process.stderr, stderr_limit), daemon=True),
    ]
    for thread in threads:
        thread.start()
    deadline = time.monotonic() + timeout_seconds
    violation: str | None = None
    roots = tuple(monitor_roots)
    next_process_count = 0.0
    try:
        while process.poll() is None:
            try:
                violation = failures.get_nowait()
            except queue.Empty:
                pass
            if violation is None:
                try:
                    for root in roots:
                        _resource_usage(root)
                except (JourneyError, OSError) as exc:
                    violation = str(exc)
            now = time.monotonic()
            if violation is None and now >= next_process_count:
                try:
                    descendants = _descendant_process_count(process.pid)
                except JourneyError as exc:
                    violation = str(exc)
                else:
                    if (
                        descendants is not None
                        and descendants > MAX_DESCENDANT_PROCESSES
                    ):
                        violation = (
                            "command exceeded its "
                            f"{MAX_DESCENDANT_PROCESSES}-descendant process bound"
                        )
                next_process_count = now + 0.1
            if violation is not None:
                _terminate_process_group(process)
                break
            if now >= deadline:
                violation = f"command exceeded the {timeout_seconds:g}s bound: {argv[0]}"
                _terminate_process_group(process)
                break
            time.sleep(MONITOR_INTERVAL_SECONDS)
        try:
            process.wait(timeout=PROCESS_GROUP_GRACE_SECONDS + 1.0)
        except subprocess.TimeoutExpired:
            _terminate_process_group(process)
            process.wait(timeout=1.0)
        if violation is None:
            try:
                for root in roots:
                    _resource_usage(root)
            except (JourneyError, OSError) as exc:
                violation = str(exc)
        if _process_group_exists(process):
            if violation is None:
                violation = "command left a descendant process outside its bounded lifetime"
            _terminate_process_group(process)
    finally:
        for thread in threads:
            thread.join(timeout=1.0)
            if thread.is_alive() and violation is None:
                violation = "bounded output reader did not reach end-of-file"
        for stream in (process.stdout, process.stderr):
            try:
                stream.close()
            except OSError:
                pass
    if violation is None:
        try:
            violation = failures.get_nowait()
        except queue.Empty:
            pass
    if violation is not None:
        raise JourneyError(violation)
    return CommandResult(
        int(process.returncode),
        bytes(buffers["stdout"]).decode("utf-8", errors="replace"),
        bytes(buffers["stderr"]).decode("utf-8", errors="replace"),
    )


def _json_stdout(result: CommandResult, label: str) -> dict[str, Any]:
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        detail = result.stderr.strip() or result.stdout.strip() or "no output"
        raise JourneyError(f"{label} did not emit one JSON object: {detail}") from exc
    if not isinstance(value, dict):
        raise JourneyError(f"{label} JSON root must be an object")
    return value


def _select_recipe(
    pulp: pathlib.Path, symptom: str, timeout_seconds: float,
    cwd: pathlib.Path | None = None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    result = _run(
        [str(pulp), "gpu", "recipes", "list", "--symptom", symptom, "--json"],
        timeout_seconds,
        cwd=cwd,
        monitor_roots=[ResourceRoot(cwd)] if cwd is not None else (),
    )
    if result.returncode == 2:
        raise JourneyUnavailable(f"symptom discovery is unavailable: {result.stderr.strip()}")
    if result.returncode != 0:
        raise JourneyError(
            f"symptom discovery failed with exit {result.returncode}: {result.stderr.strip()}"
        )
    discovery = _json_stdout(result, "symptom discovery")
    revision = discovery.get("catalog_revision")
    matches = discovery.get("recipes")
    if discovery.get("schema") != DISCOVERY_SCHEMA:
        raise JourneyError("symptom discovery schema is not the supported v1 contract")
    if type(revision) is not int or revision < 1:
        raise JourneyError("symptom discovery has no valid catalog revision")
    if not isinstance(matches, list) or len(matches) != 1:
        raise JourneyError("symptom must select exactly one recipe")
    selected = matches[0]
    if not isinstance(selected, dict) or selected.get("callable") is not True:
        raise JourneyUnavailable("the uniquely selected recipe is not callable in this CLI")
    recipe = selected.get("recipe")
    if not isinstance(recipe, dict) or not isinstance(recipe.get("id"), str):
        raise JourneyError("selected recipe has no stable id")
    symptoms = recipe.get("symptoms")
    if not isinstance(symptoms, list) or symptom not in symptoms:
        raise JourneyError("selected recipe does not declare the requested symptom")
    return discovery, recipe


def _probe_command(
    pulp: pathlib.Path, recipe: dict[str, Any], artifact_dir: pathlib.Path
) -> list[str]:
    try:
        command = list(recipe["entrypoints"]["cli"]["command"])
    except (KeyError, TypeError) as exc:
        raise JourneyError("selected recipe has no CLI command") from exc
    if not command or command[0] != "pulp" or "<absolute-artifact-dir>" not in command:
        raise JourneyError("selected recipe command is not executable by this harness")
    command[0] = str(pulp)
    command[command.index("<absolute-artifact-dir>")] = str(artifact_dir)
    return command


def _verified_regular_file(
    path: pathlib.Path, artifact_dir: pathlib.Path
) -> tuple[int, str]:
    try:
        if artifact_dir.is_symlink() or not artifact_dir.is_dir():
            raise JourneyError("artifact directory must be a real directory")
        resolved_dir = artifact_dir.resolve(strict=True)
        if path.parent.resolve(strict=True) != resolved_dir:
            raise JourneyError("declared artifact escapes its evidence directory")
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except OSError as exc:
        raise JourneyError(f"declared artifact is unavailable: {path.name}") from exc
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            raise JourneyError(f"declared artifact is not one confined regular file: {path.name}")
        if metadata.st_size > MAX_TOTAL_ARTIFACT_BYTES:
            raise JourneyError(f"declared artifact exceeds its size bound: {path.name}")
        digest = hashlib.sha256()
        with os.fdopen(descriptor, "rb") as stream:
            descriptor = -1
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return metadata.st_size, digest.hexdigest()
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _verified_artifacts(
    evidence: dict[str, Any], artifact_dir: pathlib.Path, containment_root: pathlib.Path
) -> dict[str, str]:
    artifacts = evidence.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts or len(artifacts) > MAX_ARTIFACTS:
        raise JourneyError("probe artifact declaration is empty or exceeds its count bound")
    try:
        resolved_root = containment_root.resolve(strict=True)
        resolved_artifact_dir = artifact_dir.resolve(strict=True)
    except OSError as exc:
        raise JourneyError("artifact directory is unavailable") from exc
    if not resolved_artifact_dir.is_relative_to(resolved_root):
        raise JourneyError("artifact directory escapes its evidence root")
    hashes: dict[str, str] = {}
    total_bytes = 0
    for artifact in artifacts:
        if not isinstance(artifact, dict) or set(artifact) != {
            "name", "kind", "mime", "bytes", "sha256"
        }:
            raise JourneyError("probe artifact declaration is malformed")
        name = artifact.get("name")
        kind = artifact.get("kind")
        mime = artifact.get("mime")
        declared_bytes = artifact.get("bytes")
        declared_hash = artifact.get("sha256")
        if (
            not isinstance(name, str) or not name or len(name) > 240 or "\\" in name
            or name in {".", ".."} or pathlib.PurePosixPath(name).name != name
            or name in hashes or kind not in ARTIFACT_KINDS
            or not isinstance(mime, str) or not mime or len(mime) > 128
            or type(declared_bytes) is not int or not 0 <= declared_bytes <= MAX_TOTAL_ARTIFACT_BYTES
            or not isinstance(declared_hash, str)
            or re.fullmatch(r"[0-9a-f]{64}", declared_hash) is None
        ):
            raise JourneyError("probe artifact declaration is unsafe or incomplete")
        actual_bytes, actual_hash = _verified_regular_file(artifact_dir / name, artifact_dir)
        if actual_bytes != declared_bytes or actual_hash != declared_hash:
            raise JourneyError(f"declared artifact bytes or digest do not match: {name}")
        total_bytes += actual_bytes
        if total_bytes > MAX_TOTAL_ARTIFACT_BYTES:
            raise JourneyError("probe artifacts exceed the 16 MiB aggregate bound")
        hashes[name] = actual_hash
    try:
        actual_names = {entry.name for entry in artifact_dir.iterdir()}
    except OSError as exc:
        raise JourneyError("artifact directory cannot be enumerated") from exc
    if actual_names != set(hashes):
        raise JourneyError("artifact directory contains undeclared or missing entries")
    return dict(sorted(hashes.items()))


def _validate_result_schema_shape(evidence: Any) -> None:
    if not isinstance(evidence, dict) or set(evidence) != RESULT_FIELDS:
        raise JourneyError("probe result does not have the closed v1 field set")
    dimensions = evidence["dimensions"]
    if (
        not isinstance(dimensions, dict)
        or set(dimensions) != {"width", "height", "work_items"}
        or type(dimensions["width"]) is not int or not 1 <= dimensions["width"] <= 4096
        or type(dimensions["height"]) is not int or not 1 <= dimensions["height"] <= 4096
        or type(dimensions["work_items"]) is not int
        or not 1 <= dimensions["work_items"] <= 1_048_576
    ):
        raise JourneyError("probe result dimensions violate the v1 contract")
    tolerance = evidence["tolerance"]
    if (
        not isinstance(tolerance, dict) or set(tolerance) != {"absolute", "relative"}
        or any(
            isinstance(tolerance[field], bool)
            or not isinstance(tolerance[field], (int, float))
            or not math.isfinite(tolerance[field]) or tolerance[field] < 0
            for field in ("absolute", "relative")
        )
    ):
        raise JourneyError("probe result tolerance violates the v1 contract")
    adapter = evidence["adapter"]
    adapter_fields = {"status", "class", "backend", "name", "vendor", "architecture", "device"}
    if (
        not isinstance(adapter, dict) or set(adapter) != adapter_fields
        or adapter["status"] not in {"authentic", "unverified", "unavailable"}
        or adapter["class"] not in {"hardware", "software", "null", "unknown"}
        or any(
            value is not None and (not isinstance(value, str) or len(value) > 256)
            for value in (adapter[field] for field in adapter_fields - {"status", "class"})
        )
    ):
        raise JourneyError("probe result adapter violates the v1 contract")
    if any(
        not isinstance(evidence[field], str) or not 1 <= len(evidence[field]) <= 64
        for field in ("clock", "input_format", "output_format", "encoding")
    ):
        raise JourneyError("probe result execution strings violate the v1 contract")
    if (
        type(evidence["seed"]) is not int or evidence["seed"] < 0
        or evidence["adapter_policy"] not in {"hardware-required", "hardware-preferred", "any-supported"}
        or type(evidence["numeric_sample_count"]) is not int
        or not 0 <= evidence["numeric_sample_count"] <= 4096
        or (
            evidence["mutation"] is not None
            and (not isinstance(evidence["mutation"], str) or len(evidence["mutation"]) > 128)
        )
        or evidence["verdict"] not in {"pass", "fail", "unavailable", "unverified"}
    ):
        raise JourneyError("probe result execution identity violates the v1 contract")
    recommendations = evidence["recommendations"]
    if (
        not isinstance(recommendations, list) or len(recommendations) > 16
        or any(not isinstance(item, str) or not 1 <= len(item) <= 512 for item in recommendations)
    ):
        raise JourneyError("probe result recommendations violate the v1 contract")


def _typed_passes(evidence: dict[str, Any]) -> list[dict[str, Any]]:
    passes = evidence.get("passes")
    if not isinstance(passes, list) or not passes or len(passes) > 16:
        raise JourneyError("probe evidence has no pass-level work evidence")
    required = {
        "sequence", "name", "verdict", "work_completed", "expected",
        "observed", "absolute_error", "code",
    }
    for index, item in enumerate(passes):
        if not isinstance(item, dict) or set(item) != required:
            raise JourneyError("every probe pass must be a complete typed object")
        if (
            type(item["sequence"]) is not int or item["sequence"] != index
            or not isinstance(item["name"], str) or not item["name"] or len(item["name"]) > 64
            or item["verdict"] not in {"pass", "fail", "unavailable", "unverified"}
            or type(item["work_completed"]) is not bool
            or not isinstance(item["code"], str) or not item["code"] or len(item["code"]) > 128
            or any(
                value is not None and (
                    isinstance(value, bool) or not isinstance(value, (int, float))
                    or not math.isfinite(value)
                )
                for value in (item["expected"], item["observed"], item["absolute_error"])
            )
        ):
            raise JourneyError("probe pass fields are not a valid ordered typed sequence")
        expected = item["expected"]
        observed = item["observed"]
        absolute_error = item["absolute_error"]
        if item["verdict"] == "pass" and item["work_completed"] is not True:
            raise JourneyError("a passing probe pass must prove work completed")
        if (expected is None) != (observed is None) or (expected is None) != (absolute_error is None):
            raise JourneyError("numeric evidence fields must appear together")
        if absolute_error is not None:
            if absolute_error < 0.0:
                raise JourneyError("absolute error must be nonnegative")
            calculated = abs(observed - expected)
            epsilon = sys.float_info.epsilon * max(1.0, calculated, absolute_error) * 8.0
            if abs(calculated - absolute_error) > epsilon:
                raise JourneyError("absolute error does not match observed minus expected")
    return passes


def _validate_evidence(
    evidence: dict[str, Any], recipe_id: str, artifact_dir: pathlib.Path,
    containment_root: pathlib.Path,
) -> dict[str, str]:
    _validate_result_schema_shape(evidence)
    if evidence.get("schema") != RESULT_SCHEMA or evidence.get("version") != 1:
        raise JourneyError("probe evidence is not the supported result schema")
    if evidence.get("recipe_id") != recipe_id:
        raise JourneyError("probe evidence does not bind the selected recipe")
    evidence_id = evidence.get("gpu_evidence_id")
    if not isinstance(evidence_id, str) or re.fullmatch(r"[0-9a-f]{32}", evidence_id) is None:
        raise JourneyError("probe evidence has no correlation id")
    for field in ("source_digest", "signature_digest"):
        value = evidence.get(field)
        if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
            raise JourneyError(f"probe evidence has no valid {field}")
    passes = _typed_passes(evidence)
    if not any(item["work_completed"] is True for item in passes):
        raise JourneyError("probe evidence does not prove work completed")
    aggregate = "pass"
    for item in passes:
        if item["verdict"] == "fail":
            aggregate = "fail"
        elif aggregate != "fail" and item["verdict"] == "unavailable":
            aggregate = "unavailable"
        elif aggregate == "pass" and item["verdict"] == "unverified":
            aggregate = "unverified"
    if evidence.get("verdict") != aggregate:
        raise JourneyError("top-level verdict does not aggregate the typed passes")
    return _verified_artifacts(evidence, artifact_dir, containment_root)


def _authentic_adapter(evidence: dict[str, Any]) -> dict[str, Any]:
    adapter = evidence.get("adapter")
    fields = ("status", "class", "backend", "name", "vendor", "architecture", "device")
    identity_fields = ("status", "class", "backend", "name", "vendor", "architecture")
    placeholders = {"unknown", "generic", "n/a", "none", "null", "unavailable"}
    if (
        evidence.get("adapter_policy") != "hardware-required"
        or not isinstance(adapter, dict) or adapter.get("status") != "authentic"
        or adapter.get("class") != "hardware"
        or any(
            not isinstance(adapter.get(field), str) or not adapter[field]
            for field in identity_fields
        )
        or any(adapter[field].strip().casefold() in placeholders for field in identity_fields)
    ):
        raise JourneyError("acceptance requires authentic hardware adapter evidence")
    if adapter["backend"] == "Metal":
        raw_device = adapter.get("device")
        device_match = (
            re.fullmatch(r"vendor=0x([0-9a-f]+),device=0x([0-9a-f]+)", raw_device)
            if isinstance(raw_device, str) and raw_device.strip().casefold() not in placeholders
            else None
        )
        if (
            adapter["vendor"].casefold() != "apple"
            or re.fullmatch(r"Apple (?:M|A)[1-9][0-9]*(?: Pro| Max| Ultra)?", adapter["name"])
            is None
            or re.fullmatch(r"metal-[1-9][0-9]*", adapter["architecture"]) is None
            or (device_match is not None and int(device_match.group(1), 16) != 0x106B)
            # Dawn omits the optional numeric identity when Metal exposes no
            # registry/PCI-style IDs. Exactly JSON null is allowed behind the
            # strict Apple identity above; placeholders and malformed values
            # remain failures.
            or (raw_device is not None and device_match is None)
        ):
            raise JourneyError("Metal adapter evidence has no concrete Apple identity")
    elif (
        not isinstance(adapter.get("device"), str)
        or not adapter["device"]
        or adapter["device"].strip().casefold() in placeholders
    ):
        raise JourneyError("non-Metal adapter evidence has no concrete device identity")
    return {key: adapter[key] for key in fields}


def _pass_contract(evidence: dict[str, Any]) -> list[str]:
    return [item["name"] for item in _typed_passes(evidence)]


def _artifact_contract(evidence: dict[str, Any]) -> dict[str, dict[str, Any]]:
    artifacts = evidence.get("artifacts")
    if not isinstance(artifacts, list):
        raise JourneyError("probe evidence has no artifact contract")
    return {
        item["name"]: {"kind": item["kind"], "mime": item["mime"], "bytes": item["bytes"]}
        for item in artifacts
    }


def _execution_contract(evidence: dict[str, Any]) -> dict[str, Any]:
    return {
        field: evidence[field]
        for field in (
            "dimensions", "seed", "clock", "input_format", "output_format",
            "encoding", "tolerance", "adapter_policy", "numeric_sample_count",
        )
    }


def _partition_artifact_changes(
    reference_hashes: dict[str, str], negative_hashes: dict[str, str]
) -> tuple[list[str], list[str]]:
    if set(reference_hashes) != set(negative_hashes):
        raise JourneyError("negative run changed the recipe's artifact identities")
    changed = sorted(name for name in reference_hashes if reference_hashes[name] != negative_hashes[name])
    stable = sorted(set(reference_hashes) - set(changed))
    if not changed or not stable:
        raise JourneyError("negative mutation must preserve and change bounded artifacts")
    return stable, changed


def _snapshot_tree(root: pathlib.Path) -> dict[str, Any]:
    _require_real_directory(root)
    _resource_usage(ResourceRoot(root))
    entries: list[dict[str, Any]] = []
    total = 0

    def inspect(directory: pathlib.Path) -> None:
        nonlocal total
        try:
            directory_entries = sorted(os.scandir(directory), key=lambda item: item.name)
        except OSError as exc:
            raise JourneyError(f"tree cannot be enumerated: {directory}") from exc
        for entry in directory_entries:
            relative = pathlib.Path(entry.path).relative_to(root).as_posix()
            metadata = entry.stat(follow_symlinks=False)
            mode = f"{stat.S_IMODE(metadata.st_mode):04o}"
            if stat.S_ISDIR(metadata.st_mode):
                entries.append({"path": relative, "type": "directory", "mode": mode})
                inspect(pathlib.Path(entry.path))
            elif stat.S_ISREG(metadata.st_mode):
                payload = _read_regular_bytes(pathlib.Path(entry.path), MAX_TOTAL_ARTIFACT_BYTES)
                total += len(payload)
                entries.append({
                    "path": relative,
                    "type": "file",
                    "mode": mode,
                    "bytes": len(payload),
                    "sha256": _sha256_bytes(payload),
                })
            else:
                raise JourneyError(f"tree contains a symlink or special entry: {relative}")

    inspect(root)
    canonical = json.dumps(entries, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return {
        "sha256": _sha256_bytes(canonical),
        "entry_count": len(entries),
        "total_file_bytes": total,
        "entries": entries,
    }


def _tree_diff(before: dict[str, Any], after: dict[str, Any]) -> dict[str, list[str]]:
    before_entries = {item["path"]: item for item in before["entries"]}
    after_entries = {item["path"]: item for item in after["entries"]}
    return {
        "added": sorted(set(after_entries) - set(before_entries)),
        "removed": sorted(set(before_entries) - set(after_entries)),
        "modified": sorted(
            path for path in set(before_entries) & set(after_entries)
            if before_entries[path] != after_entries[path]
        ),
    }


def _seed_script() -> str:
    return r'''#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || -z "$1" ]]; then
  echo "usage: ./run-probe.sh <exact-symptom-token>" >&2
  exit 2
fi

workspace="$(pwd -P)"
script_dir="$(cd "$(dirname "$0")" && pwd -P)"
if [[ "$workspace" != "$script_dir" ]]; then
  echo "run-probe.sh must run from its workspace" >&2
  exit 2
fi

symptom="$1"
pulp_path="$(command -v pulp)"
printf 'cwd: %s\nPATH: %s\ninstalled pulp: %s\n' "$workspace" "$PATH" "$pulp_path" >&2

discovery_path="$workspace/discovery.json"
pulp gpu recipes list --symptom "$symptom" --json > "$discovery_path"
recipe_id="$(python3 - "$discovery_path" <<'PY'
import json
import pathlib
import sys

value = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
matches = value.get("recipes")
if value.get("schema") != "pulp.gpu-recipes-discovery.v1" or not isinstance(matches, list) or len(matches) != 1:
    raise SystemExit("symptom did not select exactly one recipe")
match = matches[0]
recipe = match.get("recipe") if isinstance(match, dict) else None
if match.get("callable") is not True or not isinstance(recipe, dict) or not isinstance(recipe.get("id"), str):
    raise SystemExit("selected recipe is not callable")
print(recipe["id"])
PY
)"

seeded_option="--negative-control"
run_kind="repaired"
probe=(pulp gpu probe --recipe "$recipe_id")
if [[ -n "$seeded_option" ]]; then
  run_kind="negative"
  probe+=("$seeded_option")
fi
artifact_dir="$workspace/artifacts/$run_kind"
result_path="$workspace/$run_kind-result.json"
if [[ -e "$artifact_dir" || -L "$artifact_dir" || -e "$result_path" || -L "$result_path" ]]; then
  echo "refusing to overwrite an existing $run_kind result" >&2
  exit 2
fi
probe+=(--artifacts "$artifact_dir" --json)

printf 'selected recipe: %s\nrunning:' "$recipe_id" >&2
printf ' %q' "${probe[@]}" >&2
printf '\n' >&2
set +e
"${probe[@]}" > "$result_path"
probe_rc=$?
set -e
cat "$result_path"
printf 'probe exit: %d\n' "$probe_rc" >&2
exit "$probe_rc"
'''


def _task_text() -> str:
    return """# Independent GPU recipe repair

Use only the installed `pulp` on `PATH`, this workspace, the exact symptom from
the prompt, and the public catalog and documentation installed beside that
binary. Derive the install prefix from `command -v pulp`; do not use a source
checkout, a private plan, a remembered recipe ID, or network documentation.

Start only from the symptom. Query the installed CLI catalog, inspect the
installed catalog file, and follow its installed public documentation for the
unique callable recipe. Then run `./run-probe.sh <symptom>`. A completed typed
failure exits 1. Read its pass-level code, mutation, expected value, observed
value, and absolute error. Diagnose the single incorrect option in
`run-probe.sh` from those public surfaces, change only that option, preserve all
receipts and artifacts, and rerun the same script with the same symptom.
"""


def _prompt_text(workspace: pathlib.Path, symptom: str, run_nonce: str) -> str:
    return f"""You are the fresh independent agent for one GPU recipe usability acceptance run.

You have no Pulp source checkout, no recipe ID, and no prior task context. Use
only the installed `pulp` on PATH, the workspace below, the exact symptom token,
and the public catalog/documentation installed beside that binary. Do not use
network documentation.

Workspace: {workspace}
Symptom token: {symptom}
Fresh run nonce: {run_nonce}

Work only inside the workspace. First run `pwd` and `command -v pulp`. Read
`TASK.md`; derive the install prefix from the installed binary; query the CLI by
symptom; inspect the installed `share/pulp/gpu-recipes.yaml`; and read at least
one installed public document named by the selected catalog row before editing.
Run `./run-probe.sh {symptom}`, interpret the completed typed failure, make only
the diagnosed option correction, and rerun it. Your final response must satisfy
the supplied JSON schema and repeat the fresh run nonce exactly. Do not search
for or read any source checkout, private plan, private verifier case, or
remembered recipe ID.
"""


def _final_output_schema(run_nonce: str, symptom: str) -> dict[str, Any]:
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "type": "object",
        "additionalProperties": False,
        "required": [
            "run_nonce", "symptom", "selected_recipe", "negative", "edit", "repaired",
        ],
        "properties": {
            "run_nonce": {"const": run_nonce},
            "symptom": {"const": symptom},
            "selected_recipe": {"type": "string", "minLength": 1, "maxLength": 128},
            "negative": {
                "type": "object", "additionalProperties": False,
                "required": ["exit_code", "verdict", "pass", "code", "mutation",
                             "expected", "observed", "absolute_error"],
                "properties": {
                    "exit_code": {"const": 1}, "verdict": {"const": "fail"},
                    "pass": {"type": "string", "minLength": 1, "maxLength": 128},
                    "code": {"type": "string", "minLength": 1, "maxLength": 128},
                    "mutation": {"type": "string", "minLength": 1, "maxLength": 256},
                    "expected": {}, "observed": {}, "absolute_error": {},
                },
            },
            "edit": {
                "type": "object", "additionalProperties": False,
                "required": ["path", "removed", "added"],
                "properties": {
                    "path": {"const": "run-probe.sh"},
                    "removed": {"type": "string", "minLength": 1, "maxLength": 256},
                    "added": {"type": "string", "maxLength": 256},
                },
            },
            "repaired": {
                "type": "object", "additionalProperties": False,
                "required": ["exit_code", "verdict"],
                "properties": {"exit_code": {"const": 0}, "verdict": {"const": "pass"}},
            },
        },
    }


def _require_revision(value: str, label: str) -> str:
    if re.fullmatch(r"[0-9a-f]{40}", value) is None:
        raise JourneyError(f"{label} must be an exact lowercase 40-hex revision")
    return value


def _installed_cli_linkage_digest(
    pulp: pathlib.Path,
    forbidden_roots: Iterable[pathlib.Path],
    cwd: pathlib.Path,
    timeout_seconds: float,
) -> str:
    if sys.platform != "darwin":
        return _sha256_bytes(f"linkage-audit-not-applicable:{sys.platform}".encode("utf-8"))
    outputs: list[str] = []
    for option in ("-l", "-L"):
        result = _run(
            ["/usr/bin/otool", option, str(pulp)],
            min(timeout_seconds, 30.0),
            cwd=cwd,
            monitor_roots=[ResourceRoot(cwd)],
        )
        if result.returncode != 0:
            raise JourneyError("installed CLI linkage could not be inspected")
        outputs.append(result.stdout)
    linkage = "\n".join(outputs)
    if any(str(root.resolve(strict=True)) in linkage for root in forbidden_roots):
        raise JourneyError("installed CLI linkage exposes a source-checkout path")
    if "@rpath/" in outputs[1] and "@executable_path/../lib" not in outputs[0]:
        raise JourneyError("installed CLI does not resolve private runtimes from its install prefix")
    return _sha256_bytes(linkage.encode("utf-8"))


def prepare_case(
    pulp: pathlib.Path,
    symptom: str,
    workspace: pathlib.Path,
    case_dir: pathlib.Path,
    source_root: pathlib.Path,
    build_root: pathlib.Path,
    cli_install_script: pathlib.Path,
    installed_prefix: pathlib.Path,
    plan_root: pathlib.Path,
    plan_document: pathlib.Path,
    forbidden_roots: list[pathlib.Path],
    timeout_seconds: float,
) -> dict[str, Any]:
    if not math.isfinite(timeout_seconds) or not 1.0 <= timeout_seconds <= 1800.0:
        raise JourneyError("--timeout-seconds must be between 1 and 1800 seconds")
    if not symptom or symptom.strip() != symptom:
        raise JourneyError("--symptom must be a non-empty exact token")
    default_source = pathlib.Path(__file__).resolve(strict=True).parents[2]
    source_root = source_root.resolve(strict=True)
    plan_root = plan_root.resolve(strict=True)
    build_root = build_root.resolve(strict=True)
    installed_prefix = installed_prefix.resolve(strict=True)
    if source_root != default_source:
        raise JourneyError("--source-root must be the exact checkout containing this harness")
    if source_root == plan_root or _is_within(source_root, plan_root) or _is_within(plan_root, source_root):
        raise JourneyError("source and canonical plan must be distinct repositories")
    if plan_document.is_absolute() or any(part in {"", ".", ".."} for part in plan_document.parts):
        raise JourneyError("--plan-document must be a normalized repository-relative path")
    plan_document_path = plan_root / plan_document
    if plan_document_path.resolve(strict=True) != plan_document_path:
        raise JourneyError("--plan-document must not traverse a symlink")

    # Check the Release-only build boundary before source cleanliness. The
    # required macOS gate is intentionally Debug and uses this ordering to
    # prove configuration rejection independently of checkout state.
    trust.require_release_build_configuration(
        build_root=build_root, source_root=source_root
    )
    source = trust.git_repository_identity(
        source_root, expected_repository="Generous-Corp/pulp"
    )
    plan = trust.git_repository_identity(
        plan_root,
        expected_repository="danielraffel/pulp-planning",
        required_document=plan_document_path,
        require_origin_main=True,
    )
    all_forbidden = list(dict.fromkeys([
        source_root, build_root, plan_root,
        *(root.resolve(strict=True) for root in forbidden_roots),
    ]))
    _validate_isolation(workspace, case_dir, all_forbidden)
    _validate_install_isolation(workspace, case_dir, installed_prefix)
    if any(
        _is_within(installed_prefix, root) or _is_within(root, installed_prefix)
        for root in all_forbidden
    ):
        raise JourneyError("installed prefix must be isolated from source, build, and plan roots")
    os.mkdir(workspace, 0o755)
    os.mkdir(case_dir, 0o700)
    build, installed_identity = trust.build_install_identity(
        source=source,
        build_root=build_root,
        install_script=cli_install_script,
        installed_prefix=installed_prefix,
        installed_pulp=pulp,
        timeout=timeout_seconds,
    )
    pulp = pathlib.Path(installed_identity["path"])
    linkage_sha256 = _installed_cli_linkage_digest(
        pulp, all_forbidden, case_dir, timeout_seconds
    )
    public_material = trust.installed_public_material(source_root, installed_prefix)
    record_key = trust.create_record_keypair(case_dir)
    run_nonce = secrets.token_hex(32)

    _write_bytes(workspace / "run-probe.sh", _seed_script().encode("utf-8"))
    os.chmod(workspace / "run-probe.sh", 0o755)
    _write_bytes(workspace / "TASK.md", _task_text().encode("utf-8"))
    initial_tree = _snapshot_tree(workspace)

    discovery, recipe = _select_recipe(pulp, symptom, timeout_seconds, case_dir)
    recipe_id = recipe["id"]
    reference_dir = case_dir / "reference-artifacts"
    command = _probe_command(pulp, recipe, reference_dir)
    reference_result = _run(
        command,
        timeout_seconds,
        cwd=case_dir,
        monitor_roots=[ResourceRoot(case_dir)],
    )
    if reference_result.returncode == 2:
        raise JourneyUnavailable("reference recipe evidence is unavailable or unverified")
    if reference_result.returncode != 0:
        raise JourneyError(f"reference recipe did not pass (exit {reference_result.returncode})")
    reference = _json_stdout(reference_result, "reference probe")
    reference_hashes = _validate_evidence(reference, recipe_id, reference_dir, case_dir)
    adapter = _authentic_adapter(reference)
    if reference.get("verdict") != "pass" or reference.get("mutation") is not None:
        raise JourneyError("reference probe is not an unmutated pass")
    if any(item["verdict"] != "pass" for item in _typed_passes(reference)):
        raise JourneyError("reference probe contains a non-passing semantic pass")
    reference_path = case_dir / "reference-result.json"
    _write_json(reference_path, reference)

    version_result = _run(
        [str(pulp), "version"], timeout_seconds,
        cwd=case_dir, monitor_roots=[ResourceRoot(case_dir)],
    )
    if version_result.returncode != 0:
        raise JourneyError("installed CLI did not report its version")
    version_match = re.search(r"(?m)^Pulp SDK version: ([0-9]+\.[0-9]+\.[0-9]+)$", version_result.stdout)
    if version_match is None or version_match.group(1) != build["build_info"]["kSdkVersion"]:
        raise JourneyError("installed CLI version does not match generated Release build provenance")
    prompt = _prompt_text(workspace, symptom, run_nonce)
    if recipe_id in prompt:
        raise JourneyError("independent prompt must not disclose the selected recipe ID")
    prompt_path = case_dir / "agent-prompt.txt"
    _write_bytes(prompt_path, prompt.encode("utf-8"))

    identity_seed = {
        "symptom": symptom,
        "workspace": str(workspace),
        "forbidden_roots": sorted({str(root.resolve(strict=True)) for root in all_forbidden}),
        "source_revision": source["revision"],
        "plan_revision": plan["revision"],
        "pulp_binary_sha256": _sha256(pulp, MAX_BINARY_BYTES),
        "initial_tree_sha256": initial_tree["sha256"],
        "reference_result_sha256": _sha256(reference_path),
        "run_nonce": run_nonce,
        "record_public_key_sha256": record_key["public_key_sha256"],
        "limits": _v4_limits(),
    }
    case_id = _sha256_bytes(
        json.dumps(identity_seed, sort_keys=True, separators=(",", ":")).encode("utf-8")
    )[:32]
    case = {
        "schema": CASE_SCHEMA,
        "status": "prepared-structural-nonterminal",
        "case_id": case_id,
        "symptom": symptom,
        "workspace": str(workspace),
        "forbidden_roots": identity_seed["forbidden_roots"],
        "source_revision": source["revision"],
        "plan_revision": plan["revision"],
        "source": source,
        "plan": plan,
        "build": build,
        "public_material": public_material,
        "run_nonce": run_nonce,
        "record_key": record_key,
        "limits": _v4_limits(),
        "installed_cli": {
            **installed_identity,
            "path": str(pulp),
            "version_output": version_result.stdout.strip(),
            "version_output_sha256": _sha256_bytes(version_result.stdout.encode("utf-8")),
            "linkage_sha256": linkage_sha256,
        },
        "harnesses": {
            "journey_sha256": _sha256(pathlib.Path(__file__).resolve(strict=True)),
            "trust_sha256": _sha256(pathlib.Path(trust.__file__).resolve(strict=True)),
            "isolation_sha256": _sha256(
                pathlib.Path(trust.isolation.__file__).resolve(strict=True)
            ),
        },
        "prompt": {
            "path": str(prompt_path),
            "sha256": _sha256(prompt_path),
            "bytes": prompt_path.stat().st_size,
        },
        "initial_tree": initial_tree,
        "correction": {"path": "run-probe.sh", "before": SEED_LINE, "after": REPAIRED_LINE},
        "selection": {
            "catalog_schema": CATALOG_SCHEMA,
            "catalog_revision": discovery["catalog_revision"],
            "recipe_id": recipe_id,
        },
        "reference": {
            "command": command,
            "exit_code": reference_result.returncode,
            "result_path": str(reference_path),
            "result_json_sha256": _sha256(reference_path),
            "result": reference,
            "artifacts_sha256": reference_hashes,
            "adapter": adapter,
        },
    }
    _require_v4_structural_nonterminal(case, "prepared v4 case")
    case_path = case_dir / "case.json"
    _write_json(case_path, case)
    return {
        "schema": CASE_SCHEMA,
        "status": case["status"],
        "case_id": case_id,
        "workspace": str(workspace),
        "private_case": str(case_path),
        "agent_prompt": str(prompt_path),
    }


def _parse_transcript(payload: bytes) -> tuple[list[dict[str, Any]], str]:
    events: list[dict[str, Any]] = []
    for line_number, line in enumerate(payload.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            raise JourneyError(f"agent transcript line {line_number} is not JSON") from exc
        if not isinstance(event, dict) or not isinstance(event.get("type"), str):
            raise JourneyError("every agent transcript event needs a string type")
        events.append(event)
        if len(events) > MAX_BUNDLE_EVENTS:
            raise JourneyError("agent transcript exceeds its event-count cap")
    if (
        len(events) < 3 or events[0].get("type") != "thread.started"
        or events[1].get("type") != "turn.started"
        or events[-1].get("type") != "turn.completed"
    ):
        raise JourneyError("agent transcript must contain one ordered started/completed turn")
    thread_id = events[0].get("thread_id")
    if not isinstance(thread_id, str) or trust.UUID_RE.fullmatch(thread_id) is None:
        raise JourneyError("thread.started has no session identity")
    types = [event["type"] for event in events]
    if (
        types.count("thread.started") != 1
        or types.count("turn.started") != 1
        or types.count("turn.completed") != 1
        or any(item in types for item in ("turn.failed", "item.failed", "error"))
    ):
        raise JourneyError("agent transcript does not contain one successful completed turn")
    return events, thread_id


def _string_corpus(value: Any) -> str:
    strings: list[str] = []

    def collect(item: Any) -> None:
        if isinstance(item, str):
            strings.append(item)
        elif isinstance(item, dict):
            for key, child in item.items():
                strings.append(str(key))
                collect(child)
        elif isinstance(item, list):
            for child in item:
                collect(child)

    collect(value)
    return "\n".join(strings)


def _fresh_agent_environment(
    pulp: pathlib.Path, workspace: pathlib.Path, agent_home: pathlib.Path,
    agent_config: pathlib.Path, agent_codex_home: pathlib.Path, proxy_port: int,
) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items() if key in AGENT_INHERITED_ENV_KEYS
    }
    environment.update({
        "HOME": str(agent_home),
        "XDG_CONFIG_HOME": str(agent_config),
        "CODEX_HOME": str(agent_codex_home),
        "TMPDIR": str(agent_home / "tmp"),
        "PATH": os.pathsep.join([str(pulp.parent), os.defpath]),
        "PWD": str(workspace),
        "PULP_UPDATE_CHECK_DISABLED": "1",
        "NO_COLOR": "1",
        "HTTPS_PROXY": f"http://127.0.0.1:{proxy_port}",
        "HTTP_PROXY": f"http://127.0.0.1:{proxy_port}",
        "NO_PROXY": "",
    })
    return environment


def _credential_strings(value: Any) -> list[str]:
    strings: list[str] = []

    def visit(item: Any) -> None:
        if isinstance(item, str) and len(item) >= 8:
            strings.append(item)
        elif isinstance(item, dict):
            for child in item.values():
                visit(child)
        elif isinstance(item, list):
            for child in item:
                visit(child)

    visit(value)
    return strings


def _agent_auth_material() -> tuple[bytes, list[str], tuple[str, ...]]:
    """Return minimal compact auth for keychain staging and its endpoint set."""

    if api_key := os.environ.get("OPENAI_API_KEY"):
        auth = {"auth_mode": "apikey", "OPENAI_API_KEY": api_key}
        payload = json.dumps(auth, sort_keys=True, separators=(",", ":")).encode("utf-8")
        return payload, _credential_strings(auth), ("api.openai.com",)
    host_root = pathlib.Path(
        os.environ.get("CODEX_HOME", str(pathlib.Path.home() / ".codex"))
    ).expanduser()
    source = host_root / "auth.json"
    payload = _read_regular_bytes(source, 1024 * 1024)
    try:
        auth = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise JourneyError("Codex authentication is not bounded UTF-8 JSON") from exc
    if not isinstance(auth, dict):
        raise JourneyError("Codex authentication JSON root is not an object")
    if auth.get("auth_mode") != "chatgpt" or not isinstance(auth.get("tokens"), dict):
        raise JourneyError("A5 supports only ChatGPT-token or OPENAI_API_KEY Codex auth")
    token_keys = {"access_token", "account_id", "id_token", "refresh_token"}
    tokens = auth["tokens"]
    if set(tokens) != token_keys or any(
        not isinstance(tokens.get(key), str) or not tokens[key] for key in token_keys
    ):
        raise JourneyError("ChatGPT Codex authentication token set is not closed")
    minimal = {
        "auth_mode": "chatgpt",
        "tokens": {key: tokens[key] for key in sorted(token_keys)},
        "last_refresh": auth.get("last_refresh"),
    }
    if not isinstance(minimal["last_refresh"], str) or not minimal["last_refresh"]:
        raise JourneyError("ChatGPT Codex authentication has no refresh identity")
    compact = json.dumps(minimal, sort_keys=True, separators=(",", ":")).encode("utf-8")
    credentials = sorted(set(_credential_strings(minimal)), key=len, reverse=True)
    return compact, credentials, ("auth.openai.com", "chatgpt.com")


def _codex_exec_config_arguments(environment: dict[str, str]) -> list[str]:
    shell_values = {
        key: environment[key]
        for key in ("HOME", "TMPDIR", "PATH", "PWD", "PULP_UPDATE_CHECK_DISABLED", "NO_COLOR")
    }
    inline = ", ".join(
        f"{key} = {json.dumps(value)}" for key, value in sorted(shell_values.items())
    )
    return [
        "-c", 'cli_auth_credentials_store="keyring"',
        "-c", "features.secret_auth_storage=false",
        "-c", 'web_search="disabled"',
        "-c", "check_for_update_on_startup=false",
        "-c", 'shell_environment_policy.inherit="none"',
        "-c", f"shell_environment_policy.set={{ {inline} }}",
    ]


def _reject_credential_disclosure(payloads: Iterable[bytes], credentials: Iterable[str]) -> None:
    material = tuple(payloads)
    for credential in credentials:
        encoded = credential.encode("utf-8")
        if any(encoded in payload for payload in material):
            raise JourneyError("agent outputs contain copied authentication material")


def _parse_agent_final_output(payload: bytes, case: dict[str, Any]) -> dict[str, Any]:
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise JourneyError("agent final message is not one UTF-8 JSON object") from exc
    if not isinstance(value, dict) or set(value) != {
        "run_nonce", "symptom", "selected_recipe", "negative", "edit", "repaired",
    }:
        raise JourneyError("agent final message does not have its closed result fields")
    negative = value.get("negative")
    edit = value.get("edit")
    repaired = value.get("repaired")
    if (
        value.get("run_nonce") != case["run_nonce"]
        or value.get("symptom") != case["symptom"]
        or not isinstance(value.get("selected_recipe"), str)
        or not isinstance(negative, dict)
        or set(negative) != {
            "exit_code", "verdict", "pass", "code", "mutation", "expected",
            "observed", "absolute_error",
        }
        or negative.get("exit_code") != 1 or negative.get("verdict") != "fail"
        or any(not isinstance(negative.get(key), str) or not negative[key]
               for key in ("pass", "code", "mutation"))
        or not isinstance(edit, dict) or set(edit) != {"path", "removed", "added"}
        or edit.get("path") != "run-probe.sh"
        or not isinstance(edit.get("removed"), str) or not edit["removed"]
        or not isinstance(edit.get("added"), str)
        or not isinstance(repaired, dict) or set(repaired) != {"exit_code", "verdict"}
        or repaired != {"exit_code": 0, "verdict": "pass"}
    ):
        raise JourneyError("agent final message violates the closed output contract")
    if len(payload) > MAX_BUNDLE_STRING_BYTES:
        raise JourneyError("agent final message exceeds its byte cap")
    return value


def record_agent(
    case_path: pathlib.Path,
    agent_bin: pathlib.Path,
    model: str,
    timeout_seconds: float,
) -> dict[str, Any]:
    if (
        not math.isfinite(timeout_seconds)
        or not 1.0 <= timeout_seconds <= MAX_AGENT_WALL_CLOCK_SECONDS
    ):
        raise JourneyError("--timeout-seconds must be between 1 and 900 seconds")
    _require_real_directory(case_path.parent)
    case = _load_json(case_path)
    if (
        case.get("schema") != CASE_SCHEMA
        or case.get("status") != "prepared-structural-nonterminal"
    ):
        raise JourneyError("record requires a nonterminal prepared case")
    _require_case_identity(case, case_path)
    _validate_case_material(case, case_path)
    workspace = pathlib.Path(case["workspace"])
    _require_real_directory(workspace)
    installed_prefix = pathlib.Path(case["installed_cli"]["prefix"])
    _require_real_directory(installed_prefix)
    _validate_install_isolation(workspace, case_path.parent, installed_prefix)
    forbidden_roots = [pathlib.Path(item) for item in case.get("forbidden_roots", [])]
    if not forbidden_roots or any(
        _is_within(workspace, root) or _is_within(case_path.parent, root)
        for root in forbidden_roots
    ):
        raise JourneyError("prepared paths are not isolated from recorded source trees")
    if _snapshot_tree(workspace) != case.get("initial_tree"):
        raise JourneyError("agent workspace changed after preparation")
    pulp = pathlib.Path(case["installed_cli"]["path"]).resolve(strict=True)
    if _sha256(pulp, MAX_BINARY_BYTES) != case["installed_cli"]["sha256"]:
        raise JourneyError("installed CLI bytes changed after preparation")
    prompt_path = pathlib.Path(case["prompt"]["path"])
    prompt_payload = _read_regular_bytes(prompt_path)
    if _sha256_bytes(prompt_payload) != case["prompt"]["sha256"]:
        raise JourneyError("agent prompt changed after preparation")
    prompt = prompt_payload.decode("utf-8")
    recipe_id = case["selection"]["recipe_id"]
    if recipe_id in prompt:
        raise JourneyError("agent prompt discloses the selected recipe ID")
    if not model or len(model) > 128 or re.fullmatch(r"[A-Za-z0-9._:-]+", model) is None:
        raise JourneyError("model identity is missing or malformed")
    agent_identity = trust.official_codex_identity(agent_bin)
    agent_bin = pathlib.Path(agent_identity["path"])

    case_dir = case_path.parent
    record_key = case["record_key"]
    private_key = pathlib.Path(record_key["private_key_path"])
    public_key = pathlib.Path(record_key["public_key_path"])
    if (
        not private_key.is_file() or private_key.is_symlink()
        or not public_key.is_file() or public_key.is_symlink()
        or _sha256(public_key) != record_key["public_key_sha256"]
    ):
        raise JourneyError("record requires its unused one-use signing keypair")
    output_paths = [
        case_dir / name for name in (
            "agent-session.json", "agent-transcript.jsonl", "agent-stderr.log",
            "agent-last-message.json", "codex-session.jsonl", "agent-seatbelt.sb",
            "agent-output-schema.json",
        )
    ]
    if any(path.exists() or path.is_symlink() for path in output_paths):
        raise JourneyError("record outputs already exist; prepare a fresh case")
    runtime_root = pathlib.Path(tempfile.mkdtemp(prefix="pulp-gpu-agent-runtime-")).resolve()
    _require_real_directory(runtime_root)
    os.chmod(runtime_root, 0o700)
    agent_home = runtime_root / "home"
    agent_config = runtime_root / "config"
    agent_codex_home = runtime_root / "codex"
    runtime_last_message = runtime_root / "last-message.txt"
    runtime_output_schema = runtime_root / "agent-output-schema.json"
    runtime_profile = runtime_root / "agent-seatbelt.sb"
    last_message_payload: bytes | None = None
    launched = False
    keychain_record: dict[str, Any] | None = None
    keychain_removed = False
    try:
        try:
            installed_prefix = pathlib.Path(case["installed_cli"]["prefix"])
            if (
                _is_within(runtime_root, workspace)
                or _is_within(runtime_root, case_dir)
                or _is_within(runtime_root, installed_prefix)
                or _is_within(installed_prefix, runtime_root)
                or any(_is_within(runtime_root, root) for root in forbidden_roots)
            ):
                raise JourneyError("temporary agent config root overlaps a source or evidence tree")
            for directory in (agent_home, agent_config, agent_codex_home, agent_home / "tmp"):
                os.mkdir(directory, 0o700)
            auth_payload, credential_values, endpoint_hosts = _agent_auth_material()
            keychain_record = trust.install_codex_keychain_auth(
                codex_home=agent_codex_home, agent_bin=agent_bin, auth_payload=auth_payload,
            )
            try:
                proxy = trust.ExactHostConnectProxy(endpoint_hosts)
                with proxy:
                    environment = _fresh_agent_environment(
                        pulp, workspace, agent_home, agent_config, agent_codex_home,
                        proxy.port,
                    )
                    if any(
                        root_text in value
                        for root_text in [str(case_dir), *case["forbidden_roots"]]
                        for value in environment.values()
                    ):
                        raise JourneyError("sanitized agent environment contains a denied read path")
                    profile_text = trust.seatbelt_profile(
                        workspace=workspace,
                        runtime_root=runtime_root,
                        installed_prefix=pathlib.Path(case["installed_cli"]["prefix"]),
                        agent_bin=agent_bin,
                        denied_roots=[case_dir, *forbidden_roots],
                        network_proxy_port=proxy.port,
                    )
                    _write_bytes(runtime_profile, profile_text.encode("utf-8"))
                    _write_json(
                        runtime_output_schema,
                        _final_output_schema(case["run_nonce"], case["symptom"]),
                    )
                    denied_probes = {
                        "private-case": case_path,
                        "source-checkout": pathlib.Path(case["source"]["root"]) / "AGENTS.md",
                        "canonical-plan": (
                            pathlib.Path(case["plan"]["root"])
                            / case["plan"]["document"]["path"]
                        ),
                        "build-tree": pathlib.Path(case["build"]["root"]) / "CMakeCache.txt",
                    }
                    preflight = trust.seatbelt_preflight(
                        profile_path=runtime_profile,
                        workspace=workspace,
                        denied_probes=denied_probes,
                        agent_bin=agent_bin,
                        agent_version=agent_identity["version"],
                        installed_pulp=pulp,
                        network_proxy_port=proxy.port,
                        codex_home=agent_codex_home,
                        agent_environment=environment,
                    )
                    if _snapshot_tree(workspace) != case["initial_tree"]:
                        raise JourneyError("Seatbelt traversal controls changed the pristine workspace")
                    command = [
                        "/usr/bin/sandbox-exec", "-f", str(runtime_profile),
                        str(agent_bin), *_codex_exec_config_arguments(environment),
                        "exec", "--json", "--ignore-user-config", "--ignore-rules",
                        "--skip-git-repo-check", "--sandbox", "danger-full-access",
                        "--color", "never", "--model", model, "--cd", str(workspace),
                        "--output-schema", str(runtime_output_schema),
                        "--output-last-message", str(runtime_last_message), prompt,
                    ]
                    launched = True
                    result = _run(
                        command,
                        timeout_seconds,
                        cwd=workspace,
                        env=environment,
                        monitor_roots=[ResourceRoot(workspace), ResourceRoot(runtime_root)],
                        stdout_limit=MAX_STDOUT_BYTES,
                        stderr_limit=MAX_STDERR_BYTES,
                    )
                proxy_audit = proxy.audit()
                if not any(
                    item.get("outcome") == "completed"
                    for item in proxy_audit["connections"]
                    if isinstance(item, dict)
                ):
                    raise JourneyError("Codex session did not use the allowlisted CONNECT transport")
                proxy_audit["environment"] = {
                    key: environment[key] for key in ("HTTPS_PROXY", "HTTP_PROXY", "NO_PROXY")
                }
            finally:
                trust.remove_codex_keychain_auth(keychain_record)
                keychain_removed = True
            last_message_payload = _read_regular_bytes(runtime_last_message)
            transcript_payload = result.stdout.encode("utf-8")
            events, thread_id = _parse_transcript(transcript_payload)
            if result.returncode != 0:
                raise JourneyError(f"fresh signed Codex exited {result.returncode}")
            final_output = _parse_agent_final_output(last_message_payload, case)
            rollout_paths = sorted(
                path for path in (agent_codex_home / "sessions").rglob("rollout-*.jsonl")
                if path.is_file() and not path.is_symlink()
            )
            if len(rollout_paths) != 1 or thread_id not in rollout_paths[0].name:
                raise JourneyError("record did not produce one thread-bound Codex session JSONL")
            rollout_payload = _read_regular_bytes(rollout_paths[0], MAX_METADATA_BYTES)
            rollout_identity = trust.parse_codex_rollout(
                rollout_payload,
                thread_id=thread_id,
                cli_version=agent_identity["cli_version"],
                workspace=workspace,
                model=model,
                run_nonce=case["run_nonce"],
            )
            _reject_credential_disclosure(
                (
                    transcript_payload,
                    result.stderr.encode("utf-8"),
                    last_message_payload,
                    rollout_payload,
                ),
                credential_values,
            )
            copied_profile = case_dir / "agent-seatbelt.sb"
            copied_schema = case_dir / "agent-output-schema.json"
            transcript_path = case_dir / "agent-transcript.jsonl"
            stderr_path = case_dir / "agent-stderr.log"
            last_message = case_dir / "agent-last-message.json"
            rollout_path = case_dir / "codex-session.jsonl"
            _write_bytes(copied_profile, _read_regular_bytes(runtime_profile))
            _write_bytes(copied_schema, _read_regular_bytes(runtime_output_schema))
            _write_bytes(transcript_path, transcript_payload)
            _write_bytes(stderr_path, result.stderr.encode("utf-8"))
            _write_bytes(last_message, last_message_payload)
            _write_bytes(rollout_path, rollout_payload)
        finally:
            shutil.rmtree(runtime_root)
        if last_message_payload is None:
            raise JourneyError("agent did not publish its bounded final message")
        if keychain_record is None or not keychain_removed:
            raise JourneyError("one-use Codex keychain authentication was not removed")
        final_tree = _snapshot_tree(workspace)
        session_core = {
            "schema": SESSION_SCHEMA,
            "status": "recorded-structural-nonterminal",
            "case_id": case["case_id"],
            "case_sha256": _sha256(case_path),
            "session_id": thread_id,
            "run_nonce": case["run_nonce"],
            "model": model,
            "cwd": str(workspace),
            "path": environment["PATH"],
            "home": str(agent_home),
            "config_home": str(agent_config),
            "codex_home": str(agent_codex_home),
            "runtime_removed": not runtime_root.exists() and not runtime_root.is_symlink(),
            "environment_keys": sorted(environment),
            "agent_binary": agent_identity,
            "keychain_auth": {**keychain_record, "removed_after_record": keychain_removed},
            "network_proxy": proxy_audit,
            "installed_cli_sha256": _sha256(pulp, MAX_BINARY_BYTES),
            "source_revision": case["source_revision"],
            "plan_revision": case["plan_revision"],
            "prompt_sha256": _sha256_bytes(prompt_payload),
            "command_argv": [
                *command[:-1], f"<prompt-sha256:{_sha256_bytes(prompt_payload)}>",
            ],
            "agent_exit_code": result.returncode,
            "seatbelt": {
                "authority": "outer-macos-seatbelt",
                "codex_internal_sandbox": "danger-full-access",
                "runtime_profile_path": str(runtime_profile),
                "network_proxy_port": proxy_audit["listen_port"],
                "denied_roots": [str(case_dir), *case["forbidden_roots"]],
                "profile": {
                    "path": str(copied_profile), "sha256": _sha256(copied_profile),
                    "bytes": copied_profile.stat().st_size,
                },
                "preflight": preflight,
            },
            "output_schema": {
                "path": str(copied_schema), "sha256": _sha256(copied_schema),
                "bytes": copied_schema.stat().st_size,
            },
            "transcript": {
                "path": str(transcript_path), "sha256": _sha256(transcript_path),
                "bytes": transcript_path.stat().st_size, "events": len(events),
            },
            "codex_rollout": {
                "path": str(rollout_path), "sha256": _sha256(rollout_path),
                "bytes": rollout_path.stat().st_size,
                "events": rollout_identity["event_count"],
                "turn_id": rollout_identity["turn_id"],
            },
            "stderr": {
                "path": str(stderr_path), "sha256": _sha256(stderr_path),
                "bytes": stderr_path.stat().st_size,
            },
            "last_message": {
                "path": str(last_message), "sha256": _sha256(last_message),
                "bytes": last_message.stat().st_size, "result": final_output,
            },
            "record_private_key_destroyed": True,
            "credential_disclosure_scan": "passed",
            "final_tree": final_tree,
        }
        _require_v4_structural_nonterminal(session_core, "recorded v4 session")
        replacements = _redaction_pairs(case, case_path, session_core)
        redacted_session_core = _redact_value(session_core, replacements)
        attestation = trust.sign_record(session_core, private_key)
        redacted_attestation = trust.sign_record(redacted_session_core, private_key)
        private_key.unlink()
        if private_key.exists() or private_key.is_symlink():
            raise JourneyError("one-use record signing key was not destroyed")
        session = {
            **session_core,
            "record_attestation": attestation,
            "redacted_record_attestation": redacted_attestation,
        }
        session_path = case_dir / "agent-session.json"
        _write_json(session_path, session)
        return session
    finally:
        if launched and (private_key.exists() or private_key.is_symlink()):
            private_key.unlink()


def _validate_discovery_file(
    discovery: dict[str, Any], symptom: str, recipe_id: str, catalog_revision: int
) -> None:
    matches = discovery.get("recipes")
    if (
        discovery.get("schema") != DISCOVERY_SCHEMA
        or discovery.get("catalog_revision") != catalog_revision
        or not isinstance(matches, list) or len(matches) != 1
        or not isinstance(matches[0], dict) or matches[0].get("callable") is not True
        or not isinstance(matches[0].get("recipe"), dict)
        or matches[0]["recipe"].get("id") != recipe_id
        or symptom not in matches[0]["recipe"].get("symptoms", [])
    ):
        raise JourneyError("workspace discovery does not bind the exact symptom and recipe")


def _require_same_contract(reference: dict[str, Any], candidate: dict[str, Any]) -> None:
    if _pass_contract(candidate) != _pass_contract(reference):
        raise JourneyError("probe run changed the ordered semantic pass contract")
    if _artifact_contract(candidate) != _artifact_contract(reference):
        raise JourneyError("probe run changed the artifact contract")
    if _execution_contract(candidate) != _execution_contract(reference):
        raise JourneyError("probe run changed the typed execution identity")


# External verifier follows below.


def _require_exact_fields(value: Any, fields: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != fields:
        raise JourneyError(f"{label} does not have its closed field set")
    return value


def _require_digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise JourneyError(f"{label} is not a SHA-256 digest")
    return value


def _command_records(events: list[dict[str, Any]]) -> list[tuple[str, int, str]]:
    records: list[tuple[str, int, str]] = []

    def visit(value: Any) -> None:
        if isinstance(value, dict):
            item_type = value.get("type")
            command = value.get("command")
            exit_code = value.get("exit_code")
            if (
                isinstance(item_type, str) and "command" in item_type
                and isinstance(command, str) and type(exit_code) is int
            ):
                output = value.get("aggregated_output", value.get("output", ""))
                records.append((command, exit_code, output if isinstance(output, str) else ""))
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    visit(events)
    return records


def _agent_messages(events: list[dict[str, Any]]) -> str:
    messages: list[str] = []

    def visit(value: Any) -> None:
        if isinstance(value, dict):
            if value.get("type") == "agent_message" and isinstance(value.get("text"), str):
                messages.append(value["text"])
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    visit(events)
    return "\n".join(messages)


def _transcript_has_agent_edit(events: list[dict[str, Any]], symptom: str) -> bool:
    completed_items = [
        event.get("item")
        for event in events
        if event.get("type") == "item.completed" and isinstance(event.get("item"), dict)
    ]
    run_positions = [
        index
        for index, item in enumerate(completed_items)
        if item.get("type") == "command_execution"
        and isinstance(item.get("command"), str)
        and "run-probe.sh" in item["command"]
        and symptom in item["command"]
    ]
    if len(run_positions) != 2:
        return False
    for item in completed_items[run_positions[0] + 1:run_positions[1]]:
        item_type = item.get("type")
        corpus = _string_corpus(item)
        if item_type == "file_change" and "run-probe.sh" in corpus:
            return True
        if (
            item_type == "command_execution"
            and "run-probe.sh" in corpus
            and "seeded_option" in corpus
            and "negative-control" in corpus
        ):
            return True
    return False


def _entry_map(snapshot: dict[str, Any]) -> dict[str, dict[str, Any]]:
    entries = snapshot.get("entries")
    if not isinstance(entries, list):
        raise JourneyError("tree snapshot has no entries")
    mapped: dict[str, dict[str, Any]] = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            raise JourneyError("tree snapshot entry is malformed")
        if entry["path"] in mapped:
            raise JourneyError("tree snapshot repeats a path")
        mapped[entry["path"]] = entry
    return mapped


def _require_case_identity(case: dict[str, Any], case_path: pathlib.Path) -> None:
    _require_exact_fields(
        case,
        {
            "schema", "status", "case_id", "symptom",
            "workspace", "forbidden_roots", "source_revision", "plan_revision",
            "source", "plan", "build", "public_material", "run_nonce", "record_key",
            "limits",
            "installed_cli", "harnesses", "prompt", "initial_tree", "correction",
            "selection", "reference",
        },
        "prepared case",
    )
    if (
        case["schema"] != CASE_SCHEMA
        or case["status"] != "prepared-structural-nonterminal"
        or not isinstance(case["case_id"], str)
        or re.fullmatch(r"[0-9a-f]{32}", case["case_id"]) is None
        or not isinstance(case["symptom"], str) or not case["symptom"]
        or not isinstance(case["run_nonce"], str)
        or re.fullmatch(r"[0-9a-f]{64}", case["run_nonce"]) is None
    ):
        raise JourneyError("prepared case is not a structural nonterminal v4 identity")
    if case["limits"] != _v4_limits():
        raise JourneyError("prepared v4 case does not freeze the exact resource limits")
    _require_revision(case["source_revision"], "source revision")
    _require_revision(case["plan_revision"], "plan revision")
    source = _require_exact_fields(
        case["source"],
        {
            "root", "revision", "clean", "status_sha256", "origin", "origin_main",
            "head_ref", "commit_signature",
        },
        "source provenance",
    )
    plan = _require_exact_fields(
        case["plan"],
        {
            "root", "revision", "clean", "status_sha256", "origin", "origin_main",
            "head_ref", "commit_signature", "document",
        },
        "plan provenance",
    )
    source_origin = _require_exact_fields(
        source["origin"], {"url", "repository"}, "source Git origin"
    )
    plan_origin = _require_exact_fields(
        plan["origin"], {"url", "repository"}, "plan Git origin"
    )
    for label, identity in (("source", source), ("plan", plan)):
        signature = _require_exact_fields(
            identity["commit_signature"], {"status", "signer", "fingerprint"},
            f"{label} commit signature",
        )
        if (
            not isinstance(identity["head_ref"], str) or not identity["head_ref"]
            or re.fullmatch(r"[0-9a-f]{40}", str(identity["origin_main"])) is None
            or not isinstance(signature["status"], str) or len(signature["status"]) != 1
            or not isinstance(signature["signer"], str)
            or not isinstance(signature["fingerprint"], str)
        ):
            raise JourneyError(f"{label} Git authority identity is malformed")
    if (
        source["revision"] != case["source_revision"] or source["clean"] is not True
        or plan["revision"] != case["plan_revision"] or plan["clean"] is not True
        or source["status_sha256"] != _sha256_bytes(b"")
        or plan["status_sha256"] != _sha256_bytes(b"")
        or source_origin["repository"] != "Generous-Corp/pulp"
        or plan_origin["repository"] != "danielraffel/pulp-planning"
        or not isinstance(source_origin["url"], str) or not source_origin["url"]
        or not isinstance(plan_origin["url"], str) or not plan_origin["url"]
        or plan["revision"] != plan["origin_main"]
    ):
        raise JourneyError("case revisions are not derived from clean source and plan repositories")
    document = _require_exact_fields(
        plan["document"], {"path", "git_blob", "bytes", "sha256"},
        "canonical plan document",
    )
    if (
        not isinstance(document["path"], str) or not document["path"]
        or re.fullmatch(r"[0-9a-f]{40}", str(document["git_blob"])) is None
        or type(document["bytes"]) is not int or document["bytes"] < 1
    ):
        raise JourneyError("canonical plan document identity is malformed")
    _require_digest(document["sha256"], "canonical plan document digest")
    if case["correction"] != {
        "path": "run-probe.sh", "before": SEED_LINE, "after": REPAIRED_LINE
    }:
        raise JourneyError("prepared private case does not contain the single seeded correction")
    harnesses = _require_exact_fields(
        case["harnesses"],
        {"journey_sha256", "trust_sha256", "isolation_sha256"},
        "harness identities",
    )
    if (
        _sha256(pathlib.Path(__file__).resolve(strict=True)) != harnesses["journey_sha256"]
        or _sha256(pathlib.Path(trust.__file__).resolve(strict=True)) != harnesses["trust_sha256"]
        or _sha256(pathlib.Path(trust.isolation.__file__).resolve(strict=True))
        != harnesses["isolation_sha256"]
    ):
        raise JourneyError("journey or trust harness bytes changed after preparation")
    record_key = _require_exact_fields(
        case["record_key"],
        {"algorithm", "private_key_path", "public_key_path", "public_key_sha256"},
        "record signing key identity",
    )
    public_key = pathlib.Path(record_key["public_key_path"])
    if (
        record_key["algorithm"] != "rsa-3072-sha256"
        or pathlib.Path(record_key["private_key_path"]) != case_path.parent / "record-signing-key.pem"
        or public_key != case_path.parent / "record-signing-public.pem"
        or not public_key.is_file() or public_key.is_symlink()
        or _sha256(public_key, 64 * 1024)
        != _require_digest(record_key["public_key_sha256"], "record public key digest")
    ):
        raise JourneyError("record signing public identity is invalid")
    if not isinstance(case["forbidden_roots"], list) or not case["forbidden_roots"]:
        raise JourneyError("prepared case has no source-tree isolation record")
    expected_roots = {
        source["root"], plan["root"],
        _require_exact_fields(
            case["build"],
            {"root", "cmake_cache", "cmake", "install_script", "build_info", "install_replay"},
            "build provenance",
        )["root"],
    }
    if not expected_roots.issubset(set(case["forbidden_roots"])):
        raise JourneyError("source, build, and plan are not all denied read roots")
    workspace = pathlib.Path(case["workspace"])
    _require_real_directory(workspace)
    installed_prefix = pathlib.Path(case["installed_cli"]["prefix"])
    _require_real_directory(installed_prefix)
    _validate_install_isolation(workspace, case_path.parent, installed_prefix)
    for item in case["forbidden_roots"]:
        root = pathlib.Path(item)
        _require_real_directory(root)
        if (
            _is_within(workspace, root)
            or _is_within(case_path.parent, root)
            or _is_within(pathlib.Path(case["installed_cli"]["path"]), root)
        ):
            raise JourneyError("workspace, private case, or installed CLI is inside a source tree")
    identity_seed = {
        "symptom": case["symptom"],
        "workspace": case["workspace"],
        "forbidden_roots": sorted(set(case["forbidden_roots"])),
        "source_revision": case["source_revision"],
        "plan_revision": case["plan_revision"],
        "pulp_binary_sha256": case["installed_cli"]["sha256"],
        "initial_tree_sha256": case["initial_tree"]["sha256"],
        "reference_result_sha256": case["reference"]["result_json_sha256"],
        "run_nonce": case["run_nonce"],
        "record_public_key_sha256": record_key["public_key_sha256"],
        "limits": _v4_limits(),
    }
    expected_case_id = _sha256_bytes(
        json.dumps(identity_seed, sort_keys=True, separators=(",", ":")).encode("utf-8")
    )[:32]
    if case["case_id"] != expected_case_id:
        raise JourneyError("case identity does not bind its provenance and private reference")


def _validate_session_identity(
    case: dict[str, Any], case_path: pathlib.Path, session: dict[str, Any]
) -> None:
    _require_exact_fields(
        session,
        {
            "schema", "status", "case_id", "case_sha256",
            "session_id", "run_nonce", "model", "cwd", "path", "home", "config_home",
            "codex_home", "runtime_removed", "environment_keys", "agent_binary",
            "keychain_auth", "network_proxy",
            "installed_cli_sha256", "source_revision", "plan_revision", "prompt_sha256",
            "command_argv", "agent_exit_code", "seatbelt", "output_schema", "transcript",
            "codex_rollout", "stderr", "last_message", "record_private_key_destroyed",
            "credential_disclosure_scan", "final_tree", "record_attestation",
            "redacted_record_attestation",
        },
        "agent session",
    )
    workspace = pathlib.Path(case["workspace"])
    case_dir = case_path.parent
    agent_home = pathlib.Path(session["home"])
    agent_config = pathlib.Path(session["config_home"])
    agent_codex_home = pathlib.Path(session["codex_home"])
    runtime_root = agent_home.parent
    runtime_isolated = (
        agent_home == runtime_root / "home"
        and agent_config == runtime_root / "config"
        and agent_codex_home == runtime_root / "codex"
        and runtime_root.is_absolute()
        and runtime_root.name.startswith("pulp-gpu-agent-runtime-")
        and not _is_within(runtime_root, workspace)
        and not _is_within(runtime_root, case_dir)
        and not _is_within(runtime_root, pathlib.Path(case["installed_cli"]["prefix"]))
        and not _is_within(pathlib.Path(case["installed_cli"]["prefix"]), runtime_root)
        and all(not _is_within(runtime_root, pathlib.Path(item)) for item in case["forbidden_roots"])
    )
    if (
        session["schema"] != SESSION_SCHEMA
        or session["status"] != "recorded-structural-nonterminal"
        or session["case_id"] != case["case_id"]
        or session["case_sha256"] != _sha256(case_path)
        or not isinstance(session["session_id"], str)
        or trust.UUID_RE.fullmatch(session["session_id"]) is None
        or session["run_nonce"] != case["run_nonce"]
        or not isinstance(session["model"], str) or not session["model"]
        or session["cwd"] != str(workspace)
        or session["path"] != os.pathsep.join(
            [str(pathlib.Path(case["installed_cli"]["path"]).parent), os.defpath]
        )
        or not runtime_isolated
        or session["runtime_removed"] is not True
        or runtime_root.exists() or runtime_root.is_symlink()
        or session["agent_exit_code"] != 0
        or session["installed_cli_sha256"] != case["installed_cli"]["sha256"]
        or session["source_revision"] != case["source_revision"]
        or session["plan_revision"] != case["plan_revision"]
        or session["prompt_sha256"] != case["prompt"]["sha256"]
        or session["record_private_key_destroyed"] is not True
        or session["credential_disclosure_scan"] != "passed"
    ):
        raise JourneyError("agent session identity or isolation binding is invalid")
    private_key = pathlib.Path(case["record_key"]["private_key_path"])
    if private_key.exists() or private_key.is_symlink():
        raise JourneyError("one-use record signing key still exists after the agent session")
    session_core = {
        key: value for key, value in session.items()
        if key not in {"record_attestation", "redacted_record_attestation"}
    }
    public_key_path = pathlib.Path(case["record_key"]["public_key_path"])
    trust.verify_record_signature(
        session_core,
        session["record_attestation"],
        public_key_path,
    )
    redacted_session_core = _redact_value(
        session_core, _redaction_pairs(case, case_path, session)
    )
    trust.verify_record_signature(
        redacted_session_core,
        session["redacted_record_attestation"],
        public_key_path,
    )
    keys = session["environment_keys"]
    if (
        not isinstance(keys, list) or any(not isinstance(item, str) for item in keys)
        or not set(keys).issubset(AGENT_INHERITED_ENV_KEYS | AGENT_FIXED_ENV_KEYS)
        or any(item.startswith("GIT_") for item in keys)
        or any(item.startswith("PULP_") and item != "PULP_UPDATE_CHECK_DISABLED" for item in keys)
        or not {
            "HOME", "XDG_CONFIG_HOME", "CODEX_HOME", "PATH", "PWD",
            "HTTPS_PROXY", "HTTP_PROXY", "NO_PROXY",
        }.issubset(keys)
    ):
        raise JourneyError("agent environment contains repository metadata or misses isolation keys")
    agent = session["agent_binary"]
    if agent != trust.official_codex_identity(pathlib.Path(agent["path"])):
        raise JourneyError("agent binary no longer has its recorded official Codex identity")
    keychain = _require_exact_fields(
        session["keychain_auth"],
        {
            "backend", "service", "account", "auth_mode", "payload_bytes",
            "trusted_application", "creator_access_removed", "fallback_auth_file_absent",
            "removed_after_record",
        },
        "one-use Codex keychain authentication",
    )
    expected_account = "cli|" + hashlib.sha256(
        str(agent_codex_home).encode("utf-8")
    ).hexdigest()[:16]
    if (
        keychain["backend"] != "macos-keychain-direct"
        or keychain["service"] != trust.CODEX_KEYCHAIN_SERVICE
        or keychain["account"] != expected_account
        or keychain["auth_mode"] not in {"chatgpt", "apikey"}
        or type(keychain["payload_bytes"]) is not int
        or not 1 <= keychain["payload_bytes"] <= 1024 * 1024
        or keychain["trusted_application"] != agent["path"]
        or keychain["creator_access_removed"] is not True
        or keychain["fallback_auth_file_absent"] is not True
        or keychain["removed_after_record"] is not True
    ):
        raise JourneyError("one-use Codex authentication is not keychain-only and removed")
    proxy = _require_exact_fields(
        session["network_proxy"],
        {
            "schema", "listen_host", "listen_port", "allowed_hosts", "max_connections",
            "max_bytes_each_way", "connections", "environment",
        },
        "CONNECT proxy audit",
    )
    expected_hosts = (
        ["api.openai.com"] if keychain["auth_mode"] == "apikey"
        else ["auth.openai.com", "chatgpt.com"]
    )
    proxy_url = f"http://127.0.0.1:{proxy['listen_port']}"
    connections = proxy["connections"]
    if (
        proxy["schema"] != "pulp.gpu-clean-agent-connect-proxy.v1"
        or proxy["listen_host"] != "127.0.0.1"
        or type(proxy["listen_port"]) is not int or not 1 <= proxy["listen_port"] <= 65535
        or proxy["allowed_hosts"] != expected_hosts
        or proxy["max_connections"] != 32
        or proxy["max_bytes_each_way"] != 64 * 1024 * 1024
        or proxy["environment"] != {
            "HTTPS_PROXY": proxy_url, "HTTP_PROXY": proxy_url, "NO_PROXY": "",
        }
        or not isinstance(connections, list)
        or not 1 <= len(connections) <= proxy["max_connections"]
        or not any(
            isinstance(item, dict) and item.get("outcome") == "completed"
            for item in connections
        )
        or any(
            not isinstance(item, dict)
            or set(item) != {
                "host", "port", "outcome", "bytes_to_upstream", "bytes_to_client",
            }
            or item["host"] not in {*expected_hosts, "rejected"}
            or item["port"] != 443
            or item["outcome"] not in {"completed", "transport-preflight"}
            or type(item["bytes_to_upstream"]) is not int
            or type(item["bytes_to_client"]) is not int
            or not 0 <= item["bytes_to_upstream"] <= proxy["max_bytes_each_way"]
            or not 0 <= item["bytes_to_client"] <= proxy["max_bytes_each_way"]
            for item in connections
        )
    ):
        raise JourneyError("CONNECT proxy audit does not prove bounded exact-host egress")
    argv = session["command_argv"]
    def flag_value(flag: str) -> str | None:
        if not isinstance(argv, list):
            return None
        try:
            index = argv.index(flag)
        except ValueError:
            return None
        return argv[index + 1] if index + 1 < len(argv) else None

    expected_environment = {
        "HOME": str(agent_home), "TMPDIR": str(agent_home / "tmp"),
        "PATH": session["path"], "PWD": str(workspace),
        "PULP_UPDATE_CHECK_DISABLED": "1", "NO_COLOR": "1",
    }
    expected_argv = [
        "/usr/bin/sandbox-exec", "-f", str(runtime_root / "agent-seatbelt.sb"),
        agent["path"], *_codex_exec_config_arguments(expected_environment),
        "exec", "--json", "--ignore-user-config", "--ignore-rules",
        "--skip-git-repo-check", "--sandbox", "danger-full-access", "--color", "never",
        "--model", session["model"], "--cd", str(workspace), "--output-schema",
        str(runtime_root / "agent-output-schema.json"), "--output-last-message",
        str(runtime_root / "last-message.txt"),
        f"<prompt-sha256:{case['prompt']['sha256']}>",
    ]
    if (
        not isinstance(argv, list) or any(not isinstance(item, str) for item in argv)
        or argv != expected_argv
        or "--ignore-user-config" not in argv or "--ignore-rules" not in argv
        or "--ephemeral" in argv or "--skip-git-repo-check" not in argv
        or flag_value("--sandbox") != "danger-full-access"
        or flag_value("--model") != session["model"]
        or flag_value("--cd") != str(workspace)
        or flag_value("--output-schema") != str(runtime_root / "agent-output-schema.json")
        or flag_value("--output-last-message") != str(runtime_root / "last-message.txt")
        or any(
            forbidden in argv
            for forbidden in (
                "--dangerously-bypass-approvals-and-sandbox", "--add-dir", "--full-auto",
            )
        )
        or argv[-1] != f"<prompt-sha256:{case['prompt']['sha256']}>"
        or any(path in item for path in [str(case_dir), *case["forbidden_roots"]] for item in argv)
    ):
        raise JourneyError("agent command does not prove the single outer-Seatbelt launch")

    seatbelt = _require_exact_fields(
        session["seatbelt"],
        {
            "authority", "codex_internal_sandbox", "runtime_profile_path",
            "network_proxy_port", "denied_roots", "profile", "preflight",
        },
        "Seatbelt identity",
    )
    expected_denied = [str(case_dir), *case["forbidden_roots"]]
    profile_payload = _validate_bound_file(
        seatbelt["profile"], case_dir / "agent-seatbelt.sb", "Seatbelt profile"
    )
    expected_profile = trust.seatbelt_profile_for_paths(
        workspace=workspace,
        runtime_root=runtime_root,
        installed_prefix=pathlib.Path(case["installed_cli"]["prefix"]),
        agent_bin=pathlib.Path(agent["path"]),
        denied_roots=[pathlib.Path(item) for item in expected_denied],
        network_proxy_port=proxy["listen_port"],
    ).encode("utf-8")
    if (
        seatbelt["authority"] != "outer-macos-seatbelt"
        or seatbelt["codex_internal_sandbox"] != "danger-full-access"
        or seatbelt["runtime_profile_path"] != str(runtime_root / "agent-seatbelt.sb")
        or seatbelt["network_proxy_port"] != proxy["listen_port"]
        or seatbelt["denied_roots"] != expected_denied
        or profile_payload != expected_profile
    ):
        raise JourneyError("Seatbelt profile does not bind the required read/write boundary")
    preflight = _require_exact_fields(
        seatbelt["preflight"], {"schema", "profile_sha256", "controls", "passed"},
        "Seatbelt preflight",
    )
    controls = preflight["controls"]
    expected_controls = {
        ("workspace-read", "positive"),
        ("workspace-write-remove", "positive"),
        ("signed-codex-version", "positive"),
        ("signed-codex-exec-help", "positive"),
        ("signed-codex-keychain-auth", "positive"),
        ("installed-pulp-version", "positive"),
        ("connect-proxy", "positive"),
        ("non-allowlisted-loopback", "network-denied"),
        ("ambient-usr-local", "directory-read-denied"),
        ("ambient-site-library", "directory-read-denied"),
        ("non-allowlisted-read", "absolute-denied"),
        ("non-allowlisted-read", "relative-denied"),
        ("non-allowlisted-write", "write-denied"),
        ("denied-symlink", "symlink-denied"),
        *((label, kind) for label in (
            "private-case", "source-checkout", "canonical-plan", "build-tree"
        ) for kind in ("absolute-denied", "relative-denied")),
    }
    if (
        preflight["schema"] != "pulp.gpu-clean-agent-seatbelt-preflight.v1"
        or preflight["profile_sha256"] != seatbelt["profile"]["sha256"]
        or preflight["passed"] is not True or not isinstance(controls, list)
        or len(controls) != len(expected_controls)
        or {
            (item.get("label"), item.get("kind"))
            for item in controls if isinstance(item, dict)
        } != expected_controls
        or any(
            set(item) != {"label", "kind", "exit_code"}
            or type(item["exit_code"]) is not int
            or ((item["kind"] == "positive") != (item["exit_code"] == 0))
            for item in controls if isinstance(item, dict)
        )
        or any(not isinstance(item, dict) for item in controls)
    ):
        raise JourneyError("Seatbelt planted controls do not prove direct and traversal denial")
    output_schema = _validate_bound_file(
        session["output_schema"], case_dir / "agent-output-schema.json", "agent output schema"
    )
    if output_schema != _json_bytes(_final_output_schema(case["run_nonce"], case["symptom"])):
        raise JourneyError("agent output schema is not bound to this case nonce and symptom")


def _validate_bound_file(
    record: Any, expected_path: pathlib.Path, label: str, extra_fields: set[str] | None = None
) -> bytes:
    record = _require_exact_fields(
        record, {"path", "sha256", "bytes", *(extra_fields or set())}, label
    )
    payload = _read_regular_bytes(expected_path)
    if (
        record["path"] != str(expected_path)
        or record["bytes"] != len(payload)
        or record["sha256"] != _sha256_bytes(payload)
    ):
        raise JourneyError(f"{label} bytes do not match the session binding")
    return payload


def _expected_initial_tree() -> dict[str, Any]:
    task = _task_text().encode("utf-8")
    script = _seed_script().encode("utf-8")
    entries = [
        {
            "path": "TASK.md", "type": "file", "mode": "0600", "bytes": len(task),
            "sha256": _sha256_bytes(task),
        },
        {
            "path": "run-probe.sh", "type": "file", "mode": "0755", "bytes": len(script),
            "sha256": _sha256_bytes(script),
        },
    ]
    canonical = json.dumps(entries, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return {
        "sha256": _sha256_bytes(canonical),
        "entry_count": len(entries),
        "total_file_bytes": len(task) + len(script),
        "entries": entries,
    }


def _validate_case_material(
    case: dict[str, Any], case_path: pathlib.Path,
    *, allowed_source_untracked: Iterable[pathlib.Path] = (),
) -> tuple[dict[str, Any], dict[str, str], dict[str, Any]]:
    workspace = pathlib.Path(case["workspace"])
    case_dir = case_path.parent
    if case["initial_tree"] != _expected_initial_tree():
        raise JourneyError("prepared before-tree is not the documented seeded workspace")
    prompt_record = _require_exact_fields(
        case["prompt"], {"path", "sha256", "bytes"}, "prepared prompt"
    )
    prompt_path = case_dir / "agent-prompt.txt"
    prompt_payload = _read_regular_bytes(prompt_path)
    expected_prompt = _prompt_text(workspace, case["symptom"], case["run_nonce"]).encode("utf-8")
    if (
        prompt_record["path"] != str(prompt_path)
        or prompt_payload != expected_prompt
        or prompt_record["bytes"] != len(prompt_payload)
        or prompt_record["sha256"] != _sha256_bytes(prompt_payload)
    ):
        raise JourneyError("prepared prompt is not the public no-context prompt")
    prompt_text = prompt_payload.decode("utf-8")
    forbidden_prompt_text = [
        case["selection"]["recipe_id"], SEED_LINE, REPAIRED_LINE, str(case_dir),
        *case["forbidden_roots"],
    ]
    if any(item and item in prompt_text for item in forbidden_prompt_text):
        raise JourneyError("agent prompt leaks a recipe or private source/verifier path")

    source_root = pathlib.Path(case["source"]["root"])
    plan_root = pathlib.Path(case["plan"]["root"])
    current_source = trust.git_repository_identity(
        source_root, expected_repository="Generous-Corp/pulp",
        allowed_untracked=allowed_source_untracked,
    )
    current_plan = trust.git_repository_identity(
        plan_root,
        expected_repository="danielraffel/pulp-planning",
        required_document=plan_root / case["plan"]["document"]["path"],
        require_origin_main=True,
    )
    if current_source != case["source"] or current_plan != case["plan"]:
        raise JourneyError("source or canonical plan provenance changed after preparation")
    for label, identity in (("source", current_source), ("plan", current_plan)):
        signature = identity["commit_signature"]
        if (
            signature["status"] != "G"
            or signature["fingerprint"] != trust.AUTHORIZED_GIT_SIGNING_FINGERPRINT
        ):
            raise JourneyError(
                f"{label} HEAD lacks the authorized valid commit signature required for recording"
            )
    current_build, current_installed = trust.build_install_identity(
        source=current_source,
        build_root=pathlib.Path(case["build"]["root"]),
        install_script=pathlib.Path(case["build"]["install_script"]["path"]),
        installed_prefix=pathlib.Path(case["installed_cli"]["prefix"]),
        installed_pulp=pathlib.Path(case["installed_cli"]["path"]),
        timeout=120.0,
    )
    if current_build != case["build"]:
        raise JourneyError("Release build/install provenance changed after preparation")
    current_public = trust.installed_public_material(
        source_root, pathlib.Path(case["installed_cli"]["prefix"])
    )
    if current_public != case["public_material"]:
        raise JourneyError("installed public catalog or documentation changed after preparation")
    installed = _require_exact_fields(
        case["installed_cli"],
        {
            "prefix", "path", "cpp_path", "bytes", "sha256",
            "fresh_install_replay_sha256", "version_output", "version_output_sha256",
            "linkage_sha256",
        },
        "installed CLI identity",
    )
    pulp = pathlib.Path(installed["path"])
    if (
        not pulp.is_absolute() or not pulp.is_file() or pulp.is_symlink()
        or installed["bytes"] != pulp.stat().st_size
        or installed["sha256"] != _sha256(pulp, MAX_BINARY_BYTES)
        or any(installed[key] != current_installed[key] for key in current_installed)
        or not isinstance(installed["version_output"], str)
        or not installed["version_output"]
    ):
        raise JourneyError("installed CLI identity changed or is not an installed executable")
    _require_digest(installed["version_output_sha256"], "CLI version output digest")
    version_result = _run(
        [str(pulp), "version"], 30.0, cwd=case_dir,
        monitor_roots=[ResourceRoot(case_dir)],
    )
    if (
        version_result.returncode != 0
        or version_result.stdout.strip() != installed["version_output"]
        or _sha256_bytes(version_result.stdout.encode("utf-8"))
        != installed["version_output_sha256"]
    ):
        raise JourneyError("installed CLI version output changed after preparation")
    linkage_sha256 = _installed_cli_linkage_digest(
        pulp,
        [pathlib.Path(item) for item in case["forbidden_roots"]],
        case_dir,
        30.0,
    )
    if installed["linkage_sha256"] != linkage_sha256:
        raise JourneyError("installed CLI linkage changed after preparation")

    selection = _require_exact_fields(
        case["selection"], {"catalog_schema", "catalog_revision", "recipe_id"},
        "private recipe selection",
    )
    if (
        selection["catalog_schema"] != CATALOG_SCHEMA
        or type(selection["catalog_revision"]) is not int
        or selection["catalog_revision"] < 1
        or not isinstance(selection["recipe_id"], str) or not selection["recipe_id"]
    ):
        raise JourneyError("private recipe selection is malformed")
    reference_record = _require_exact_fields(
        case["reference"],
        {
            "command", "exit_code", "result_path", "result_json_sha256", "result",
            "artifacts_sha256", "adapter",
        },
        "private reference",
    )
    reference_path = case_dir / "reference-result.json"
    reference = _load_json(reference_path)
    if (
        reference_record["exit_code"] != 0
        or reference_record["result_path"] != str(reference_path)
        or reference_record["result"] != reference
        or reference_record["result_json_sha256"] != _sha256(reference_path)
    ):
        raise JourneyError("private reference result is not bound to its prepared bytes")
    command = reference_record["command"]
    reference_artifact_dir = case_dir / "reference-artifacts"
    if (
        not isinstance(command, list) or any(not isinstance(item, str) for item in command)
        or not command or command[0] != str(pulp)
        or "--recipe" not in command
        or command[command.index("--recipe") + 1] != selection["recipe_id"]
        or "--artifacts" not in command
        or command[command.index("--artifacts") + 1] != str(reference_artifact_dir)
        or "--negative-control" in command
    ):
        raise JourneyError("private reference command is not the unmutated installed recipe")
    reference_hashes = _validate_evidence(
        reference, selection["recipe_id"], reference_artifact_dir, case_dir
    )
    adapter = _authentic_adapter(reference)
    if (
        reference_record["artifacts_sha256"] != reference_hashes
        or reference_record["adapter"] != adapter
        or reference["verdict"] != "pass" or reference["mutation"] is not None
        or any(item["verdict"] != "pass" for item in _typed_passes(reference))
    ):
        raise JourneyError("private reference is not an authentic unmutated pass")
    return reference, reference_hashes, adapter


def _validate_workspace_evidence(
    case: dict[str, Any], reference: dict[str, Any], reference_hashes: dict[str, str],
    reference_adapter: dict[str, Any],
) -> tuple[
    dict[str, Any], dict[str, Any], dict[str, str], dict[str, str], list[str], list[str]
]:
    workspace = pathlib.Path(case["workspace"])
    recipe_id = case["selection"]["recipe_id"]
    discovery = _load_json(workspace / "discovery.json")
    _validate_discovery_file(
        discovery, case["symptom"], recipe_id, case["selection"]["catalog_revision"]
    )
    negative = _load_json(workspace / "negative-result.json")
    repaired = _load_json(workspace / "repaired-result.json")
    negative_hashes = _validate_evidence(
        negative, recipe_id, workspace / "artifacts" / "negative", workspace
    )
    repaired_hashes = _validate_evidence(
        repaired, recipe_id, workspace / "artifacts" / "repaired", workspace
    )
    _require_same_contract(reference, negative)
    _require_same_contract(reference, repaired)
    negative_passes = _typed_passes(negative)
    repaired_passes = _typed_passes(repaired)
    failed = [item for item in negative_passes if item["verdict"] == "fail"]
    if (
        negative["verdict"] != "fail" or not isinstance(negative["mutation"], str)
        or not negative["mutation"] or len(failed) != 1
        or any(item["verdict"] not in {"pass", "fail"} for item in negative_passes)
        or any(item["work_completed"] is not True for item in negative_passes)
    ):
        raise JourneyError("seeded run is not one completed typed negative control")
    if (
        repaired["verdict"] != "pass" or repaired["mutation"] is not None
        or any(item["verdict"] != "pass" for item in repaired_passes)
        or _authentic_adapter(negative) != reference_adapter
        or _authentic_adapter(repaired) != reference_adapter
        or repaired["source_digest"] != reference["source_digest"]
        or repaired["signature_digest"] != reference["signature_digest"]
        or negative["source_digest"] == reference["source_digest"]
        or negative["signature_digest"] == reference["signature_digest"]
    ):
        raise JourneyError("repaired proof does not restore the authentic reference identity")
    evidence_ids = {
        reference["gpu_evidence_id"], negative["gpu_evidence_id"], repaired["gpu_evidence_id"]
    }
    if len(evidence_ids) != 3:
        raise JourneyError("reference, negative, and repaired proofs are not independent executions")
    reference_without_id = {key: value for key, value in reference.items() if key != "gpu_evidence_id"}
    repaired_without_id = {key: value for key, value in repaired.items() if key != "gpu_evidence_id"}
    if repaired_without_id != reference_without_id or repaired_hashes != reference_hashes:
        raise JourneyError("repaired receipt and artifacts do not exactly match the reference proof")
    stable, changed = _partition_artifact_changes(reference_hashes, negative_hashes)
    return negative, repaired, negative_hashes, repaired_hashes, stable, changed


def _validate_workspace_tree(
    case: dict[str, Any], session: dict[str, Any], negative: dict[str, Any],
    repaired: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, list[str]], str]:
    workspace = pathlib.Path(case["workspace"])
    task_payload = _read_regular_bytes(workspace / "TASK.md")
    script_payload = _read_regular_bytes(workspace / "run-probe.sh")
    expected_task = _task_text().encode("utf-8")
    seeded_script = _seed_script()
    repaired_script = seeded_script.replace(SEED_LINE, REPAIRED_LINE, 1)
    if seeded_script.count(SEED_LINE) != 1 or REPAIRED_LINE in seeded_script:
        raise JourneyError("harness does not have exactly one documented seed")
    if task_payload != expected_task or script_payload != repaired_script.encode("utf-8"):
        raise JourneyError("agent edit is not exactly the documented one-line correction")
    final_tree = _snapshot_tree(workspace)
    if final_tree != session["final_tree"]:
        raise JourneyError("workspace changed after the independent agent session")
    mapped = _entry_map(final_tree)
    if mapped.get("run-probe.sh", {}).get("mode") != "0755":
        raise JourneyError("agent correction changed the executable script mode")
    expected_paths = {
        "TASK.md", "run-probe.sh", "discovery.json", "negative-result.json",
        "repaired-result.json", "artifacts", "artifacts/negative", "artifacts/repaired",
    }
    for run_kind, evidence in (("negative", negative), ("repaired", repaired)):
        for artifact in evidence["artifacts"]:
            expected_paths.add(f"artifacts/{run_kind}/{artifact['name']}")
    if set(mapped) != expected_paths:
        raise JourneyError("workspace contains missing or extra files beyond the bounded journey")
    diff = _tree_diff(case["initial_tree"], final_tree)
    expected_added = sorted(expected_paths - {"TASK.md", "run-probe.sh"})
    if (
        diff["added"] != expected_added
        or diff["removed"]
        or diff["modified"] != ["run-probe.sh"]
    ):
        raise JourneyError("before/after tree diff contains more than the documented repair and receipts")
    script_diff = "".join(
        difflib.unified_diff(
            seeded_script.splitlines(keepends=True),
            repaired_script.splitlines(keepends=True),
            fromfile="before/run-probe.sh", tofile="after/run-probe.sh",
        )
    )
    removed = [line for line in script_diff.splitlines() if line.startswith("-") and not line.startswith("---")]
    added = [line for line in script_diff.splitlines() if line.startswith("+") and not line.startswith("+++")]
    if removed != [f"-{SEED_LINE}"] or added != [f"+{REPAIRED_LINE}"]:
        raise JourneyError("script diff is not exactly the documented seeded-option correction")
    return final_tree, diff, script_diff


def _diagnostic_tokens(negative: dict[str, Any]) -> list[str]:
    failed = [item for item in _typed_passes(negative) if item["verdict"] == "fail"]
    if len(failed) != 1:
        raise JourneyError("negative receipt does not have one typed diagnosis")
    item = failed[0]
    values = [
        item["name"], item["code"], negative["mutation"],
        json.dumps(item["expected"], separators=(",", ":")),
        json.dumps(item["observed"], separators=(",", ":")),
        json.dumps(item["absolute_error"], separators=(",", ":")),
    ]
    return [value for value in values if isinstance(value, str) and value]


def _transcript_uses_network_tool(value: Any) -> bool:
    if isinstance(value, dict):
        item_type = str(value.get("type", "")).lower().replace("-", "_")
        tool_name = str(value.get("name", "")).lower().replace("-", "_")
        if (
            ("web" in item_type and "search" in item_type)
            or item_type in {"web_search", "search_query"}
            or ("web" in tool_name and "search" in tool_name)
        ):
            return True
        return any(_transcript_uses_network_tool(child) for child in value.values())
    if isinstance(value, list):
        return any(_transcript_uses_network_tool(child) for child in value)
    return False


def _validate_public_discovery_sequence(
    case: dict[str, Any], events: list[dict[str, Any]],
    records: list[tuple[str, int, str]], run_positions: list[int],
) -> None:
    if len(run_positions) != 2:
        raise JourneyError("public discovery validation requires exactly two probe runs")
    symptom = case["symptom"]
    recipe_id = case["selection"]["recipe_id"]
    discovery_positions = [
        index for index, (command, exit_code, output) in enumerate(records)
        if "gpu recipes list" in command and "--symptom" in command
        and symptom in command and "--json" in command and exit_code == 0
        and symptom in output and recipe_id in output
    ]
    catalog_positions = [
        index for index, (command, exit_code, output) in enumerate(records)
        if "share/pulp/gpu-recipes.yaml" in command and exit_code == 0
        and recipe_id in output and symptom in output
    ]
    public_docs = {
        item["installed_relative_path"]: item["source_path"]
        for item in case["public_material"]
        if item["role"] in {"guide", "reference"}
    }
    doc_records = [
        (index, installed_relative, source_relative)
        for index, (command, exit_code, output) in enumerate(records)
        for installed_relative, source_relative in public_docs.items()
        if installed_relative in command and exit_code == 0 and len(output.strip()) >= 32
    ]
    commands = "\n".join(record[0] for record in records)
    forbidden_network = (
        r"(?i)(?:^|[;&|()\s])(?:curl|wget|ftp|nc|netcat|telnet|ssh|scp|sftp|gh)\b",
        r"(?i)https?://",
        r"(?i)\bgit\s+(?:clone|fetch|pull|ls-remote)\b",
        r"(?i)\b(?:urllib|requests|http\.client|socket)\b",
        r"(?i)\b(?:fetch|XMLHttpRequest)\s*\(",
    )
    if (
        not discovery_positions
        or not catalog_positions
        or not doc_records
        or discovery_positions[0] >= catalog_positions[0]
        or discovery_positions[0] >= run_positions[0]
        or catalog_positions[0] >= run_positions[0]
        or min(item[0] for item in doc_records) >= run_positions[0]
        or not any(
            source_relative in "\n".join(records[index][2] for index in catalog_positions)
            for _, _, source_relative in doc_records
        )
        or any(re.search(pattern, commands) for pattern in forbidden_network)
        or _transcript_uses_network_tool(events)
    ):
        raise JourneyError(
            "transcript does not prove pre-edit symptom-only installed catalog/document discovery"
        )


def _validate_transcript(
    case: dict[str, Any], case_path: pathlib.Path, session: dict[str, Any],
    negative: dict[str, Any], repaired: dict[str, Any],
) -> tuple[list[dict[str, Any]], list[tuple[str, int, str]]]:
    case_dir = case_path.parent
    transcript_payload = _validate_bound_file(
        session["transcript"], case_dir / "agent-transcript.jsonl", "full agent transcript",
        {"events"},
    )
    _validate_bound_file(session["stderr"], case_dir / "agent-stderr.log", "agent stderr")
    last_message_payload = _validate_bound_file(
        session["last_message"], case_dir / "agent-last-message.json", "agent final message",
        {"result"},
    )
    events, thread_id = _parse_transcript(transcript_payload)
    if thread_id != session["session_id"] or session["transcript"]["events"] != len(events):
        raise JourneyError("transcript event count or session identity does not match")
    records = _command_records(events)
    symptom = case["symptom"]
    run_positions = [
        index for index, record in enumerate(records)
        if "run-probe.sh" in record[0] and symptom in record[0]
    ]
    run_records = [records[index] for index in run_positions]
    if len(run_records) != 2 or [item[1] for item in run_records] != [1, 0]:
        raise JourneyError("transcript must bind exactly one negative and one repaired script execution")
    if not _transcript_has_agent_edit(events, symptom):
        raise JourneyError("transcript does not bind the agent's edit between the two script runs")
    commands = "\n".join(record[0] for record in records)
    if "pwd" not in commands or "command -v pulp" not in commands:
        raise JourneyError("transcript does not prove the requested cwd and installed-CLI checks")
    _validate_public_discovery_sequence(case, events, records, run_positions)
    corpus = _string_corpus(events)
    final_result = _parse_agent_final_output(last_message_payload, case)
    if final_result != session["last_message"]["result"]:
        raise JourneyError("session does not bind the parsed structured final output")
    failed = [item for item in _typed_passes(negative) if item["verdict"] == "fail"]
    if len(failed) != 1:
        raise JourneyError("negative evidence has no unique failed pass")
    failed_pass = failed[0]
    expected_final = {
        "run_nonce": case["run_nonce"],
        "symptom": symptom,
        "selected_recipe": case["selection"]["recipe_id"],
        "negative": {
            "exit_code": 1,
            "verdict": "fail",
            "pass": failed_pass["name"],
            "code": failed_pass["code"],
            "mutation": negative["mutation"],
            "expected": failed_pass["expected"],
            "observed": failed_pass["observed"],
            "absolute_error": failed_pass["absolute_error"],
        },
        "edit": {"path": "run-probe.sh", "removed": SEED_LINE, "added": REPAIRED_LINE},
        "repaired": {"exit_code": 0, "verdict": "pass"},
    }
    if final_result != expected_final:
        raise JourneyError("structured final output does not exactly interpret the evidence and repair")
    final_message = last_message_payload.decode("utf-8", errors="strict").strip()
    agent_messages = _agent_messages(events)
    if not final_message or final_message not in agent_messages:
        raise JourneyError("bounded final message is not present in the full tool transcript")
    required_corpus = [
        str(case["workspace"]), case["selection"]["recipe_id"],
        negative["gpu_evidence_id"], repaired["gpu_evidence_id"],
        "probe exit: 1", "probe exit: 0",
    ]
    if any(token not in corpus for token in required_corpus):
        raise JourneyError("transcript omits a selected recipe, receipt, cwd, or exit binding")
    private_paths = [str(case_path.parent), *case["forbidden_roots"]]
    if any(path in corpus for path in private_paths):
        raise JourneyError("agent transcript exposes a private verifier or source-checkout path")
    rollout_payload = _validate_bound_file(
        session["codex_rollout"], case_dir / "codex-session.jsonl", "Codex session JSONL",
        {"events", "turn_id"},
    )
    rollout = trust.parse_codex_rollout(
        rollout_payload,
        thread_id=thread_id,
        cli_version=session["agent_binary"]["cli_version"],
        workspace=pathlib.Path(case["workspace"]),
        model=session["model"],
        run_nonce=case["run_nonce"],
    )
    if (
        session["codex_rollout"]["events"] != rollout["event_count"]
        or session["codex_rollout"]["turn_id"] != rollout["turn_id"]
    ):
        raise JourneyError("Codex rollout identity is not joined to the stdout transcript")
    return events, records


def _validate_superseded_disposition(value: Any) -> None:
    value = _require_exact_fields(
        value,
        {
            "schema", "status", "acceptance_gate_satisfied", "recorded_date",
            "prior_artifact", "invalidation", "replacement_gate",
        },
        "legacy receipt disposition",
    )
    prior = _require_exact_fields(
        value["prior_artifact"],
        {"filename", "schema", "sha256", "historical_provenance"},
        "legacy prior artifact",
    )
    invalidation = _require_exact_fields(
        value["invalidation"], {"reason", "nonterminal_effect"}, "legacy invalidation"
    )
    replacement = _require_exact_fields(
        value["replacement_gate"],
        {"required_schema", "required_status", "required_new_filename", "requirements"},
        "legacy replacement gate",
    )
    if (
        value["schema"] != SUPERSEDED_SCHEMA
        or value["status"] != "superseded-nonterminal"
        or value["acceptance_gate_satisfied"] is not False
        or value["recorded_date"] != "2026-08-28"
        or prior["filename"] != "m3-a5-clean-agent-20260828.json"
        or prior["schema"] != "pulp.gpu-clean-agent-journey.v1"
        or re.fullmatch(r"[0-9a-f]{64}", prior["sha256"]) is None
        or not isinstance(prior["historical_provenance"], dict)
        or not isinstance(invalidation["reason"], str) or not invalidation["reason"]
        or invalidation["nonterminal_effect"] != "cannot satisfy Horizon-A A5"
        or replacement["required_schema"] != "pulp.gpu-clean-agent-verification.v2"
        or replacement["required_status"] != "independent-agent-accepted"
        or replacement["required_new_filename"] == prior["filename"]
        or not isinstance(replacement["requirements"], list) or not replacement["requirements"]
    ):
        raise JourneyError("legacy receipt is not an explicit superseded/nonterminal disposition")


def _bundle_bounds(value: Any, *, depth: int = 0) -> None:
    if depth > MAX_BUNDLE_DEPTH:
        raise JourneyError("audit bundle exceeds its nesting-depth cap")
    if value is None or isinstance(value, (bool, int)):
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise JourneyError("audit bundle contains a non-finite number")
        return
    if isinstance(value, str):
        if len(value.encode("utf-8")) > MAX_BUNDLE_STRING_BYTES:
            raise JourneyError("audit bundle contains an oversized string")
        return
    if isinstance(value, list):
        if len(value) > MAX_BUNDLE_EVENTS:
            raise JourneyError("audit bundle contains an oversized list")
        for item in value:
            _bundle_bounds(item, depth=depth + 1)
        return
    if isinstance(value, dict):
        if len(value) > MAX_WORKSPACE_ENTRIES:
            raise JourneyError("audit bundle contains an oversized object")
        for key, item in value.items():
            if not isinstance(key, str) or len(key.encode("utf-8")) > 256:
                raise JourneyError("audit bundle contains an invalid key")
            _bundle_bounds(item, depth=depth + 1)
        return
    raise JourneyError("audit bundle contains an unsupported value")


def _redaction_pairs(
    case: dict[str, Any], case_path: pathlib.Path, session: dict[str, Any]
) -> list[tuple[str, str]]:
    runtime_root = str(pathlib.Path(session["home"]).parent)
    values = {
        str(case_path.parent): "$PRIVATE_CASE",
        case["build"]["root"]: "$BUILD_ROOT",
        case["source"]["root"]: "$SOURCE_ROOT",
        case["plan"]["root"]: "$PLAN_ROOT",
        case["workspace"]: "$WORKSPACE",
        case["installed_cli"]["prefix"]: "$INSTALL_PREFIX",
        session["agent_binary"]["path"]: "$CODEX_BINARY",
        runtime_root: "$AGENT_RUNTIME",
    }
    return sorted(values.items(), key=lambda item: len(item[0]), reverse=True)


def _redact_value(value: Any, replacements: list[tuple[str, str]]) -> Any:
    if isinstance(value, str):
        redacted = value
        for original, placeholder in replacements:
            redacted = redacted.replace(original, placeholder)
        return redacted
    if isinstance(value, list):
        return [_redact_value(item, replacements) for item in value]
    if isinstance(value, tuple):
        return [_redact_value(item, replacements) for item in value]
    if isinstance(value, dict):
        return {key: _redact_value(item, replacements) for key, item in value.items()}
    return value


def _bounded_redacted_bytes(
    value: dict[str, Any], replacements: list[tuple[str, str]], label: str,
) -> tuple[dict[str, Any], bytes]:
    redacted = _redact_value(value, replacements)
    _bundle_bounds(redacted)
    payload = _json_bytes(redacted)
    if len(payload) > MAX_BUNDLE_BYTES:
        raise JourneyError(f"{label} exceeds its {MAX_BUNDLE_BYTES}-byte cap")
    leaked = [original for original, _ in replacements if original in payload.decode("utf-8")]
    if leaked:
        raise JourneyError(f"{label} still contains a private absolute path")
    return redacted, payload


def _public_rollout_events(payload: bytes) -> list[dict[str, Any]]:
    selected: list[dict[str, Any]] = []
    for line in payload.splitlines():
        if not line.strip():
            continue
        event = json.loads(line)
        event_type = event.get("type")
        source_payload = event.get("payload", {})
        if event_type == "session_meta":
            allowed = {
                "id", "session_id", "cwd", "originator", "source", "cli_version",
                "model_provider",
            }
        elif event_type == "turn_context":
            allowed = {"turn_id", "cwd", "model", "approval_policy", "sandbox_policy"}
        elif event_type == "event_msg" and source_payload.get("type") in {
            "task_started", "task_complete",
        }:
            allowed = {"type", "turn_id", "started_at", "completed_at", "duration_ms"}
        elif event_type == "response_item" and source_payload.get("type") in {
            "message", "custom_tool_call", "custom_tool_call_output",
            "function_call", "function_call_output",
        }:
            allowed = {
                "type", "role", "content", "call_id", "id", "name", "namespace",
                "input", "arguments", "output", "status",
            }
        else:
            continue
        projected = {
            "type": event_type,
            "payload": {key: source_payload[key] for key in sorted(allowed) if key in source_payload},
        }
        if "timestamp" in event:
            projected["timestamp"] = event["timestamp"]
        if "ordinal" in event:
            projected["ordinal"] = event["ordinal"]
        selected.append(projected)
    return selected


def verify_case(
    case_path: pathlib.Path, session_path: pathlib.Path, receipt_path: pathlib.Path,
    bundle_path: pathlib.Path,
) -> dict[str, Any]:
    _require_real_directory(case_path.parent)
    _require_real_directory(session_path.parent)
    legacy_path = (
        pathlib.Path(__file__).resolve(strict=True).parents[2]
        / "docs/validation/gpu-clean-agent/m3-a5-clean-agent-20260828.json"
    )
    _validate_superseded_disposition(_load_json(legacy_path))
    case = _load_json(case_path)
    _require_case_identity(case, case_path)
    workspace = pathlib.Path(case["workspace"])
    source_root = pathlib.Path(case["source"]["root"])
    durable_parent = source_root / "docs/validation/gpu-clean-agent"
    if (
        receipt_path.parent != durable_parent or bundle_path.parent != durable_parent
        or receipt_path == bundle_path
        or receipt_path.name == "m3-a5-clean-agent-20260828.json"
        or bundle_path.name == "m3-a5-clean-agent-20260828.json"
        or receipt_path.suffix != ".json" or bundle_path.suffix != ".json"
        or re.fullmatch(r"[A-Za-z0-9._-]+", receipt_path.name) is None
        or re.fullmatch(r"[A-Za-z0-9._-]+", bundle_path.name) is None
        or _is_within(receipt_path, workspace) or _is_within(receipt_path, case_path.parent)
        or _is_within(bundle_path, workspace) or _is_within(bundle_path, case_path.parent)
    ):
        raise JourneyError("structural receipt and bundle must be new durable A5 source artifacts")
    _require_real_directory(durable_parent)
    for output_path in (receipt_path, bundle_path):
        relative = output_path.relative_to(source_root).as_posix()
        tracked = _run(
            [
                "/usr/bin/git", "-c", "core.fsmonitor=false", "-C", str(source_root),
                "ls-files", "--error-unmatch", "--", relative,
            ],
            30.0,
        )
        if tracked.returncode == 0:
            raise JourneyError("structural receipt and bundle paths must be new untracked artifacts")
    if receipt_path.exists() or receipt_path.is_symlink():
        raise JourneyError("structural verification receipt already exists")
    expected_session_path = case_path.parent / "agent-session.json"
    if session_path != expected_session_path:
        raise JourneyError("verifier requires the session beside its private prepared case")
    session = _load_json(session_path)
    _validate_session_identity(case, case_path, session)
    reference, reference_hashes, adapter = _validate_case_material(
        case,
        case_path,
        allowed_source_untracked=(bundle_path,) if bundle_path.exists() else (),
    )
    negative, repaired, negative_hashes, repaired_hashes, stable, changed = (
        _validate_workspace_evidence(case, reference, reference_hashes, adapter)
    )
    final_tree, tree_diff, script_diff = _validate_workspace_tree(
        case, session, negative, repaired
    )
    events, records = _validate_transcript(case, case_path, session, negative, repaired)
    replacements = _redaction_pairs(case, case_path, session)
    rollout_payload = _read_regular_bytes(
        case_path.parent / "codex-session.jsonl", MAX_METADATA_BYTES
    )
    public_key_payload = _read_regular_bytes(
        pathlib.Path(case["record_key"]["public_key_path"]), 64 * 1024
    )
    bundle = {
        "schema": "pulp.gpu-clean-agent-audit-bundle.v2",
        "status": "structural-verification-material",
        "case_id": case["case_id"],
        "redaction": {
            "format": "absolute-path-placeholders-v1",
            "placeholders": sorted({placeholder for _, placeholder in replacements}),
            "private_content_included": False,
        },
        "trust_boundary": {
            "official_codex_code_signature_required": True,
            "stdout_thread_joined_to_persisted_rollout": True,
            "one_use_record_signature_verified": True,
            "redacted_session_signature_verified": True,
            "outer_macos_seatbelt_preflight_verified": True,
            "local_record_orchestrator_is_trusted": True,
            "remote_session_attestation_claimed": False,
        },
        "case": case,
        "prompt_text": _read_regular_bytes(pathlib.Path(case["prompt"]["path"])).decode("utf-8"),
        "record_public_key": {
            "algorithm": "rsa-3072-sha256",
            "bytes": len(public_key_payload),
            "sha256": _sha256_bytes(public_key_payload),
            "pem": public_key_payload.decode("ascii"),
        },
        "session": session,
        "stdout_transcript_events": events,
        "codex_rollout_public_events": _public_rollout_events(rollout_payload),
        "command_records": records,
        "workspace": {
            "before_tree": case["initial_tree"],
            "after_tree": final_tree,
            "exact_tree_diff": tree_diff,
            "exact_script_diff": script_diff,
        },
        "evidence": {
            "selection": {**case["selection"], "symptom": case["symptom"]},
            "reference": {
                "result": reference, "artifacts_sha256": reference_hashes,
            },
            "negative": {
                "exit_code": 1, "result": negative, "artifacts_sha256": negative_hashes,
            },
            "repaired": {
                "exit_code": 0, "result": repaired, "artifacts_sha256": repaired_hashes,
            },
            "adapter": adapter,
            "stable_negative_artifacts": stable,
            "changed_negative_artifacts": changed,
        },
    }
    _require_v4_structural_nonterminal(bundle, "v4 structural audit bundle")
    redacted_bundle, bundle_payload = _bounded_redacted_bytes(
        bundle, replacements, "durable audit bundle"
    )
    bundle_record = {
        "schema": redacted_bundle["schema"],
        "filename": bundle_path.name,
        "bytes": len(bundle_payload),
        "sha256": _sha256_bytes(bundle_payload),
        "stdout_event_count": len(events),
        "codex_rollout_public_event_count": len(redacted_bundle["codex_rollout_public_events"]),
    }
    receipt = {
        "schema": VERIFICATION_SCHEMA,
        "status": "structural-verification-passed",
        "case_id": case["case_id"],
        "case_sha256": _sha256(case_path),
        "session_sha256": _sha256(session_path),
        "provenance": {
            "source": case["source"], "plan": case["plan"], "build": case["build"],
            "public_material": case["public_material"],
        },
        "installed_cli": case["installed_cli"],
        "agent": {
            **session["agent_binary"],
            "model": session["model"],
            "session_id": session["session_id"],
            "cwd": session["cwd"],
            "path": session["path"],
            "launcher_argv": session["command_argv"],
            "environment_keys": session["environment_keys"],
            "temporary_home": session["home"],
            "temporary_config_home": session["config_home"],
            "temporary_codex_home": session["codex_home"],
            "temporary_runtime_removed": session["runtime_removed"],
            "seatbelt": session["seatbelt"],
            "codex_rollout": session["codex_rollout"],
            "record_attestation": session["record_attestation"],
            "ignore_user_config": True,
            "ignore_rules": True,
        },
        "prompt": {
            **case["prompt"],
            "text": _read_regular_bytes(pathlib.Path(case["prompt"]["path"])).decode("utf-8"),
        },
        "transcript": {
            **session["transcript"],
            "stderr": session["stderr"],
            "last_message": session["last_message"],
            "command_records_sha256": _sha256_bytes(_json_bytes(records)),
            "command_record_count": len(records),
            "event_count": len(events),
        },
        "durable_audit_bundle": bundle_record,
        "workspace": {
            "path": str(workspace),
            "before_tree": case["initial_tree"],
            "after_tree": final_tree,
            "exact_tree_diff": tree_diff,
            "exact_script_diff": script_diff,
        },
        "selection": {
            **case["selection"],
            "symptom": case["symptom"],
            "discovery_sha256": _sha256(workspace / "discovery.json"),
        },
        "reference": {
            "result": reference,
            "result_json_sha256": _sha256(case_path.parent / "reference-result.json"),
            "artifacts_sha256": reference_hashes,
        },
        "negative": {
            "exit_code": 1,
            "result": negative,
            "result_json_sha256": _sha256(workspace / "negative-result.json"),
            "artifacts_sha256": negative_hashes,
        },
        "repaired": {
            "exit_code": 0,
            "result": repaired,
            "result_json_sha256": _sha256(workspace / "repaired-result.json"),
            "artifacts_sha256": repaired_hashes,
        },
        "adapter": adapter,
        "stable_negative_artifacts": stable,
        "changed_negative_artifacts": changed,
        "supersedes": {
            "schema": "pulp.gpu-clean-agent-journey.v1",
            "checked_in_receipt": "m3-a5-clean-agent-20260828.json",
        },
        "trust_boundary": bundle["trust_boundary"],
    }
    _require_v4_structural_nonterminal(receipt, "v4 structural verification")
    redacted_receipt, receipt_payload = _bounded_redacted_bytes(
        receipt, replacements, "structural verification receipt"
    )
    _publish_structural_pair(
        bundle_path=bundle_path,
        bundle_payload=bundle_payload,
        receipt_path=receipt_path,
        receipt_payload=receipt_payload,
    )
    return redacted_receipt


def _absolute_path(value: str, label: str) -> pathlib.Path:
    path = pathlib.Path(value)
    if not path.is_absolute() or any(part in {".", ".."} for part in path.parts):
        raise JourneyError(f"{label} must be an absolute normalized path")
    return path


def _relative_path(value: str, label: str) -> pathlib.Path:
    path = pathlib.Path(value)
    if path.is_absolute() or not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        raise JourneyError(f"{label} must be a normalized repository-relative path")
    return path


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="prepare, record, or externally verify an independent GPU recipe repair"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser("prepare", help="create a seeded public workspace and private reference")
    prepare.add_argument("--pulp", required=True)
    prepare.add_argument("--symptom", required=True)
    prepare.add_argument("--workspace", required=True)
    prepare.add_argument("--case-dir", required=True)
    prepare.add_argument("--source-root", required=True)
    prepare.add_argument("--build-root", required=True)
    prepare.add_argument("--cli-install-script", required=True)
    prepare.add_argument("--installed-prefix", required=True)
    prepare.add_argument("--plan-root", required=True)
    prepare.add_argument("--plan-document", required=True)
    prepare.add_argument("--forbidden-root", action="append", default=[])
    prepare.add_argument("--timeout-seconds", type=float, default=120.0)

    record = subparsers.add_parser("record", help="launch one signed Codex inside outer Seatbelt")
    record.add_argument("--case", required=True)
    record.add_argument("--agent-bin", required=True)
    record.add_argument("--model", required=True)
    record.add_argument("--timeout-seconds", type=float, default=900.0)

    verify = subparsers.add_parser(
        "verify", help="externally validate and publish structural nonterminal evidence"
    )
    verify.add_argument("--case", required=True)
    verify.add_argument("--session", required=True)
    verify.add_argument("--receipt", required=True)
    verify.add_argument("--bundle", required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        if args.command == "prepare":
            result = prepare_case(
                _absolute_path(args.pulp, "--pulp"),
                args.symptom,
                _absolute_path(args.workspace, "--workspace"),
                _absolute_path(args.case_dir, "--case-dir"),
                _absolute_path(args.source_root, "--source-root"),
                _absolute_path(args.build_root, "--build-root"),
                _absolute_path(args.cli_install_script, "--cli-install-script"),
                _absolute_path(args.installed_prefix, "--installed-prefix"),
                _absolute_path(args.plan_root, "--plan-root"),
                _relative_path(args.plan_document, "--plan-document"),
                [_absolute_path(item, "--forbidden-root") for item in args.forbidden_root],
                args.timeout_seconds,
            )
        elif args.command == "record":
            result = record_agent(
                _absolute_path(args.case, "--case"),
                _absolute_path(args.agent_bin, "--agent-bin"),
                args.model,
                args.timeout_seconds,
            )
        else:
            result = verify_case(
                _absolute_path(args.case, "--case"),
                _absolute_path(args.session, "--session"),
                _absolute_path(args.receipt, "--receipt"),
                _absolute_path(args.bundle, "--bundle"),
            )
    except JourneyUnavailable as exc:
        print(f"UNAVAILABLE: {exc}", file=sys.stderr)
        return 2
    except (JourneyError, trust.TrustError, OSError, KeyError, TypeError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
