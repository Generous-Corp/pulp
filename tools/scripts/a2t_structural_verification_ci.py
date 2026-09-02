#!/usr/bin/env python3
"""Issue a nonterminal A2T verifier attestation inside the required macOS PR job."""

from __future__ import annotations

import hashlib
import json
import os
import re
import resource
import stat
import subprocess
import sys
import tempfile
import types
from pathlib import Path
from typing import Any, Mapping


REPOSITORY_NAME = "Generous-Corp/pulp"
RECEIPT_PATH = Path("evidence/receipt.json")
VERIFIER_PATH = Path("tools/scripts/verify_gpu_trace_overhead_acceptance.py")
ISSUER_PATH = Path("tools/scripts/a2t_structural_verification_ci.py")
WORKFLOW_PATH = Path(".github/workflows/build.yml")
SCHEMA_PATH = Path(
    "docs/validation/gpu-trace-overhead/"
    "a2t-structural-verifier-attestation-v1.schema.json"
)
JSON_SCHEMA_PATH = Path("tools/scripts/json_schema_lite.py")
GPU_CONTRACT_PATH = Path("tools/scripts/gpu_trace_overhead_acceptance.py")
SDK_HANDOFF_PATH = Path("tools/scripts/sdk_capability_handoff.py")
SDK_PROVENANCE_PATH = Path("tools/scripts/sdk_provenance.py")
EXECUTED_DEPENDENCIES = {
    "issuer_json_schema_lite": JSON_SCHEMA_PATH,
    "verifier_gpu_trace_overhead_acceptance": GPU_CONTRACT_PATH,
    "verifier_json_schema_lite": JSON_SCHEMA_PATH,
    "verifier_sdk_capability_handoff": SDK_HANDOFF_PATH,
    "verifier_sdk_provenance": SDK_PROVENANCE_PATH,
}
OUTPUT_NAME = "attestation.json"
STEP_NAME = "Verify A2T structural receipt"
CHECK_NAME = "macos"
JOB_KEY = "build"
ISSUER_COMMAND = ["python3", ISSUER_PATH.as_posix()]
VERIFIER_COMMAND = [
    "python3", VERIFIER_PATH.as_posix(), "../evidence/receipt.json",
    "--repository", ".",
]
EXPECTED_STDOUT = (
    b"gpu-trace-overhead-acceptance: ok "
    b"(v3 structural integrity only; nonterminal)\n"
)
MAX_CAPTURE_BYTES = 64 * 1024
MAX_JSON_BYTES = 8 * 1024 * 1024
SHA1 = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def _load_source_bytes(module_name: str, path: Path, data: bytes) -> types.ModuleType:
    if len(data) > 256 * 1024:
        raise RuntimeError(f"local source module is unbounded: {path.name}")
    module = types.ModuleType(module_name)
    module.__file__ = str(path)
    module.__package__ = ""
    sys.modules[module_name] = module
    exec(compile(data, str(path), "exec", dont_inherit=True), module.__dict__)
    return module


class IssuerError(RuntimeError):
    pass


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise IssuerError(message)


def _positive_int(value: str | None, name: str) -> int:
    _require(value is not None and value.isascii() and value.isdigit(), f"{name} is not a positive integer")
    parsed = int(value)
    _require(parsed > 0, f"{name} is not a positive integer")
    return parsed


def _git(repository: Path, *args: str, binary: bool = False) -> bytes | str:
    completed = subprocess.run(
        ["git", *args], cwd=repository, check=False, capture_output=True,
        text=not binary,
    )
    if completed.returncode != 0:
        detail = completed.stderr if isinstance(completed.stderr, str) else completed.stderr.decode(errors="replace")
        raise IssuerError(f"git {' '.join(args)} failed: {detail.strip()}")
    return completed.stdout


def _tracked_file(repository: Path, revision: str, path: Path) -> tuple[str, bytes]:
    _require(SHA1.fullmatch(revision) is not None, f"invalid revision for {path}")
    row = str(_git(repository, "ls-tree", revision, "--", path.as_posix())).strip()
    fields = row.split(None, 3)
    _require(
        len(fields) == 4 and fields[0] == "100644" and fields[1] == "blob"
        and SHA1.fullmatch(fields[2]) is not None,
        f"{path} is not one exact regular Git blob at {revision}",
    )
    data = _git(repository, "show", f"{revision}:{path.as_posix()}", binary=True)
    assert isinstance(data, bytes)
    return fields[2], data


