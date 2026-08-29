#!/usr/bin/env python3
"""Verify a terminal exact-head A2T offline-analysis acceptance receipt."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

import gpu_trace_overhead_acceptance as contract


EXPECTED_PLAN_REVISION = "641649b7e7fece6baae34380b6e719904506af22"
EXPECTED_PLAN_SHA256 = "00bdb8bd55fb90fb42d98a09442d2b168505a23a4208cb5b9edb67b01de69f07"
EXPECTED_PLAN_BLOB = "2d1c461d3ea640f75786a72c312d074f68f59028"
SOURCE_STAMP = re.compile(r"^[0-9a-f]{7,40}$")
_SCOPE_CACHE: dict[tuple[str, str], dict[str, Any]] = {}


def _object(value: Any, label: str, errors: list[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.append(f"{label} must be an object")
        return {}
    return value


def _gitlinks(repository: Path, revision: str) -> dict[str, str]:
    completed = subprocess.run(
        ["git", "ls-tree", "-rz", "-r", revision],
        cwd=repository, check=False, capture_output=True,
    )
    if completed.returncode != 0:
        return {}
    values: dict[str, str] = {}
    for raw in completed.stdout.split(b"\0"):
        if not raw:
            continue
        try:
            metadata, path = raw.split(b"\t", 1)
            mode, kind, value = metadata.decode("ascii").split()
            decoded = path.decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            return {}
        if mode == "160000" and kind == "commit" and contract.valid_lower_hex(
            value, 40
        ):
            values[decoded] = value
    return values


def _artifact_trace_path(receipt: dict[str, Any], repository: Path) -> Path | None:
    artifacts = receipt.get("artifacts")
    trace = artifacts.get("trace") if isinstance(artifacts, dict) else None
    role = trace.get("role") if isinstance(trace, dict) else None
    if not isinstance(role, str) or not role.startswith("repository/"):
        return None
    return repository / role.removeprefix("repository/")


def _scope_inventory(repository: Path, revision: str) -> dict[str, Any]:
    key = (str(repository.resolve()), revision)
    if key not in _SCOPE_CACHE:
        _SCOPE_CACHE[key] = contract.a2t_scope_inventory(repository, revision)
    return json.loads(json.dumps(_SCOPE_CACHE[key]))


def _verify_producer_disposition(
    receipt: dict[str, Any], repository: Path, source_revision: Any, errors: list[str]
) -> None:
    disposition = _object(
        receipt.get("producer_overhead_disposition"),
        "producer_overhead_disposition", errors,
    )
    expected_fields = {
        "status": "not-applicable-no-a2t-scoped-producer-cost",
        "formal_plan_status": "accepted-canonical-plan",
        "formal_plan_revision": EXPECTED_PLAN_REVISION,
        "formal_plan_sha256": EXPECTED_PLAN_SHA256,
        "non_a2t_owner_followup": (
            "The input-to-present latency tracing and A3 packages must each provide or "
            "bind tracing-off, tracing-on/idle, and active-capture overhead/control "
            "evidence for their later product producers; A2T does not evaluate that "
            "owner evidence."
        ),
        "required_followup": (
            "B6 must run the three-state 5-warmup/30-trial and 20 fresh-process "
            "protocol when Vellum producer instrumentation is added."
        ),
    }
    for field, expected in expected_fields.items():
        if disposition.get(field) != expected:
            errors.append(f"producer overhead disposition {field} is not the accepted contract")
    evidence = _object(
        disposition.get("evidence"), "producer overhead disposition evidence", errors
    )
    try:
        expected = _scope_inventory(repository, str(source_revision))
    except ValueError as error:
        errors.append(f"cannot recompute A2T path-scoped tree delta: {error}")
        return
    if evidence != expected:
        errors.append(
            "producer overhead evidence differs from the complete path-scoped tree delta"
        )
    if (
        evidence.get("method") != "immutable-base-to-source path-scoped tree delta"
        or evidence.get("no_a2t_scoped_producer_delta") is not True
        or evidence.get("a2t_scoped_producer_paths") != []
    ):
        errors.append("producer overhead disposition lacks a passing path-scoped tree delta")
    external = evidence.get("non_a2t_product_producers")
    if not isinstance(external, list) or not external or any(
        not isinstance(row, dict)
        or row.get("overhead_evidence_required_from_owner") is not True
        or row.get("owner_evidence_status") != "external-not-evaluated-by-a2t"
        for row in external
    ) or {
        row.get("owner_package") for row in external if isinstance(row, dict)
    } != {
        "input-to-present-latency-tracing",
        "A3-first-visible-product-evidence",
    }:
        errors.append("producer evidence does not expose every later non-A2T product producer")


def _verify_installed_build_provenance(
    installed: dict[str, Any], artifacts: dict[str, Any], source_revision: Any,
    errors: list[str],
) -> None:
    provenance = _object(
        installed.get("build_provenance"), "installed build_provenance", errors
    )
    if (
        provenance.get("method")
        != "fresh-external-cmake-build-install-byte-identity-v1"
        or provenance.get("install_prefix_initial_state")
        != "absent-and-atomically-claimed"
        or provenance.get("install_prefix_claim_method")
        != "unpredictable-staging-directory-renameatx-noreplace-retained-fd-v1"
    ):
        errors.append("installed binaries lack a fresh isolated build/install proof")
    prefix_claim = provenance.get("install_prefix_claim")
    if (
        not isinstance(prefix_claim, dict)
        or set(prefix_claim) != {"device", "inode"}
        or any(
            not isinstance(prefix_claim.get(field), int)
            or isinstance(prefix_claim.get(field), bool)
            or prefix_claim[field] < 0
            for field in ("device", "inode")
        )
    ):
        errors.append("installed build provenance lacks the claimed prefix identity")
    if not contract.valid_lower_hex(str(provenance.get("cmake_cache_sha256", "")), 64):
        errors.append("installed build provenance lacks an exact CMake cache digest")
    if provenance.get("cmake_home_revision") != source_revision:
        errors.append("installed build CMake source revision differs from the receipt")
    if provenance.get("build_targets") != list(contract.BUILD_TARGETS):
        errors.append("installed build provenance does not bind the exact CLI/MCP targets")
    if provenance.get("build_settings") != contract.REQUIRED_BUILD_SETTINGS:
        errors.append("installed build provenance is not the required Release GPU build")
    source_claim = provenance.get("source_tree_claim")
    try:
        expected_tree = contract._git_text(
            repository, "rev-parse", f"{source_revision}^{{tree}}"
        )
    except ValueError:
        expected_tree = ""
    if (
        not isinstance(source_claim, dict)
        or source_claim.get("method")
        != "retained-git-tree-vnode-and-descriptor-v1"
        or source_claim.get("revision") != source_revision
        or source_claim.get("root_tree") != expected_tree
        or source_claim.get("overlay_paths") != []
        or source_claim.get("excluded_gitlinks") != _gitlinks(repository, str(source_revision))
        or not isinstance(source_claim.get("regular_or_symlink_files"), int)
        or isinstance(source_claim.get("regular_or_symlink_files"), bool)
        or source_claim["regular_or_symlink_files"] <= 0
        or not isinstance(source_claim.get("retained_directories"), int)
        or isinstance(source_claim.get("retained_directories"), bool)
        or source_claim["retained_directories"] <= 0
    ):
        errors.append("installed build provenance lacks the retained exact Git source tree")
    build_input_claim = provenance.get("build_input_claim")
    if (
        not isinstance(build_input_claim, dict)
        or build_input_claim.get("method")
        != "regenerated-cmake-ninja-input-descriptor-v1"
        or not isinstance(build_input_claim.get("file_count"), int)
        or isinstance(build_input_claim.get("file_count"), bool)
        or build_input_claim["file_count"] < 3
        or not contract.valid_lower_hex(
            str(build_input_claim.get("manifest_sha256", "")), 64
        )
        or build_input_claim.get("forced_clean_targets")
        != list(contract.BUILD_TARGETS)
    ):
        errors.append("installed build provenance lacks retained forced-clean build inputs")
    binaries = provenance.get("binaries")
    expected_roles = {
        "pulp": ("installed-prefix/bin/pulp", "external-build/pulp", "cli"),
        "pulp-cpp": (
            "installed-prefix/bin/pulp-cpp",
            "external-build/tools/cli/pulp-cpp",
            None,
        ),
        "pulp-mcp": (
            "installed-prefix/bin/pulp-mcp",
            "external-build/tools/mcp/pulp-mcp",
            "mcp",
        ),
    }
    if not isinstance(binaries, dict) or set(binaries) != set(expected_roles):
        errors.append("installed build provenance lacks the exact binary inventory")
        return
    for name, (installed_role, build_role, artifact_role) in expected_roles.items():
        row = binaries.get(name)
        if not isinstance(row, dict):
            errors.append(f"installed build provenance {name} row is malformed")
            continue
        digest = row.get("sha256")
        if (
            row.get("installed_role") != installed_role
            or row.get("build_output_role") != build_role
            or not isinstance(digest, str)
            or not contract.valid_lower_hex(digest, 64)
            or row.get("build_output_sha256") != digest
            or not isinstance(row.get("bytes"), int)
            or isinstance(row.get("bytes"), bool)
            or row["bytes"] <= 0
            or not isinstance(row.get("build_output_bytes"), int)
            or isinstance(row.get("build_output_bytes"), bool)
            or row["build_output_bytes"] != row["bytes"]
        ):
            errors.append(f"installed {name} is not byte-identical to its exact build output")
        if artifact_role is not None:
            artifact = artifacts.get(artifact_role)
            if (
                not isinstance(artifact, dict)
                or artifact.get("sha256") != digest
                or artifact.get("bytes") != row.get("bytes")
            ):
                errors.append(f"measured {artifact_role} artifact differs from build provenance")
    if binaries["pulp"].get("sha256") == binaries["pulp-cpp"].get("sha256"):
        errors.append("installed pulp does not prove the required Rust front")


def verify(
    receipt: Any, repository: Path, *, require_terminal: bool = True
) -> list[str]:
    errors: list[str] = []
    if not isinstance(receipt, dict):
        return ["A2T receipt must be an object"]
    repository = repository.resolve()
    if receipt.get("schema") != "pulp.gpu-trace-overhead-acceptance.v2":
        errors.append("receipt schema must be pulp.gpu-trace-overhead-acceptance.v2")
    errors.extend(contract.source_binding_errors(receipt, repository))

    source_revision = receipt.get("source_revision")
    if receipt.get("mcp_source_revision") != source_revision:
        errors.append("CLI and MCP source revisions differ")
    source_identity = _object(receipt.get("source_identity"), "source_identity", errors)
    if source_identity.get("repository") != "Generous-Corp/pulp":
        errors.append("source identity does not name canonical Pulp")
    if source_identity.get("revision") != source_revision:
        errors.append("source identity revision differs from the receipt")
    if source_identity.get("clean") is not True:
        errors.append("source identity is not clean")

    installed = _object(
        receipt.get("installed_source_identity"), "installed_source_identity", errors
    )
    build_info = _object(installed.get("build_info"), "installed build_info", errors)
    if (
        installed.get("prefix_role") != "isolated-clean-release-install"
        or installed.get("build_info_role")
        != "installed-prefix/include/pulp/runtime/build_info.hpp"
    ):
        errors.append("installed source identity does not name the isolated prefix stamp")
    if installed.get("source_revision") != source_revision:
        errors.append("installed source revision differs from the receipt")
    if build_info.get("kBuildType") != "Release" or build_info.get("kGitDirty") is not False:
        errors.append("installed CLI/MCP are not stamped as a clean Release build")
    stamped_sha = build_info.get("kGitSha")
    if (
        not isinstance(stamped_sha, str)
        or SOURCE_STAMP.fullmatch(stamped_sha) is None
        or not isinstance(source_revision, str)
        or not source_revision.startswith(stamped_sha)
    ):
        errors.append("installed build stamp is not bound to source_revision")
    if not contract.valid_lower_hex(str(installed.get("build_info_sha256", "")), 64):
        errors.append("installed build_info lacks an exact SHA-256")

    plan = _object(receipt.get("accepted_plan"), "accepted_plan", errors)
    expected_plan = {
        "repository": "danielraffel/pulp-planning",
        "revision": EXPECTED_PLAN_REVISION,
        "path": contract.PLAN_PATH,
        "blob": EXPECTED_PLAN_BLOB,
        "sha256": EXPECTED_PLAN_SHA256,
    }
    for key, expected in expected_plan.items():
        if plan.get(key) != expected:
            errors.append(f"accepted plan {key} does not match the canonical plan")
    _verify_producer_disposition(receipt, repository, source_revision, errors)
    artifacts = _object(receipt.get("artifacts"), "artifacts", errors)
    sibling = _object(artifacts.get("sibling_binding"), "sibling_binding", errors)
    if sibling.get("verified_same_resolved_parent") is not True:
        errors.append("installed CLI/MCP sibling binding did not pass")
    for role in ("cli", "mcp"):
        artifact = _object(artifacts.get(role), f"artifacts.{role}", errors)
        executable = "pulp" if role == "cli" else "pulp-mcp"
        if artifact.get("role") != f"installed-prefix/bin/{executable}":
            errors.append(f"{role} binary role does not name the installed sibling")
        if not contract.valid_lower_hex(str(artifact.get("sha256", "")), 64):
            errors.append(f"{role} binary lacks an exact SHA-256")
        if (
            not isinstance(artifact.get("bytes"), int)
            or isinstance(artifact.get("bytes"), bool)
            or artifact.get("bytes", 0) <= 0
        ):
            errors.append(f"{role} binary has no positive byte count")
    _verify_installed_build_provenance(
        installed, artifacts, source_revision, errors
    )

    processor = _object(artifacts.get("trace_processor"), "trace_processor", errors)
    version = processor.get("version")
    platform_key = processor.get("platform")
    if version != contract.PINNED_PROCESSOR_VERSION:
        errors.append("trace processor version differs from the SDK match")
    if platform_key not in contract.PROCESSOR_SHA256:
        errors.append("trace processor platform is not a pinned platform")
    elif processor.get("sha256") != contract.PROCESSOR_SHA256[platform_key]:
        errors.append("trace processor digest differs from the source pin")
    if not str(processor.get("version_output", "")).startswith(f"Perfetto {version}-"):
        errors.append("trace processor version output is missing or incoherent")
    try:
        source_version, source_pins = contract._processor_source_contract(repository)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        errors.append(f"cannot verify current processor source contract: {error}")
    else:
        if source_version != version or source_pins.get(str(platform_key)) != processor.get("sha256"):
            errors.append("current source processor contract differs from the receipt")

    trace_path = _artifact_trace_path(receipt, repository)
    trace_artifact = _object(artifacts.get("trace"), "artifacts.trace", errors)
    if trace_path is None or not trace_path.is_file():
        errors.append("measured trace does not resolve to a checked-in fixture")
    else:
        if contract.sha256(trace_path) != trace_artifact.get("sha256"):
            errors.append("measured trace digest differs from the checked-in artifact")
        if trace_path.stat().st_size != trace_artifact.get("bytes"):
            errors.append("measured trace byte count differs from the checked-in artifact")

    protocol = _object(receipt.get("protocol"), "protocol", errors)
    expected_protocol = {
        "question": "gpu-startup",
        "warmups": 5,
        "measured_paired_trials": 30,
        "fresh_start_paired_trials": 20,
        "order": "alternating cli-first/mcp-first",
        "environment_path": "/usr/bin:/bin:/usr/sbin:/sbin",
    }
    for key, expected in expected_protocol.items():
        if protocol.get(key) != expected:
            errors.append(f"protocol {key} differs from terminal A2T acceptance")

    measured = _object(receipt.get("measured"), "measured", errors)
    fresh = _object(receipt.get("fresh_start"), "fresh_start", errors)
    raw = measured.get("raw_samples")
    fresh_raw = fresh.get("raw_samples")
    if not isinstance(raw, list) or len(raw) != 30:
        errors.append("measured raw sample count must be exactly 30")
    if not isinstance(fresh_raw, list) or len(fresh_raw) != 20:
        errors.append("fresh-process raw sample count must be exactly 20")
    summaries = (
        (measured.get("cli"), 30, "measured CLI"),
        (measured.get("persistent_mcp_request"), 30, "measured MCP"),
        (fresh.get("cli"), 20, "fresh CLI"),
        (fresh.get("mcp_process_initialize_request_shutdown"), 20, "fresh MCP"),
    )
    for value, count, label in summaries:
        if not isinstance(value, dict) or value.get("count") != count:
            errors.append(f"{label} summary count must be exactly {count}")
    for rows, label in ((raw, "measured"), (fresh_raw, "fresh")):
        if not isinstance(rows, list):
            continue
        for index, row in enumerate(rows):
            expected_order = "cli-first" if index % 2 == 0 else "mcp-first"
            if not isinstance(row, dict) or row.get("trial") != index + 1 or row.get("order") != expected_order:
                errors.append(f"{label} trial {index + 1} is missing alternating-order identity")
                break
            duration_fields = (
                ("cli_duration_ns", "mcp_duration_ns")
                if label == "measured"
                else ("cli_duration_ns", "mcp_process_initialize_request_shutdown_duration_ns")
            )
            if any(type(row.get(field)) is not int or row[field] <= 0 for field in duration_fields):
                errors.append(f"{label} trial {index + 1} lacks positive raw durations")
                break

    measured_rows_valid = (
        isinstance(raw, list) and len(raw) == 30
        and all(
            isinstance(row, dict)
            and type(row.get("cli_duration_ns")) is int
            and row["cli_duration_ns"] > 0
            and type(row.get("mcp_duration_ns")) is int
            and row["mcp_duration_ns"] > 0
            for row in raw
        )
    )
    if measured_rows_valid:
        measured_cli = [row["cli_duration_ns"] for row in raw]
        measured_mcp = [row["mcp_duration_ns"] for row in raw]
        if measured.get("cli") != contract.summary(measured_cli):
            errors.append("measured CLI summary does not match raw durations")
        if measured.get("persistent_mcp_request") != contract.summary(measured_mcp):
            errors.append("measured MCP summary does not match raw durations")
        if measured.get("confidence") != contract.paired_delta_confidence(
            measured_cli, measured_mcp
        ):
            errors.append("measured confidence interval does not match raw durations")

    fresh_rows_valid = (
        isinstance(fresh_raw, list) and len(fresh_raw) == 20
        and all(
            isinstance(row, dict)
            and type(row.get("cli_duration_ns")) is int
            and row["cli_duration_ns"] > 0
            and type(row.get("mcp_process_initialize_request_shutdown_duration_ns")) is int
            and row["mcp_process_initialize_request_shutdown_duration_ns"] > 0
            for row in fresh_raw
        )
    )
    if fresh_rows_valid:
        fresh_cli = [row["cli_duration_ns"] for row in fresh_raw]
        fresh_mcp = [
            row["mcp_process_initialize_request_shutdown_duration_ns"]
            for row in fresh_raw
        ]
        if fresh.get("cli") != contract.summary(fresh_cli):
            errors.append("fresh CLI summary does not match raw durations")
        if fresh.get("mcp_process_initialize_request_shutdown") != contract.summary(
            fresh_mcp
        ):
            errors.append("fresh MCP summary does not match raw durations")

    replay = receipt.get("fixture_replay")
    if not isinstance(replay, list) or len(replay) != len(contract.FIXTURE_REPLAY):
        errors.append("fixture replay does not contain the exact required matrix")
        replay = []
    replay_by_case = {
        row.get("case"): row for row in replay if isinstance(row, dict)
    }
    if set(replay_by_case) != {row[0] for row in contract.FIXTURE_REPLAY}:
        errors.append("fixture replay cases differ from the required matrix")
    measured_role = trace_artifact.get("role")
    measured_semantic = receipt.get("semantic_result")
    matching_semantic = None
    for case, filename, question, verdict, dominant, action in contract.FIXTURE_REPLAY:
        row = replay_by_case.get(case)
        if not isinstance(row, dict):
            continue
        if row.get("question") != question or row.get("cli_rerun") != "pass" or row.get("cli_mcp_parity") != "pass":
            errors.append(f"fixture replay {case} lacks CLI rerun/MCP parity")
        trace = _object(row.get("trace"), f"fixture replay {case} trace", errors)
        fixture_path = repository / "test/fixtures/perfetto-gpu" / filename
        if trace.get("role") != f"repository/test/fixtures/perfetto-gpu/{filename}":
            errors.append(f"fixture replay {case} has the wrong trace role")
        elif not fixture_path.is_file() or trace.get("sha256") != contract.sha256(fixture_path):
            errors.append(f"fixture replay {case} has the wrong trace digest")
        elif trace.get("bytes") != fixture_path.stat().st_size:
            errors.append(f"fixture replay {case} has the wrong trace byte count")
        semantic = _object(row.get("semantic_result"), f"fixture replay {case} semantic", errors)
        required_semantic = set(contract.SEMANTIC_FIELDS) - {"unavailable_reason", "dominant_stage"}
        if not required_semantic.issubset(semantic):
            errors.append(f"fixture replay {case} omits required semantic fields")
        if semantic.get("question") != question:
            errors.append(f"fixture replay {case} semantic result has the wrong question")
        if semantic.get("verdict") != verdict or semantic.get("dominant_stage") != dominant:
            errors.append(f"fixture replay {case} has the wrong intended verdict/stage")
        if action is not None:
            actions = semantic.get("next_actions")
            actual = actions[0].get("code") if isinstance(actions, list) and actions and isinstance(actions[0], dict) else None
            if actual != action:
                errors.append(f"fixture replay {case} has the wrong intended next action")
        if measured_role == trace.get("role") and protocol.get("question") == question:
            matching_semantic = semantic
    if measured_semantic != matching_semantic:
        errors.append("measured semantic result is not the same-artifact replay result")

    acceptance = _object(receipt.get("acceptance"), "acceptance", errors)
    expected_acceptance = {
        "semantic_parity": "pass",
        "same_installed_prefix": "pass",
        "offline_latency_budget": "unverified-no-ratified-budget",
        "producer_overhead_budget": "not-applicable-no-a2t-scoped-producer-delta",
        "xrun_check": "not-applicable-offline-no-audio-thread",
    }
    for field, expected in expected_acceptance.items():
        if acceptance.get(field) != expected:
            errors.append(f"acceptance {field} differs from the Horizon-A contract")
    human = receipt.get("human_perfetto_ui_correlation")
    if require_terminal:
        if acceptance.get("terminal_status") != "pass":
            errors.append("receipt is nonterminal")
        if acceptance.get("human_perfetto_ui_correlation") != "pass":
            errors.append("human Perfetto UI correlation did not pass")
        if trace_path is not None and isinstance(measured_semantic, dict):
            try:
                contract.preserve_human_perfetto_ui_correlation(
                    receipt,
                    question=str(protocol.get("question")),
                    trace_sha256=contract.sha256(trace_path),
                    semantic_result=measured_semantic,
                )
            except ValueError as error:
                errors.append(f"human Perfetto UI correlation is invalid: {error}")
    elif human is not None and isinstance(measured_semantic, dict) and trace_path is not None:
        try:
            contract.preserve_human_perfetto_ui_correlation(
                receipt,
                question=str(protocol.get("question")),
                trace_sha256=contract.sha256(trace_path),
                semantic_result=measured_semantic,
            )
        except ValueError as error:
            errors.append(f"optional human Perfetto UI correlation is invalid: {error}")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    parser.add_argument("--allow-nonterminal", action="store_true")
    args = parser.parse_args(argv)
    try:
        payload = json.loads(args.receipt.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"gpu-trace-overhead-acceptance: FAIL: {error}", file=sys.stderr)
        return 1
    errors = verify(payload, args.repository, require_terminal=not args.allow_nonterminal)
    if errors:
        for error in errors:
            print(f"gpu-trace-overhead-acceptance: FAIL: {error}", file=sys.stderr)
        return 1
    print("gpu-trace-overhead-acceptance: ok (v2 exact-head, SDK-matched, same artifact)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
