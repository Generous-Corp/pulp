#!/usr/bin/env python3
"""Execute a Shipyard-selected literal CTest file without regex construction.

This adapter is intentionally narrow. Shipyard owns exact-head selection and
writes one literal test name per line. This script independently proves that
the current CTest inventory matches Pulp's protected contract, that every
requested name expands to the expected registrations (including duplicate
display names), and only then invokes CTest with ``--tests-from-file`` as an
argv element. Any ambiguity exits before a test process starts.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import tomllib
from pathlib import Path
from typing import Any, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ci"))

import build_dir_lock
import changed_surface_inventory as inventory


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO_ROOT / ".shipyard" / "config.toml"
DEFAULT_CONTRACT = REPO_ROOT / ".shipyard" / "changed-surface-inventory.json"
EXCLUDE_NAME = inventory.EXCLUDED_NAME_REGEX
EXCLUDE_LABEL = inventory.EXCLUDED_LABEL_REGEX
MINIMUM_CTEST_VERSION = (3, 29)
# Shipyard keeps the base64-expanded command below cmd.exe's 8,191-character
# ceiling. Larger selections conservatively stay on the full validation path.
MAX_SELECTED_TEST_BYTES = 4 * 1024


class SelectionExecutionError(ValueError):
    """The literal selection cannot safely replace the full test stage."""


def parse_literal_selection(payload: bytes) -> list[str]:
    """Parse a UTF-8, newline-delimited set of literal test names."""

    if b"\0" in payload or b"\r" in payload:
        raise SelectionExecutionError("selected-tests file contains an invalid line boundary")
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise SelectionExecutionError("selected-tests file is not UTF-8") from error
    if not text.endswith("\n"):
        raise SelectionExecutionError("selected-tests file must end with one newline")
    names = text[:-1].split("\n")
    if not names or any(not name.strip() for name in names):
        raise SelectionExecutionError("selected-tests file contains an empty test name")
    if len(names) != len(set(names)):
        raise SelectionExecutionError("selected-tests file contains duplicate names")
    return names


def decode_selection_receipt(
    encoded: str, expected_sha256: str
) -> tuple[list[str], bytes, list[str], bytes, dict[str, Any]]:
    """Authenticate and parse Shipyard's exact-identity selection receipt."""

    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise SelectionExecutionError("selected-tests SHA-256 is malformed")
    if not encoded or not re.fullmatch(r"[A-Za-z0-9_-]+", encoded):
        raise SelectionExecutionError("selected-tests payload is not canonical URL-safe base64")
    try:
        padding = "=" * (-len(encoded) % 4)
        payload = base64.b64decode(encoded + padding, altchars=b"-_", validate=True)
    except (ValueError, binascii.Error) as error:
        raise SelectionExecutionError("selected-tests payload is malformed") from error
    canonical = base64.urlsafe_b64encode(payload).decode("ascii").rstrip("=")
    if canonical != encoded:
        raise SelectionExecutionError("selected-tests payload is not canonically encoded")
    if len(payload) > MAX_SELECTED_TEST_BYTES:
        raise SelectionExecutionError("selected-tests payload exceeds the safe execution limit")
    if hashlib.sha256(payload).hexdigest() != expected_sha256:
        raise SelectionExecutionError("selected-tests payload digest mismatch")
    try:
        receipt = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SelectionExecutionError("selection receipt is not valid UTF-8 JSON") from error
    required = {
        "schema_version",
        "repository",
        "pull_request",
        "target",
        "base_sha",
        "head_sha",
        "tree_sha",
        "policy_digest",
        "selection_receipt_digest",
        "validation_contract_digest",
        "workflow_digest",
        "selected_tests_digest",
        "selected_tests",
    }
    if isinstance(receipt, dict) and receipt.get("schema_version") == 2:
        required.update({"selected_build_targets_digest", "selected_build_targets"})
    if not isinstance(receipt, dict) or set(receipt) != required:
        raise SelectionExecutionError("selection receipt has an unexpected schema")
    if receipt["schema_version"] not in (1, 2):
        raise SelectionExecutionError("selection receipt schema version is unsupported")
    if not isinstance(receipt["pull_request"], int) or receipt["pull_request"] <= 0:
        raise SelectionExecutionError("selection receipt PR identity is invalid")
    for key in ("repository", "target"):
        if not isinstance(receipt[key], str) or not receipt[key]:
            raise SelectionExecutionError(f"selection receipt {key} is invalid")
    for key in ("base_sha", "head_sha", "tree_sha"):
        if not isinstance(receipt[key], str) or not re.fullmatch(
            r"[0-9a-fA-F]{40}", receipt[key]
        ):
            raise SelectionExecutionError(f"selection receipt {key} is invalid")
    for key in (
        "policy_digest",
        "selection_receipt_digest",
        "validation_contract_digest",
        "workflow_digest",
        "selected_tests_digest",
    ):
        if not isinstance(receipt[key], str) or not re.fullmatch(
            r"[0-9a-f]{64}", receipt[key]
        ):
            raise SelectionExecutionError(f"selection receipt {key} is invalid")
    selected_tests = receipt["selected_tests"]
    if not isinstance(selected_tests, list) or not all(
        isinstance(name, str) for name in selected_tests
    ):
        raise SelectionExecutionError("selection receipt selected_tests is invalid")
    literal_payload = "".join(f"{name}\n" for name in selected_tests).encode("utf-8")
    names = parse_literal_selection(literal_payload)
    if hashlib.sha256(literal_payload).hexdigest() != receipt["selected_tests_digest"]:
        raise SelectionExecutionError("selection receipt literal-test digest mismatch")
    build_targets: list[str] = []
    build_target_payload = b""
    if receipt["schema_version"] == 2:
        build_targets_value = receipt["selected_build_targets"]
        if not isinstance(build_targets_value, list) or not all(
            isinstance(target, str) for target in build_targets_value
        ):
            raise SelectionExecutionError("selection receipt build targets are invalid")
        build_target_payload = "".join(
            f"{target}\n" for target in build_targets_value
        ).encode("utf-8")
        build_targets = parse_literal_selection(build_target_payload)
        if not build_targets or any(
            target.startswith("-")
            or re.fullmatch(r"[A-Za-z0-9_.:+-]+", target) is None
            for target in build_targets
        ):
            raise SelectionExecutionError("selection receipt build target is not canonical")
        digest = receipt["selected_build_targets_digest"]
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise SelectionExecutionError("selection receipt build-target digest is invalid")
        if hashlib.sha256(build_target_payload).hexdigest() != digest:
            raise SelectionExecutionError("selection receipt build-target digest mismatch")
    return names, literal_payload, build_targets, build_target_payload, receipt


