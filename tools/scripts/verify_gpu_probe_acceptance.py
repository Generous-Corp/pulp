#!/usr/bin/env python3
"""Verify a durable A2 GPU-probe acceptance receipt."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any

import json_schema_lite


SHA256 = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA = re.compile(r"^[0-9a-f]{40}$")
GROUPS = {
    "compute": "gpu-compute.magnitude.v1",
    "stft": "gpu-audio.stft.v1",
    "renderer": "renderer3d.hardcoded-cube.v1",
    "threejs": "threejs.multi-pass.v1",
}
HARDWARE_REQUIRED = {"compute", "stft", "threejs"}
EXPECTED_SAMPLE_COUNTS = {
    "compute": 256,
    "stft": 1024,
    "renderer": 0,
    "threejs": 5,
}
EXPECTED_BINARY_ROLES = {
    "compute": "installed_rust_cli",
    "stft": "installed_rust_cli",
    "renderer": "scene3d_cpp_cli",
    "threejs": "v8_threejs_cpp_cli",
}
RESULT_SCHEMA = (
    Path(__file__).resolve().parents[2]
    / "docs/contracts/gpu-probe-result-v1.schema.json"
)


def _load(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _canonical_repeat(result: dict[str, Any]) -> dict[str, Any]:
    value = copy.deepcopy(result)
    value.pop("gpu_evidence_id", None)
    return value


def verify(root: Path) -> list[str]:
    errors: list[str] = []
    try:
        result_schema = _load(RESULT_SCHEMA)
    except (OSError, json.JSONDecodeError) as error:
        return [f"cannot read GPU probe result schema: {error}"]
    receipt_path = root / "receipt.json"
    try:
        receipt = _load(receipt_path)
    except (OSError, json.JSONDecodeError) as error:
        return [f"cannot read receipt.json: {error}"]

    if receipt.get("schema") != "pulp.gpu-probe-acceptance-receipt.v1":
        errors.append("receipt schema mismatch")
    if not GIT_SHA.fullmatch(str(receipt.get("integration_head", ""))):
        errors.append("integration_head must be an exact Git SHA")
    for role, commit in receipt.get("source_heads", {}).items():
        if not GIT_SHA.fullmatch(str(commit)):
            errors.append(f"source head {role} must be an exact Git SHA")
    context = receipt.get("execution_context", {})
    if context.get("cwd_role") != "fresh-temporary-directory-outside-any-checkout":
        errors.append("installed fronts were not recorded outside every checkout")
    if context.get("path") != "/usr/bin:/bin:/usr/sbin:/sbin":
        errors.append("installed fronts did not use the bounded system-only PATH")
    for role, digest in receipt.get("binaries", {}).items():
        if not SHA256.fullmatch(str(digest)):
            errors.append(f"binary {role} lacks a SHA-256 digest")

    declared = receipt.get("raw_sha256", {})
    expected_files = {
        *(f"{group}-{suffix}.json" for group in GROUPS for suffix in ("run1", "run2", "negative")),
        "mcp-transcript.jsonl",
    }
    if set(declared) != expected_files:
        errors.append("raw_sha256 does not name the exact 13-file evidence set")
    for name, digest in declared.items():
        path = root / name
        try:
            observed = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError as error:
            errors.append(f"cannot read {name}: {error}")
            continue
        if observed != digest:
            errors.append(f"raw digest mismatch for {name}")

    run_groups = receipt.get("run_groups", {})
    if set(run_groups) != set(GROUPS):
        errors.append("receipt does not bind the exact four run groups")
    for group, recipe in GROUPS.items():
        if run_groups.get(group, {}).get("recipe") != recipe:
            errors.append(f"receipt run group {group} does not bind recipe {recipe}")
        role = run_groups.get(group, {}).get("binary_role")
        if role != EXPECTED_BINARY_ROLES[group] or role not in receipt.get("binaries", {}):
            errors.append(f"receipt run group {group} has the wrong executable role")
        runs: dict[str, dict[str, Any]] = {}
        for suffix in ("run1", "run2", "negative"):
            path = root / f"{group}-{suffix}.json"
            try:
                result = _load(path)
            except (OSError, json.JSONDecodeError) as error:
                errors.append(f"cannot parse {path.name}: {error}")
                continue
            runs[suffix] = result
            for problem in json_schema_lite.validate(result, result_schema):
                errors.append(f"{path.name}: schema: {problem}")
            if result.get("recipe_id") != recipe:
                errors.append(f"{path.name}: recipe mismatch")
            if not result.get("passes") or not all(p.get("work_completed") for p in result["passes"]):
                errors.append(f"{path.name}: work was not proven complete")
            artifacts = result.get("artifacts", [])
            if not artifacts or sum(int(a.get("bytes", 0)) for a in artifacts) > 512 * 1024:
                errors.append(f"{path.name}: artifacts are absent or exceed 512 KiB")
            for artifact in artifacts:
                if not SHA256.fullmatch(str(artifact.get("sha256", ""))):
                    errors.append(f"{path.name}: artifact lacks SHA-256")
            if result.get("numeric_sample_count") != EXPECTED_SAMPLE_COUNTS[group]:
                errors.append(f"{path.name}: numeric sample count changed")
            adapter = result.get("adapter", {})
            if group in HARDWARE_REQUIRED and (
                adapter.get("status") != "authentic"
                or adapter.get("class") != "hardware"
                or adapter.get("backend") != "Metal"
            ):
                errors.append(f"{path.name}: hardware-required adapter claim is not authentic Metal")
            if group == "renderer" and (
                adapter.get("status") != "unverified"
                or adapter.get("class") != "unknown"
                or "Metal" not in str(adapter.get("name", ""))
            ):
                errors.append(f"{path.name}: renderer adapter truthfulness changed")

        if set(runs) != {"run1", "run2", "negative"}:
            continue
        if runs["run1"].get("verdict") != "pass" or runs["run2"].get("verdict") != "pass":
            errors.append(f"{group}: both positive runs must pass")
        for label in ("run1", "run2"):
            if not all(p.get("verdict") == "pass" for p in runs[label].get("passes", [])):
                errors.append(f"{group}: {label} contains a non-passing semantic pass")
        if _canonical_repeat(runs["run1"]) != _canonical_repeat(runs["run2"]):
            errors.append(f"{group}: positive rerun is not deterministic")
        negative = runs["negative"]
        if negative.get("verdict") != "fail" or not negative.get("mutation"):
            errors.append(f"{group}: seeded negative control did not fail")
        if not any(p.get("verdict") == "fail" and p.get("work_completed") for p in negative.get("passes", [])):
            errors.append(f"{group}: negative control lacks a completed causal failure")

    try:
        transcript = [json.loads(line) for line in (root / "mcp-transcript.jsonl").read_text().splitlines()]
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f"cannot parse MCP transcript: {error}")
        transcript = []
    if len(transcript) != 3 or [row.get("id") for row in transcript] != [1, 2, 3]:
        errors.append("MCP transcript must contain initialize, positive, and negative responses")
    elif transcript[1].get("result", {}).get("structuredContent", {}).get("exit_code") != 0:
        errors.append("MCP positive result did not preserve exit 0")
    else:
        mcp_evidence: dict[str, Any] = {}
        for label, row in (("positive", transcript[1]), ("negative", transcript[2])):
            evidence = row.get("result", {}).get("structuredContent", {}).get("evidence")
            for problem in json_schema_lite.validate(evidence, result_schema):
                errors.append(f"MCP {label} evidence schema: {problem}")
            if isinstance(evidence, dict):
                mcp_evidence[label] = evidence
            content = row.get("result", {}).get("content", [])
            try:
                text_evidence = json.loads(content[0]["text"])
            except (IndexError, KeyError, TypeError, json.JSONDecodeError):
                errors.append(f"MCP {label} text evidence is missing or malformed")
            else:
                if text_evidence != evidence:
                    errors.append(f"MCP {label} text and structured evidence disagree")
        for label, raw_name in (("positive", "compute-run1.json"), ("negative", "compute-negative.json")):
            if label not in mcp_evidence:
                continue
            raw = _load(root / raw_name)
            if _canonical_repeat(mcp_evidence[label]) != _canonical_repeat(raw):
                errors.append(f"MCP {label} evidence is not the installed CLI recipe result")
        negative_result = transcript[2].get("result", {})
        if negative_result.get("isError") is not True:
            errors.append("MCP completed failure did not preserve isError=true")
        if negative_result.get("structuredContent", {}).get("exit_code") != 1:
            errors.append("MCP completed failure did not preserve exit 1")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt_dir", type=Path)
    args = parser.parse_args(argv)
    errors = verify(args.receipt_dir)
    if errors:
        for error in errors:
            print(f"gpu-probe-acceptance: FAIL: {error}", file=sys.stderr)
        return 1
    print("gpu-probe-acceptance: ok (4 recipes, 12 runs, installed MCP pass/fail)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
