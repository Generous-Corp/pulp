#!/usr/bin/env python3
"""Canonical dependency receipts and derived A4 DPR v2 authority.

The package envelope deliberately carries no ``protected=true`` assertion.
Terminality is established by the caller proving that these exact receipt
bytes are ordinary blobs at the live protected Pulp main revision.  The A3
product-policy payload is then the only source from which A4 collection
authority and numeric policy inputs are derived.
"""

from __future__ import annotations

import copy
import json
import subprocess
from pathlib import Path
from typing import Any, Callable

import gpu_dpr_v2_evidence as evidence
import json_schema_lite


TERMINAL_SCHEMA = "pulp.gpu-vellum-package-terminal.v1"
TERMINAL_SCHEMA_PATH = Path("docs/contracts/gpu-vellum-package-terminal-v1.schema.json")
BLOCKED_MANIFEST_TEMPLATE = Path("docs/contracts/gpu-dpr-corpus-v2-template.json")
CANONICAL_MANIFEST = Path("test/fixtures/gpu-ux/dpr/manifest.json")
CANONICAL_RESULT = Path("docs/validation/gpu-dpr/terminal-result.json")
DEPENDENCY_PATHS = {
    "a2t-pulp-trace-analysis.terminal.v1": Path(
        "docs/validation/gpu-vellum-adoption/a2t-pulp-trace-analysis.json"
    ),
    "a3-pulp-dpr-product-policy.terminal.v2": Path(
        "docs/validation/gpu-vellum-adoption/a3-pulp-dpr-product-policy.json"
    ),
    "a3-pulp-runtime-control.terminal.v1": Path(
        "docs/validation/gpu-vellum-adoption/a3-pulp-runtime-control.json"
    ),
}
EXPECTED_PACKAGE = {
    "a2t-pulp-trace-analysis.terminal.v1": "a2t-pulp-trace-analysis",
    "a3-pulp-dpr-product-policy.terminal.v2": "a3-pulp-runtime-control",
    "a3-pulp-runtime-control.terminal.v1": "a3-pulp-runtime-control",
}
PRODUCT_POLICY_ID = "a3-pulp-dpr-product-policy.terminal.v2"
A2T_ID = "a2t-pulp-trace-analysis.terminal.v1"
A3_ID = "a3-pulp-runtime-control.terminal.v1"
INSTALLED_REVISION_FIELDS = {"pulp", "forge", "vellum", "provider"}


class TerminalReceiptError(ValueError):
    """Canonical dependency bytes do not establish terminal A4 authority."""


def _schema(repo_root: Path) -> dict[str, Any]:
    document = json.loads((repo_root / TERMINAL_SCHEMA_PATH).read_text(encoding="utf-8"))
    definitions = document.get("$defs", {})

    def expand(node: Any) -> Any:
        if isinstance(node, list):
            return [expand(item) for item in node]
        if not isinstance(node, dict):
            return node
        if "$ref" in node:
            prefix = "#/$defs/"
            reference = node["$ref"]
            if not reference.startswith(prefix):
                raise TerminalReceiptError(f"unsupported terminal schema ref: {reference}")
            merged = copy.deepcopy(definitions[reference.removeprefix(prefix)])
            merged.update({key: value for key, value in node.items() if key != "$ref"})
            return expand(merged)
        return {key: expand(value) for key, value in node.items() if key != "$defs"}

    return expand(document)


def _git(repo_root: Path, arguments: list[str], *, text: bool = False) -> bytes | str:
    completed = subprocess.run(
        ["git", *arguments], cwd=repo_root, check=True, capture_output=True,
        text=text, timeout=30,
    )
    return completed.stdout


def git_head(repo_root: Path) -> str:
    return str(_git(repo_root, ["rev-parse", "HEAD"], text=True)).strip()


