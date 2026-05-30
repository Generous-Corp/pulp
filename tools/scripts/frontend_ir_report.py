#!/usr/bin/env python3
"""Build a PulpFrontendIR v0 report from existing import validation artifacts."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any

from frontend_ir_common import load_json, write_json
from frontend_ir_nodes import nodes_from_rows
from frontend_ir_proofs import apply_native_proofs, load_native_proof
from frontend_ir_resources import (
    artifact_ref_from_manifest,
    binary_dependency_evidence,
    resource_counts,
    resources_from_manifest,
)
from frontend_ir_routes import (
    inline_source_audit,
    normalize_source_of_truth,
    primitive_counts,
    route_counts,
    route_name,
    route_rows,
    routes_from_rows,
    routes_present,
    row_node_id,
    semantic_role,
    source_kind,
    support_for_route,
)
from frontend_ir_sources import (
    count_map,
    dynamic_risks,
    metric_key,
    source_input,
    source_span,
    source_spans,
)
from frontend_ir_state import state_counts, state_for_row
from frontend_ir_styles import iter_style_values, style_counts, style_for_row
from frontend_ir_tokens import (
    resolve_source_token_refs,
    token_counts,
    token_key,
    tokens_from_rows,
)
from frontend_ir_tweaks import tweak_counts, tweaks_from_sidecar
from frontend_ir_validation import (
    FALLBACK_ROUTES,
    NATIVE_ROUTES,
    PLANNED_SUPPORT_ROUTES,
    ROUTE_HYBRID,
    ROUTE_LIVE_JS,
    ROUTE_NATIVE_CPP,
    ROUTE_NATIVE_HTML,
    ROUTE_RECORDED_PAINT,
    ROUTE_UNSUPPORTED,
    SCHEMAS,
    SOURCE_TRUTHS,
    is_finite_number,
    is_non_negative_int,
    is_positive_int,
    validate_count_map,
    validate_frontend_ir,
)


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def build_frontend_ir(
    route_manifest: dict[str, Any],
    source_audit: dict[str, Any],
    route_manifest_path: pathlib.Path,
    repo_root: pathlib.Path,
    tweaks: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    overlay = route_manifest.get("source_contract_overlay", {})
    if not isinstance(overlay, dict):
        overlay = {}
    rows = route_rows(route_manifest)
    if not source_audit:
        source_audit = inline_source_audit(route_manifest)

    source_artifact = source_input(route_manifest)
    source_path = source_artifact.get("path") or source_audit.get("input") or ""
    if not isinstance(source_path, str):
        source_path = ""

    counts = count_map(source_audit, rows)
    nodes = nodes_from_rows(rows)
    resources = resources_from_manifest(route_manifest, routes_present(rows), repo_root)
    tokens = tokens_from_rows(rows)
    resolved_tokens = resolve_source_token_refs(tokens, source_path, repo_root)
    tweaks = tweaks or []
    source_of_truth = overlay.get("source", {}).get("source_of_truth")
    source_of_truth = normalize_source_of_truth(source_of_truth)

    design_ref = artifact_ref_from_manifest(route_manifest, "ir", "design_ir") or {
        "path": "",
        "kind": "design_ir",
    }
    design_ref.setdefault("schema", "pulp-design-ir-v1")
    notes = [
        "frontend-ir v0 wraps existing import evidence; compile and binary audits must be supplied by route validators before production gating."
    ]
    if not design_ref.get("path"):
        notes.append("route manifest did not provide a DesignIR artifact; this report covers source and route evidence only.")
    if resolved_tokens:
        notes.append(f"resolved {resolved_tokens} token references from static source token objects.")
    if tweaks:
        notes.append(f"attached {len(tweaks)} tweak sidecar edits without mutating imported source.")

    report: dict[str, Any] = {
        "schema": SCHEMAS["frontend_ir"],
        "fixture_id": str(route_manifest.get("fixture") or overlay.get("fixture_id") or ""),
        "source": {
            "kind": source_kind(source_path),
            "path": source_path,
            "source_of_truth": source_of_truth,
            "counts": counts,
            "dynamic_risks": dynamic_risks(counts),
            "spans": source_spans(source_audit, rows, source_path),
        },
        "design_ir": design_ref,
        "route_manifest": {
            "path": repo_relative(route_manifest_path, repo_root),
            "schema": str(route_manifest.get("schema", "")),
            "kind": "route_manifest",
        },
        "nodes": nodes,
        "resources": resources,
        "tokens": tokens,
        "tweaks": tweaks,
        "routes": routes_from_rows(rows),
        "host": {
            "dpi_policy": "responsive",
            "input": ["pointer", "keyboard", "focus", "text_input"],
            "state_bridge": ["parameters", "gestures", "meters"],
            "surface": {
                "required_backend": "any",
            },
        },
        "validation": {
            "source_counts": counts,
            "style_counts": style_counts(nodes, counts),
            "state_counts": state_counts(nodes),
            "route_counts": route_counts(route_manifest, rows),
            "primitive_counts": primitive_counts(rows),
            "resource_counts": resource_counts(resources),
            "token_counts": token_counts(tokens, rows),
            "tweak_counts": tweak_counts(tweaks),
            "compile": {
                "status": "not_run",
            },
            "binary_dependencies": binary_dependency_evidence(route_manifest),
            "screenshots": [],
            "notes": notes,
        },
    }
    sha = source_artifact.get("sha256")
    if isinstance(sha, str) and sha:
        report["source"]["sha256"] = sha
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--route-manifest", required=True, type=pathlib.Path)
    parser.add_argument("--source-audit", type=pathlib.Path)
    parser.add_argument("--native-proof", action="append", type=pathlib.Path, default=[],
                        help="native compile/linkage proof artifact to attach to validation evidence")
    parser.add_argument("--tweaks", type=pathlib.Path,
                        help="pulp-tweaks-v0 sidecar to attach as non-source-mutating retheme evidence")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)

    route_manifest = load_json(args.route_manifest)
    source_audit = load_json(args.source_audit) if args.source_audit else {}
    rows = route_rows(route_manifest)
    valid_node_ids = {
        row_node_id(row, index)
        for index, row in enumerate(rows)
        if isinstance(row, dict)
    }
    tweaks = tweaks_from_sidecar(args.tweaks, valid_node_ids) if args.tweaks else []
    report = build_frontend_ir(route_manifest, source_audit, args.route_manifest, args.repo_root, tweaks)
    if args.native_proof:
        apply_native_proofs(report, [load_native_proof(path) for path in args.native_proof], args.repo_root)
    validate_frontend_ir(report)
    write_json(args.output, report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