def _working_regular_file(repository: Path, path: Path, maximum: int) -> bytes:
    absolute = repository / path
    try:
        metadata = absolute.lstat()
    except OSError as error:
        raise IssuerError(f"cannot inspect {path}: {error}") from error
    _require(stat.S_ISREG(metadata.st_mode), f"{path} is not a regular file")
    _require(0 < metadata.st_size <= maximum, f"{path} has an invalid size")
    data = absolute.read_bytes()
    _require(len(data) == metadata.st_size, f"{path} changed while it was read")
    return data


def _json_object(data: bytes, label: str) -> dict[str, Any]:
    try:
        value = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise IssuerError(f"{label} is not valid JSON: {error}") from error
    _require(isinstance(value, dict), f"{label} is not a JSON object")
    return value


def _event_head(environment: Mapping[str, str]) -> str:
    event_path = environment.get("GITHUB_EVENT_PATH")
    _require(bool(event_path), "GITHUB_EVENT_PATH is missing")
    path = Path(str(event_path))
    try:
        metadata = path.lstat()
    except OSError as error:
        raise IssuerError(f"cannot inspect GitHub event: {error}") from error
    _require(stat.S_ISREG(metadata.st_mode) and metadata.st_size <= MAX_JSON_BYTES, "GitHub event has an invalid file shape")
    event = _json_object(path.read_bytes(), "GitHub event")
    pull_request = event.get("pull_request")
    head = pull_request.get("head") if isinstance(pull_request, dict) else None
    sha = head.get("sha") if isinstance(head, dict) else None
    _require(isinstance(sha, str) and SHA1.fullmatch(sha) is not None, "GitHub event has no exact PR head")
    return sha


def _limit_output_files() -> None:
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_CAPTURE_BYTES, MAX_CAPTURE_BYTES))


def _run_verifier(
    repository: Path, source_revision: str, receipt_bytes: bytes,
) -> tuple[int, bytes, bytes]:
    with tempfile.TemporaryDirectory(prefix="pulp-a2t-verifier-") as temporary:
        root = Path(temporary)
        source_checkout = root / "source"
        evidence_directory = root / "evidence"
        evidence_directory.mkdir()
        (evidence_directory / "receipt.json").write_bytes(receipt_bytes)
        worktree_added = False
        checkout = subprocess.run(
            [
                "git", "worktree", "add", "--quiet", "--detach",
                str(source_checkout), source_revision,
            ],
            cwd=repository,
            check=False, capture_output=True, text=True,
        )
        try:
            _require(
                checkout.returncode == 0,
                f"cannot create exact source worktree: {checkout.stderr.strip()}",
            )
            worktree_added = True
            head = str(_git(source_checkout, "rev-parse", "HEAD")).strip()
            _require(
                head == source_revision,
                "verifier worktree HEAD differs from source_revision",
            )
            stdout_path = root / "stdout"
            stderr_path = root / "stderr"
            with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
                try:
                    completed = subprocess.run(
                        VERIFIER_COMMAND, cwd=source_checkout,
                        stdin=subprocess.DEVNULL, stdout=stdout_file,
                        stderr=stderr_file, check=False, timeout=120,
                        preexec_fn=_limit_output_files,
                    )
                except subprocess.TimeoutExpired as error:
                    raise IssuerError(
                        "A2T structural verifier exceeded 120 seconds"
                    ) from error
            stdout = stdout_path.read_bytes()
            stderr = stderr_path.read_bytes()
        finally:
            if worktree_added:
                active_error = sys.exc_info()[0]
                removed = subprocess.run(
                    ["git", "worktree", "remove", "--force", str(source_checkout)],
                    cwd=repository, check=False, capture_output=True, text=True,
                )
                if removed.returncode != 0 and active_error is None:
                    raise IssuerError(
                        "cannot remove exact source worktree: "
                        + removed.stderr.strip()
                    )
    _require(len(stdout) <= MAX_CAPTURE_BYTES and len(stderr) <= MAX_CAPTURE_BYTES, "A2T verifier output exceeded its bound")
    return completed.returncode, stdout, stderr


def _binding(commit: str, path: Path, blob: str) -> dict[str, str]:
    return {"commit": commit, "path": path.as_posix(), "blob": blob}