def canonical_blob(repo_root: Path, relative: Path, revision: str) -> dict[str, Any]:
    """Bind one safe working file to the exact ordinary Git blob at revision."""
    relative_text = relative.as_posix()
    evidence.safe_relative(relative_text)
    path = evidence.checked_regular_path(repo_root, relative_text, "canonical repository")
    dirty = str(_git(
        repo_root,
        ["status", "--porcelain=v1", "--untracked-files=all", "--", relative_text],
        text=True,
    ))
    if dirty:
        raise TerminalReceiptError(f"canonical repository path is dirty: {relative_text}")
    listing = bytes(_git(repo_root, ["ls-tree", "-z", revision, "--", relative_text]))
    rows = [row for row in listing.split(b"\0") if row]
    if len(rows) != 1 or b"\t" not in rows[0]:
        raise TerminalReceiptError(f"canonical path is not one Git object: {relative_text}")
    header, named = rows[0].split(b"\t", 1)
    pieces = header.decode("ascii").split(" ")
    if len(pieces) != 3 or named.decode("utf-8") != relative_text:
        raise TerminalReceiptError(f"canonical Git object identity is malformed: {relative_text}")
    mode, object_type, oid = pieces
    if mode != "100644" or object_type != "blob" or not evidence._is_lower_hex(oid, 40):
        raise TerminalReceiptError(
            f"canonical path must be an ordinary 100644 blob: {relative_text}"
        )
    git_payload = bytes(_git(repo_root, ["cat-file", "blob", oid]))
    file_digest, byte_count, file_payload = evidence.file_identity(
        path, "canonical repository file", max_bytes=evidence.MAX_JSON_BYTES, retain=True,
    )
    if git_payload != file_payload:
        raise TerminalReceiptError(f"working bytes differ from the Git blob: {relative_text}")
    return {
        "path": relative_text,
        "type": object_type,
        "mode": mode,
        "blob": oid,
        "sha256": file_digest,
        "bytes": byte_count,
    }