def git_value(*args: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(REPO_ROOT), *args],
            check=True,
            capture_output=True,
            text=True,
            shell=False,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise SelectionExecutionError(f"cannot verify checkout identity: {error}") from error
    return result.stdout.strip()


def validate_receipt_identity(receipt: dict[str, Any], target: str) -> None:
    """Bind the receipt to the clean Pulp checkout consumed by this adapter."""

    if receipt["repository"] != "Generous-Corp/pulp" or receipt["target"] != target:
        raise SelectionExecutionError("selection receipt repository or target mismatch")
    current_head = git_value("rev-parse", "HEAD")
    current_tree = git_value("rev-parse", "HEAD^{tree}")
    if current_head != receipt["head_sha"] or current_tree != receipt["tree_sha"]:
        raise SelectionExecutionError("selection receipt does not match checkout HEAD and tree")
    if git_value("status", "--porcelain", "--untracked-files=all"):
        raise SelectionExecutionError("selection execution checkout is dirty")


def require_ctest_version() -> tuple[int, int]:
    """Require the literal-file selector introduced by CTest 3.29."""

    try:
        result = subprocess.run(
            ["ctest", "--version"],
            check=True,
            capture_output=True,
            text=True,
            shell=False,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise SelectionExecutionError(f"cannot determine CTest version: {error}") from error
    match = re.search(r"ctest version (\d+)\.(\d+)", result.stdout)
    if match is None:
        raise SelectionExecutionError("cannot parse CTest version")
    version = (int(match.group(1)), int(match.group(2)))
    if version < MINIMUM_CTEST_VERSION:
        raise SelectionExecutionError(
            "authoritative literal selection requires CTest 3.29 or newer"
        )
    return version


def write_private_selection(directory: Path, payload: bytes) -> Path:
    """Materialize one owner-private, read-only snapshot for this process."""

    snapshot = directory / "selected-tests.txt"
    descriptor = os.open(snapshot, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as selection_file:
            selection_file.write(payload)
            selection_file.flush()
            os.fsync(selection_file.fileno())
    except BaseException:
        snapshot.unlink(missing_ok=True)
        raise
    snapshot.chmod(0o400)
    return snapshot


def load_policy(config_path: Path, target: str) -> dict[str, Any]:
    with config_path.open("rb") as config_file:
        config = tomllib.load(config_file)
    try:
        policy = config["targets"][target]["changed_surface_selection"]
    except (KeyError, TypeError) as error:
        raise SelectionExecutionError(
            f"config has no changed-surface policy for target {target!r}"
        ) from error
    if not isinstance(policy, dict):
        raise SelectionExecutionError("changed-surface policy is not a table")
    return policy


def declared_literal_tests(policy: dict[str, Any]) -> set[str]:
    names = set(policy.get("baseline_tests", []))
    for family in policy.get("families", []):
        names.update(family.get("tests", []))
        names.update(family.get("extended_tests", []))
    if not names or not all(isinstance(name, str) and name for name in names):
        raise SelectionExecutionError("policy has no valid literal test inventory")
    return names


def validate_build_configuration(build_dir: Path, policy: dict[str, Any]) -> None:
    """Prove the live CMake cache matches every selector build declaration."""

    cache_path = build_dir / "CMakeCache.txt"
    try:
        lines = cache_path.read_text(encoding="utf-8", errors="strict").splitlines()
    except OSError as error:
        raise SelectionExecutionError(f"cannot read CMake cache: {error}") from error
    observed: dict[str, str] = {}
    for line in lines:
        if line.startswith(("//", "#")) or ":" not in line or "=" not in line:
            continue
        key = line.split(":", 1)[0]
        observed[key] = line.split("=", 1)[1]

    build_types = {
        "debug": "Debug",
        "release": "Release",
        "rel_with_deb_info": "RelWithDebInfo",
        "min_size_rel": "MinSizeRel",
    }
    declared_type = policy.get("build_type")
    if declared_type not in build_types:
        raise SelectionExecutionError("policy build_type is missing or unsupported")
    expected = {"CMAKE_BUILD_TYPE": build_types[declared_type]}
    flags = policy.get("build_flags")
    if not isinstance(flags, list):
        raise SelectionExecutionError("policy build_flags is not an array")
    for flag in flags:
        if not isinstance(flag, str) or not flag.startswith("-D") or "=" not in flag[2:]:
            raise SelectionExecutionError(
                f"policy build flag is not an exact -DKEY=VALUE declaration: {flag!r}"
            )
        key, value = flag[2:].split("=", 1)
        if not key or (key in expected and expected[key] != value):
            raise SelectionExecutionError(f"conflicting policy build flag: {flag!r}")
        expected[key] = value

    mismatches = [
        f"{key}: expected {value!r}, observed {observed.get(key)!r}"
        for key, value in sorted(expected.items())
        if observed.get(key) != value
    ]
    if mismatches:
        raise SelectionExecutionError(
            "live CMake configuration differs from selector policy: " + "; ".join(mismatches)
        )


def ctest_json(build_dir: Path, selected_file: Path | None = None) -> list[dict[str, Any]]:
    command = ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"]
    if selected_file is not None:
        command.extend(["--tests-from-file", str(selected_file)])
    try:
        result = subprocess.run(
            command,
            check=True,
            capture_output=True,
            text=True,
            shell=False,
        )
        payload = json.loads(result.stdout)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        raise SelectionExecutionError(f"CTest inventory query failed: {error}") from error
    tests = payload.get("tests")
    if not isinstance(tests, list):
        raise SelectionExecutionError("CTest JSON has no tests array")
    return tests


def validate_selection(
    *,
    selected_names: Sequence[str],
    full_tests: list[dict[str, Any]],
    selected_tests: list[dict[str, Any]],
    source_root: Path,
    build_dir: Path,
    policy: dict[str, Any],
    contract: dict[str, Any],
    target: str,
) -> None:
    """Fail closed unless CTest's file selection equals the reviewed expansion."""

    baseline = policy.get("baseline_tests")
    if not isinstance(baseline, list) or not set(baseline).issubset(selected_names):
        raise SelectionExecutionError("selected-tests file omits the mandatory baseline")
    undeclared = sorted(set(selected_names) - declared_literal_tests(policy))
    if undeclared:
        raise SelectionExecutionError(f"selection contains undeclared names: {undeclared}")

    live_manifest = inventory.build_manifest(
        full_tests,
        source_root,
        build_dir,
        policy,
        target=target,
    )
    inventory.validate_manifest(live_manifest, contract)
    expected_groups = inventory.expand_literal_selection(live_manifest, selected_names)
    observed_groups = inventory.inventory_groups(selected_tests, source_root, build_dir)
    if inventory.canonical_json(expected_groups) != inventory.canonical_json(observed_groups):
        raise SelectionExecutionError(
            "CTest --tests-from-file expansion differs from the reviewed literal selection"
        )


def cmake_artifact_targets(build_dir: Path) -> tuple[set[str], dict[Path, str]]:
    """Read the configure-produced CMake File API target/artifact projection."""

    reply = build_dir / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json"))
    if len(indexes) != 1:
        raise SelectionExecutionError(
            f"CMake File API must contain one exact index, found {len(indexes)}"
        )
    try:
        index = json.loads(indexes[0].read_text(encoding="utf-8"))
        codemodel_name = index["reply"]["codemodel-v2"]["jsonFile"]
        codemodel = json.loads((reply / codemodel_name).read_text(encoding="utf-8"))
    except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        raise SelectionExecutionError(f"CMake File API codemodel is unavailable: {error}") from error
    configurations = codemodel.get("configurations")
    if not isinstance(configurations, list) or len(configurations) != 1:
        raise SelectionExecutionError("CMake File API must contain one build configuration")
    target_names: set[str] = set()
    artifacts: dict[Path, str] = {}
    targets = configurations[0].get("targets")
    if not isinstance(targets, list):
        raise SelectionExecutionError("CMake File API codemodel has no target list")
    for reference in targets:
        try:
            target = json.loads((reply / reference["jsonFile"]).read_text(encoding="utf-8"))
            name = target["name"]
        except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
            raise SelectionExecutionError(f"CMake File API target is invalid: {error}") from error
        if not isinstance(name, str) or not name:
            raise SelectionExecutionError("CMake File API target has no canonical name")
        target_names.add(name)
        for artifact in target.get("artifacts", []):
            path = artifact.get("path") if isinstance(artifact, dict) else None
            if not isinstance(path, str) or not path:
                continue
            resolved = (build_dir / path).resolve()
            prior = artifacts.get(resolved)
            if prior is not None and prior != name:
                raise SelectionExecutionError(
                    f"CMake artifact {resolved} is produced by multiple targets"
                )
            artifacts[resolved] = name
    return target_names, artifacts


def validate_build_target_projection(
    *,
    build_dir: Path,
    selected_tests: list[dict[str, Any]],
    selected_build_targets: Sequence[str],
) -> None:
    """Prove selected native test commands are materialized by selected targets."""

    if not selected_build_targets:
        raise SelectionExecutionError("selected build execution has no producer targets")
    target_names, artifacts = cmake_artifact_targets(build_dir)
    missing_targets = sorted(set(selected_build_targets) - target_names)
    if missing_targets:
        raise SelectionExecutionError(
            f"selected CMake targets are absent from the codemodel: {missing_targets}"
        )
    selected = set(selected_build_targets)
    build_root = build_dir.resolve()
    for test in selected_tests:
        command = test.get("command")
        if not isinstance(command, list) or not command or not isinstance(command[0], str):
            raise SelectionExecutionError("selected CTest command is malformed")
        executable = Path(command[0])
        if not executable.is_absolute():
            continue
        resolved = executable.resolve()
        if not resolved.is_relative_to(build_root):
            continue
        producer = artifacts.get(resolved)
        if producer is None:
            raise SelectionExecutionError(
                f"selected native CTest executable has no CMake producer: {resolved}"
            )
        if producer not in selected:
            raise SelectionExecutionError(
                f"selected native CTest executable requires undeclared target {producer}"
            )


def execution_argv(build_dir: Path, selected_file: Path | None = None) -> list[str]:
    """Return the exact shell-free governed CTest argv."""

    command = [
        str(REPO_ROOT / "tools" / "ci" / "governed-build.sh"),
        "ctest",
        "--test-dir",
        str(build_dir),
        "--output-on-failure",
        "--repeat",
        "until-pass:2",
        "--exclude-regex",
        EXCLUDE_NAME,
        "--label-exclude",
        EXCLUDE_LABEL,
        "--no-tests=error",
    ]
    if selected_file is not None:
        command.extend(["--tests-from-file", str(selected_file)])
    return command


def build_argv(build_dir: Path, targets: Sequence[str] = ()) -> list[str]:
    command = [
        str(REPO_ROOT / "tools" / "ci" / "governed-build.sh"),
        "cmake",
        "--build",
        str(build_dir),
    ]
    if targets:
        command.extend(["--target", *targets])
    return command


def clear_build_sentinel(build_dir: Path) -> int:
    return subprocess.run(
        [
            str(REPO_ROOT / "tools" / "ci" / "build-dir-sentinel.sh"),
            "clear",
            str(build_dir),
        ],
        shell=False,
    ).returncode


def failure_coverage(selected_result: int, full_result: int | None) -> str:
    if full_result is None:
        return "not_compared"
    if selected_result != 0 and full_result != 0:
        return "failure_observed_by_selected"
    if selected_result == 0 and full_result != 0:
        return "missed_full_failure"
    if selected_result != 0 and full_result == 0:
        return "selected_only_failure"
    return "no_failure_observed"


def comparison_verdict(selected_result: int, full_result: int | None) -> str:
    if full_result is None:
        return "not_compared"
    if selected_result == 0 and full_result == 0:
        return "matched_pass"
    if selected_result != 0 and full_result != 0:
        return "failure_overlap_unproven"
    return "mismatched_non_graduation"


def fsync_directory(directory: Path) -> None:
    """Persist a newly published receipt's directory entry on POSIX."""

    descriptor = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_result_receipt(result_dir: Path, receipt: dict[str, Any]) -> Path:
    """Append one owner-private immutable result receipt without overwriting."""

    if not result_dir.is_absolute():
        raise SelectionExecutionError("result receipt directory must be absolute")
    result_dir.mkdir(parents=True, exist_ok=True)
    payload = (json.dumps(receipt, sort_keys=True, separators=(",", ":")) + "\n").encode()
    for sequence in range(100):
        path = result_dir / f"result-{time.time_ns()}-{os.getpid()}-{sequence}.json"
        try:
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o400)
        except FileExistsError:
            continue
        with os.fdopen(descriptor, "wb") as result_file:
            result_file.write(payload)
            result_file.flush()
            os.fsync(result_file.fileno())
        fsync_directory(result_dir)
        return path
    raise SelectionExecutionError("cannot allocate immutable result receipt")


def run_locked(args: argparse.Namespace, build_dir: Path) -> int:
    """Verify and execute one selection while the build tree is exclusive."""

    verification_started = time.monotonic()
    config_path = args.config.resolve(strict=True)
    contract_path = args.inventory_contract.resolve(strict=True)
    (
        selected_names,
        selected_payload,
        selected_build_targets,
        _selected_build_target_payload,
        selection_receipt,
    ) = decode_selection_receipt(args.selection_receipt_b64, args.selection_receipt_sha256)
    validate_receipt_identity(selection_receipt, args.target)
    require_ctest_version()
    policy = load_policy(config_path, args.target)
    contract = json.loads(contract_path.read_text(encoding="utf-8"))
    source_root = inventory.source_root_for_build(build_dir).resolve(strict=True)
    if source_root != REPO_ROOT.resolve():
        raise SelectionExecutionError(
            f"build source root {source_root} does not match checkout {REPO_ROOT.resolve()}"
        )
    validate_build_configuration(build_dir, policy)
    with tempfile.TemporaryDirectory(prefix="pulp-changed-surface-") as directory:
        selected_file = write_private_selection(Path(directory), selected_payload)
        snapshot_identity = selected_file.stat()
        full_tests = ctest_json(build_dir)
        selected_tests = ctest_json(build_dir, selected_file)
        validate_selection(
            selected_names=selected_names,
            full_tests=full_tests,
            selected_tests=selected_tests,
            source_root=source_root,
            build_dir=build_dir,
            policy=policy,
            contract=contract,
            target=args.target,
        )
        if selection_receipt["schema_version"] == 2:
            validate_build_target_projection(
                build_dir=build_dir,
                selected_tests=selected_tests,
                selected_build_targets=selected_build_targets,
            )
        verification_seconds = time.monotonic() - verification_started
        selected_build_result: int | None = None
        selected_build_seconds: float | None = None
        if selection_receipt["schema_version"] == 2:
            selected_build_started = time.monotonic()
            selected_build_result = subprocess.run(
                build_argv(build_dir, selected_build_targets), shell=False
            ).returncode
            selected_build_seconds = time.monotonic() - selected_build_started
        selected_seconds = 0.0
        if selected_build_result in (None, 0):
            selected_started = time.monotonic()
            selected_result = subprocess.run(
                execution_argv(build_dir, selected_file), shell=False
            ).returncode
            selected_seconds = time.monotonic() - selected_started
        else:
            selected_result = selected_build_result
        compare_full = os.environ.get("SHIPYARD_CHANGED_SURFACE_COMPARE_FULL") == "1"
        full_build_result: int | None = None
        full_build_seconds: float | None = None
        full_result: int | None = None
        full_seconds: float | None = None
        if compare_full:
            if selection_receipt["schema_version"] == 2:
                full_build_started = time.monotonic()
                full_build_result = subprocess.run(build_argv(build_dir), shell=False).returncode
                full_build_seconds = time.monotonic() - full_build_started
                if full_build_result == 0:
                    full_build_result = clear_build_sentinel(build_dir)
            full_seconds = 0.0
            if full_build_result in (None, 0):
                full_started = time.monotonic()
                full_result = subprocess.run(
                    execution_argv(build_dir), shell=False
                ).returncode
                full_seconds = time.monotonic() - full_started
            else:
                full_result = full_build_result
        elif selection_receipt["schema_version"] == 2 and selected_result == 0:
            selected_result = clear_build_sentinel(build_dir)
        final_identity = selected_file.stat()
        if (
            (snapshot_identity.st_dev, snapshot_identity.st_ino)
            != (final_identity.st_dev, final_identity.st_ino)
            or selected_file.read_bytes() != selected_payload
        ):
            raise SelectionExecutionError("private selected-tests snapshot changed during execution")
        result_dir = os.environ.get("SHIPYARD_CHANGED_SURFACE_RESULT_DIR")
        if result_dir:
            verdict = comparison_verdict(selected_result, full_result)
            write_result_receipt(
                Path(result_dir),
                {
                    "schema_version": 2,
                    "recorded_at_unix_ns": time.time_ns(),
                    "repository": selection_receipt["repository"],
                    "pull_request": selection_receipt["pull_request"],
                    "target": selection_receipt["target"],
                    "base_sha": selection_receipt["base_sha"],
                    "head_sha": selection_receipt["head_sha"],
                    "tree_sha": selection_receipt["tree_sha"],
                    "execution_payload_sha256": args.selection_receipt_sha256,
                    "policy_digest": selection_receipt["policy_digest"],
                    "selection_receipt_digest": selection_receipt[
                        "selection_receipt_digest"
                    ],
                    "validation_contract_digest": selection_receipt[
                        "validation_contract_digest"
                    ],
                    "workflow_digest": selection_receipt["workflow_digest"],
                    "selected_tests_digest": selection_receipt["selected_tests_digest"],
                    "selected_logical_count": len(selected_names),
                    "selected_registration_count": len(selected_tests),
                    "full_registration_count": len(full_tests),
                    "verification_duration_seconds": verification_seconds,
                    "selected_duration_seconds": selected_seconds,
                    "selected_returncode": selected_result,
                    "selected_build_targets_digest": selection_receipt.get(
                        "selected_build_targets_digest"
                    ),
                    "selected_build_target_count": len(selected_build_targets),
                    "selected_build_duration_seconds": selected_build_seconds,
                    "selected_build_returncode": selected_build_result,
                    "full_duration_seconds": full_seconds,
                    "full_returncode": full_result,
                    "full_build_duration_seconds": full_build_seconds,
                    "full_build_returncode": full_build_result,
                    "full_authoritative": compare_full,
                    "failure_coverage": failure_coverage(selected_result, full_result),
                    "comparison_verdict": verdict,
                    "graduation_eligible": compare_full and verdict == "matched_pass",
                },
            )
        return full_result if full_result is not None else selected_result


def run(args: argparse.Namespace) -> int:
    build_dir = args.build_dir.resolve(strict=True)
    with build_dir_lock.exclusive_build_dir(build_dir):
        return run_locked(args, build_dir)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--selection-receipt-b64", required=True)
    parser.add_argument("--selection-receipt-sha256", required=True)
    parser.add_argument("--build-dir", default=REPO_ROOT / "build", type=Path)
    parser.add_argument("--config", default=DEFAULT_CONFIG, type=Path)
    parser.add_argument("--inventory-contract", default=DEFAULT_CONTRACT, type=Path)
    parser.add_argument("--target", default="mac")
    return parser.parse_args(argv)


def main() -> int:
    try:
        return run(parse_args())
    except (SelectionExecutionError, inventory.InventoryError, OSError, json.JSONDecodeError) as error:
        print(f"changed-surface execution refused: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