def issue(repository: Path, output_directory: Path, environment: Mapping[str, str]) -> Path:
    repository = repository.resolve()
    _require(environment.get("GITHUB_ACTIONS") == "true", "issuer may run only in GitHub Actions")
    _require(environment.get("GITHUB_EVENT_NAME") == "pull_request", "issuer may run only for a pull_request event")
    _require(environment.get("GITHUB_REPOSITORY") == REPOSITORY_NAME, "issuer is running for the wrong repository")
    _require(environment.get("RUNNER_OS") == "macOS", "issuer may run only on macOS")
    _require(environment.get("GITHUB_JOB") == JOB_KEY, "issuer is not running in the native build job")
    _require(environment.get("A2T_CHECK_NAME") == CHECK_NAME, "issuer is not running in the required macos check")
    _require(environment.get("A2T_STEP_NAME") == STEP_NAME, "issuer step identity is wrong")

    evidence_head = _event_head(environment)
    workflow_revision = environment.get("GITHUB_WORKFLOW_SHA", "")
    _require(SHA1.fullmatch(workflow_revision) is not None, "GITHUB_WORKFLOW_SHA is not an exact commit")
    run_id = _positive_int(environment.get("GITHUB_RUN_ID"), "GITHUB_RUN_ID")
    run_attempt = _positive_int(environment.get("GITHUB_RUN_ATTEMPT"), "GITHUB_RUN_ATTEMPT")

    receipt_blob, receipt_bytes = _tracked_file(repository, evidence_head, RECEIPT_PATH)
    _require(_working_regular_file(repository, RECEIPT_PATH, MAX_JSON_BYTES) == receipt_bytes, "working receipt differs from exact PR-head blob")
    receipt = _json_object(receipt_bytes, "A2T receipt")
    source_revision = receipt.get("source_revision")
    _require(isinstance(source_revision, str) and SHA1.fullmatch(source_revision) is not None, "receipt source_revision is not exact")
    _require(receipt.get("mcp_source_revision") == source_revision, "receipt mcp_source_revision differs from source_revision")
    _require(receipt.get("integration_head") == source_revision, "receipt integration_head differs from source_revision")

    verifier_blob, verifier_bytes = _tracked_file(repository, source_revision, VERIFIER_PATH)
    verifier_e_blob, verifier_e_bytes = _tracked_file(repository, evidence_head, VERIFIER_PATH)
    _require(verifier_e_blob == verifier_blob and verifier_e_bytes == verifier_bytes, "verifier changed between S and E")
    _require(_working_regular_file(repository, VERIFIER_PATH, 2 * 1024 * 1024) == verifier_bytes, "executed verifier differs from S/E blob")

    dependency_bindings: dict[str, dict[str, str]] = {}
    dependency_bytes: dict[Path, bytes] = {}
    for name, path in EXECUTED_DEPENDENCIES.items():
        source_blob, source_bytes = _tracked_file(repository, source_revision, path)
        evidence_blob, evidence_bytes = _tracked_file(repository, evidence_head, path)
        _require(
            evidence_blob == source_blob and evidence_bytes == source_bytes,
            f"executed dependency {path} changed between S and E",
        )
        _require(
            _working_regular_file(repository, path, 2 * 1024 * 1024) == source_bytes,
            f"executed dependency {path} differs from S/E blob",
        )
        dependency_bindings[name] = {
            **_binding(source_revision, path, source_blob),
            "sha256": _sha256(source_bytes),
        }
        dependency_bytes[path] = source_bytes

    issuer_blob, issuer_bytes = _tracked_file(repository, source_revision, ISSUER_PATH)
    issuer_e_blob, issuer_e_bytes = _tracked_file(repository, evidence_head, ISSUER_PATH)
    _require(issuer_e_blob == issuer_blob and issuer_e_bytes == issuer_bytes, "issuer changed between S and E")
    _require(_working_regular_file(repository, ISSUER_PATH, 512 * 1024) == issuer_bytes, "executed issuer differs from S/E blob")
    schema_blob, schema_bytes = _tracked_file(repository, source_revision, SCHEMA_PATH)
    schema_e_blob, schema_e_bytes = _tracked_file(repository, evidence_head, SCHEMA_PATH)
    _require(schema_e_blob == schema_blob and schema_e_bytes == schema_bytes, "attestation schema changed between S and E")
    _require(_working_regular_file(repository, SCHEMA_PATH, 512 * 1024) == schema_bytes, "attestation schema differs from S/E blob")
    schema = _json_object(schema_bytes, "A2T attestation schema")
    json_schema_lite = _load_source_bytes(
        "a2t_json_schema_lite", JSON_SCHEMA_PATH, dependency_bytes[JSON_SCHEMA_PATH]
    )
    workflow_blob, workflow_bytes = _tracked_file(repository, workflow_revision, WORKFLOW_PATH)
    _require(_working_regular_file(repository, WORKFLOW_PATH, 2 * 1024 * 1024) == workflow_bytes, "checked-out workflow differs from executed workflow revision")

    artifacts = receipt.get("artifacts")
    trace = artifacts.get("trace") if isinstance(artifacts, dict) else None
    trace_role = trace.get("role") if isinstance(trace, dict) else None
    trace_sha256 = trace.get("sha256") if isinstance(trace, dict) else None
    _require(isinstance(trace_role, str) and trace_role.startswith("repository/"), "receipt trace role is not repository-bound")
    _require(isinstance(trace_sha256, str) and SHA256.fullmatch(trace_sha256) is not None, "receipt trace digest is invalid")
    trace_path = Path(trace_role.removeprefix("repository/"))
    _require(not trace_path.is_absolute() and ".." not in trace_path.parts, "receipt trace path is unsafe")
    _, trace_bytes = _tracked_file(repository, source_revision, trace_path)
    _require(_sha256(trace_bytes) == trace_sha256, "receipt trace digest differs from S blob")

    exit_code, stdout, stderr = _run_verifier(
        repository, source_revision, receipt_bytes
    )
    _require(exit_code == 0, f"A2T structural verifier failed with exit {exit_code}")
    _require(stdout == EXPECTED_STDOUT, "A2T structural verifier stdout is not canonical")
    _require(stderr == b"", "A2T structural verifier wrote stderr")

    workflow_semantics = {
        "issuer_command": ISSUER_COMMAND,
        "verifier_command": VERIFIER_COMMAND,
        "step_name": STEP_NAME,
        "artifact_name": f"a2t-structural-verification-{evidence_head}",
    }
    semantic_bytes = json.dumps(workflow_semantics, sort_keys=True, separators=(",", ":")).encode()
    attestation = {
        "schema": "pulp.gpu-trace-structural-verifier-attestation.v1",
        "source_revision": source_revision,
        "evidence_head": evidence_head,
        "contract": {**_binding(source_revision, SCHEMA_PATH, schema_blob), "sha256": _sha256(schema_bytes)},
        "workflow": {**_binding(workflow_revision, WORKFLOW_PATH, workflow_blob), "sha256": _sha256(workflow_bytes), "semantics_sha256": _sha256(semantic_bytes)},
        "issuer": {**_binding(source_revision, ISSUER_PATH, issuer_blob), "sha256": _sha256(issuer_bytes)},
        "verifier": {**_binding(source_revision, VERIFIER_PATH, verifier_blob), "sha256": _sha256(verifier_bytes)},
        "dependencies": dependency_bindings,
        "receipt": {**_binding(evidence_head, RECEIPT_PATH, receipt_blob), "sha256": _sha256(receipt_bytes)},
        "trace_sha256": trace_sha256,
        "run": {"id": run_id, "attempt": run_attempt, "event": "pull_request", "head_sha": evidence_head, "workflow_sha": workflow_revision},
        "job": {"key": JOB_KEY, "check_name": CHECK_NAME},
        "step": {"name": STEP_NAME, "issuer_command": ISSUER_COMMAND, "verifier_command": VERIFIER_COMMAND},
        "result": {"exit_code": 0, "error_count": 0, "stdout_sha256": _sha256(stdout), "stderr_sha256": _sha256(stderr)},
    }
    violations = json_schema_lite.validate(attestation, schema)
    _require(not violations, "A2T attestation violates its closed schema: " + "; ".join(violations))
    output_directory.mkdir(parents=True, exist_ok=False)
    output = output_directory / OUTPUT_NAME
    output.write_text(json.dumps(attestation, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    _require(output.stat().st_size <= MAX_CAPTURE_BYTES, "A2T attestation exceeded its bound")
    return output


def main() -> int:
    output_root = os.environ.get("A2T_ATTESTATION_DIR")
    if not output_root:
        print("a2t-structural-verification-ci: FAIL: A2T_ATTESTATION_DIR is missing", file=sys.stderr)
        return 1
    try:
        output = issue(Path.cwd(), Path(output_root), os.environ)
    except (IssuerError, OSError, subprocess.SubprocessError) as error:
        print(f"a2t-structural-verification-ci: FAIL: {error}", file=sys.stderr)
        return 1
    print(f"a2t-structural-verification-ci: ok ({output})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