def _canonical_receipt(
    repo_root: Path, receipt_id: str, live_head: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    relative = DEPENDENCY_PATHS[receipt_id]
    blob = canonical_blob(repo_root, relative, live_head)
    document, digest, byte_count = evidence.regular_json(
        repo_root, relative.as_posix(), f"canonical {receipt_id}"
    )
    if digest != blob["sha256"] or byte_count != blob["bytes"]:
        raise TerminalReceiptError(f"canonical receipt changed while reading: {receipt_id}")
    problems = json_schema_lite.validate(document, _schema(repo_root))
    if problems:
        raise TerminalReceiptError(f"{receipt_id}: {'; '.join(problems)}")
    if (
        document.get("id") != receipt_id
        or document.get("package_id") != EXPECTED_PACKAGE[receipt_id]
        or document.get("repository") != "Generous-Corp/pulp"
        or document.get("disposition") not in {"pass", "no-change"}
    ):
        raise TerminalReceiptError(f"canonical receipt identity differs: {receipt_id}")
    installed = document.get("installed_revisions")
    if not isinstance(installed, dict) or set(installed) != INSTALLED_REVISION_FIELDS:
        raise TerminalReceiptError(f"canonical receipt lacks closed installed revisions: {receipt_id}")
    revision = document.get("revision")
    if subprocess.run(
        ["git", "merge-base", "--is-ancestor", str(revision), live_head],
        cwd=repo_root, capture_output=True, timeout=30,
    ).returncode != 0:
        raise TerminalReceiptError(f"receipt revision is not on protected-main history: {receipt_id}")
    dependencies = document.get("dependencies")
    dependency_ids = [item.get("id") for item in dependencies]
    if dependency_ids != sorted(dependency_ids) or len(dependency_ids) != len(set(dependency_ids)):
        raise TerminalReceiptError(f"receipt dependencies are not exact/sorted: {receipt_id}")
    return document, blob


def _digest(document: dict[str, Any]) -> str:
    return evidence.canonical_sha256(document)


def _positive_integer(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise TerminalReceiptError(f"A3 product policy lacks positive {label}")
    return value


def validate_dependencies(
    repo_root: Path, live_head: str,
    verify_remote_policy: Callable[[dict[str, Any]], None],
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    """Read the three fixed terminal receipts and prove their cross-lane chain."""
    if git_head(repo_root) != live_head:
        raise TerminalReceiptError("dependency validation is not at exact live protected main")
    documents: dict[str, dict[str, Any]] = {}
    blobs: dict[str, dict[str, Any]] = {}
    for receipt_id in DEPENDENCY_PATHS:
        documents[receipt_id], blobs[receipt_id] = _canonical_receipt(
            repo_root, receipt_id, live_head
        )

    a2t_digest = _digest(documents[A2T_ID])
    policy_digest = _digest(documents[PRODUCT_POLICY_ID])
    runtime = documents[A3_ID]
    a2t_payload = documents[A2T_ID].get("payload")
    if (
        not isinstance(a2t_payload, dict)
        or set(a2t_payload) != {
            "trace_analyzer_sha256", "trace_format", "trace_questions"
        }
        or not evidence._is_lower_hex(a2t_payload.get("trace_analyzer_sha256"), 64)
        or a2t_payload.get("trace_format") != "perfetto-or-chrome-trace-v1"
        or a2t_payload.get("trace_questions")
        != ["gpu-health", "gpu-probe", "gpu-startup"]
    ):
        raise TerminalReceiptError("terminal A2T lacks exact trace-analyzer authority")
    dependency_map = {
        item["id"]: item["sha256"] for item in runtime["dependencies"]
    }
    if dependency_map.get(A2T_ID) != a2t_digest:
        raise TerminalReceiptError("A3 runtime receipt does not bind exact terminal A2T")
    payload = runtime["payload"]
    if (
        set(payload) != {
            "a2t_receipt_sha256", "product_policy_receipt_sha256", "product_policy"
        }
        or payload.get("a2t_receipt_sha256") != a2t_digest
        or payload.get("product_policy_receipt_sha256") != policy_digest
        or payload.get("product_policy") != documents[PRODUCT_POLICY_ID].get("payload")
    ):
        raise TerminalReceiptError("A3 runtime payload does not bind exact A2T/product authority")
    if documents[PRODUCT_POLICY_ID]["disposition"] != "pass":
        raise TerminalReceiptError("A3 product-policy authority is not terminal pass")
    installed = documents[A2T_ID]["installed_revisions"]
    if any(document["installed_revisions"] != installed for document in documents.values()):
        raise TerminalReceiptError("A2T/A3 terminal receipts bind different installed revisions")
    if subprocess.run(
        ["git", "merge-base", "--is-ancestor", installed["pulp"], live_head],
        cwd=repo_root, capture_output=True, timeout=30,
    ).returncode != 0:
        raise TerminalReceiptError(
            "A2T/A3 installed Pulp revision is not on protected-main history"
        )
    policy = documents[PRODUCT_POLICY_ID]["payload"]
    verify_remote_policy(policy)
    return documents, blobs


def derive_manifest(
    blocked_manifest: dict[str, Any], dependency_documents: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    """Derive the closed authorized corpus; no caller manifest fields are accepted."""
    policy_receipt = dependency_documents[PRODUCT_POLICY_ID]
    policy = policy_receipt["payload"]
    required = {
        "id", "repository", "revision", "path", "blob", "sha256",
        "configured_max_dpr", "adaptive_profile", "timer_noise_p95_ns",
        "memory_sampler_resolution_bytes", "frame_budgets_ns",
    }
    if not isinstance(policy, dict) or set(policy) != required:
        raise TerminalReceiptError("A3 product-policy payload differs from the closed A4 authority")
    manifest = copy.deepcopy(blocked_manifest)
    protocol = manifest["v2_protocol"]
    protocol["status"] = "authorized"
    protocol["product_policy"] = {
        "id": policy["id"],
        "path": policy["path"],
        "blob": policy["blob"],
        "a3_receipt_sha256": _digest(policy_receipt),
    }
    configured = policy["configured_max_dpr"]
    if configured not in {1, 1.5, 2, 3}:
        raise TerminalReceiptError("A3 configured-max value is outside the frozen DPR rungs")
    manifest["configured_max_dpr"] = configured
    adaptive = policy["adaptive_profile"]
    if not isinstance(adaptive, dict) or not isinstance(adaptive.get("id"), str):
        raise TerminalReceiptError("A3 adaptive profile is malformed")
    manifest["adaptive_profile"] = copy.deepcopy(adaptive)
    manifest["trial_contract"]["timer_noise_p95_ns"] = _positive_integer(
        policy["timer_noise_p95_ns"], "timer-noise calibration"
    )
    manifest["trial_contract"]["memory_sampler_resolution_bytes"] = _positive_integer(
        policy["memory_sampler_resolution_bytes"], "memory-sampler resolution"
    )
    budgets = policy["frame_budgets_ns"]
    scenario_ids = {scenario["id"] for scenario in manifest["scenarios"]}
    if not isinstance(budgets, dict) or set(budgets) != scenario_ids:
        raise TerminalReceiptError("A3 frame budgets do not exactly cover the seven-role corpus")
    for scenario in manifest["scenarios"]:
        scenario["frame_budget_ns"] = _positive_integer(
            budgets[scenario["id"]], f"frame budget for {scenario['id']}"
        )
    return manifest


def dependency_projection(
    documents: dict[str, dict[str, Any]], blobs: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    return {
        receipt_id: {
            "sha256": _digest(documents[receipt_id]),
            "blob": blobs[receipt_id]["blob"],
            "path": blobs[receipt_id]["path"],
        }
        for receipt_id in sorted(documents)
    }
