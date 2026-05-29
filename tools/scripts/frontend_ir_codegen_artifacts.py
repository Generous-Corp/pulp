#!/usr/bin/env python3
"""Summarize generated C++ artifact coverage against a PulpFrontendIR report."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
from typing import Any
from frontend_ir_common import as_list, load_json, write_json
from frontend_ir_validation import SCHEMAS


SCHEMA = SCHEMAS["codegen_artifacts"]
BINDING_SCHEMA = SCHEMAS["native_cpp_binding_manifest"]
NATIVE_CPP_ROUTE = "native_cpp"


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_ref(path: pathlib.Path, repo_root: pathlib.Path, kind: str, schema: str = "") -> dict[str, Any]:
    ref: dict[str, Any] = {
        "kind": kind,
        "path": repo_relative(path, repo_root),
        "sha256": file_sha256(path),
        "byte_size": path.stat().st_size,
    }
    if schema:
        ref["schema"] = schema
    return ref


def native_cpp_route_ids(report: dict[str, Any]) -> set[str]:
    routes: set[str] = set()
    for route in as_list(report.get("routes")):
        if not isinstance(route, dict) or route.get("chosen_route") != NATIVE_CPP_ROUTE:
            continue
        node_id = route.get("node_id")
        if isinstance(node_id, str) and node_id:
            routes.add(node_id)
    return routes


def binding_entries(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    if manifest.get("schema") != BINDING_SCHEMA:
        raise ValueError(f"binding manifest schema must be {BINDING_SCHEMA}")
    entries = manifest.get("entries")
    if not isinstance(entries, list):
        raise ValueError("binding manifest entries must be an array")
    return [entry for entry in entries if isinstance(entry, dict)]


def binding_ids(entries: list[dict[str, Any]]) -> set[str]:
    ids: set[str] = set()
    for entry in entries:
        entry_id = entry.get("id")
        if isinstance(entry_id, str) and entry_id:
            ids.add(entry_id)
    return ids


def count_by(entries: list[dict[str, Any]], key: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    for entry in entries:
        value = entry.get(key)
        if isinstance(value, str) and value:
            counts[value] = counts.get(value, 0) + 1
    return dict(sorted(counts.items()))


def split_candidates(missing_routes: set[str], entries: set[str]) -> list[dict[str, Any]]:
    candidates = []
    for route_id in sorted(missing_routes):
        prefix = route_id + "."
        split = sorted(entry_id for entry_id in entries if entry_id.startswith(prefix))
        if split:
            candidates.append({
                "route_id": route_id,
                "binding_entry_ids": split,
            })
    return candidates


def build_codegen_artifact_report(
    frontend_ir: dict[str, Any],
    binding_manifest: dict[str, Any],
    *,
    frontend_ir_path: pathlib.Path,
    source_cpp: pathlib.Path,
    header: pathlib.Path,
    binding_manifest_path: pathlib.Path,
    repo_root: pathlib.Path,
) -> dict[str, Any]:
    entries = binding_entries(binding_manifest)
    routes = native_cpp_route_ids(frontend_ir)
    entry_ids = binding_ids(entries)
    direct = routes & entry_ids
    missing = routes - entry_ids
    extra = entry_ids - routes
    # Only count "extra" entries (sub-bindings not claimed by their own native
    # route) as split candidates. Searching all entry_ids would let an entry
    # that directly binds a *different* route (e.g. "foo.label") account for an
    # unrelated unbound route "foo" purely on a string-prefix collision,
    # downgrading a real binding hole from FAIL to WARN.
    splits = split_candidates(missing, extra)

    return {
        "schema": SCHEMA,
        "fixture_id": str(frontend_ir.get("fixture_id", "")),
        "frontend_ir": artifact_ref(frontend_ir_path, repo_root, "frontend_ir", SCHEMAS["frontend_ir"]),
        "artifacts": {
            "source_cpp": artifact_ref(source_cpp, repo_root, "generated_cpp_source"),
            "header": artifact_ref(header, repo_root, "generated_cpp_header"),
            "binding_manifest": artifact_ref(
                binding_manifest_path,
                repo_root,
                "generated_cpp_binding_manifest",
                BINDING_SCHEMA,
            ),
        },
        "summary": {
            "native_cpp_routes": len(routes),
            "binding_entries": len(entries),
            "directly_bound_native_routes": len(direct),
            "missing_native_route_bindings": len(missing),
            "extra_binding_entries": len(extra),
            "split_binding_candidates": len(splits),
            "binding_primitives": count_by(entries, "native_primitive"),
            "binding_route_types": count_by(entries, "route_type"),
        },
        "missing_native_route_bindings": sorted(missing),
        "extra_binding_entries": sorted(extra),
        "split_binding_candidates": splits,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frontend-ir", required=True, type=pathlib.Path)
    parser.add_argument("--source-cpp", required=True, type=pathlib.Path)
    parser.add_argument("--header", required=True, type=pathlib.Path)
    parser.add_argument("--binding-manifest", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)

    report = build_codegen_artifact_report(
        load_json(args.frontend_ir),
        load_json(args.binding_manifest),
        frontend_ir_path=args.frontend_ir,
        source_cpp=args.source_cpp,
        header=args.header,
        binding_manifest_path=args.binding_manifest,
        repo_root=args.repo_root,
    )
    write_json(args.output, report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
