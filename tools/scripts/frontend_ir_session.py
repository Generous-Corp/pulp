#!/usr/bin/env python3
"""Compare two PulpFrontendIR reports and classify reload scope."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any
from frontend_ir_common import load_json, write_json
from frontend_ir_validation import SCHEMAS


CHANGE_SCOPES = (
    "source_contract",
    "style_contract",
    "resources",
    "routes",
    "tokens",
    "tweaks",
    "validation",
)


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def stable(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def is_style_resource(resource: dict[str, Any]) -> bool:
    mime = resource.get("mime")
    if mime == "text/css":
        return True
    identity = " ".join(
        str(resource.get(key, ""))
        for key in ("id", "original_uri", "resolved_uri")
    ).lower()
    return ".css" in identity or "style" in identity


def map_by_id(items: Any) -> dict[str, dict[str, Any]]:
    if not isinstance(items, list):
        return {}
    mapped: dict[str, dict[str, Any]] = {}
    for item in items:
        if not isinstance(item, dict):
            continue
        item_id = item.get("id") or item.get("node_id")
        if isinstance(item_id, str) and item_id:
            mapped[item_id] = item
    return mapped


def resource_map(report: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return map_by_id(report.get("resources", []))


def route_map(report: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return map_by_id(report.get("routes", []))


def node_classification(report: dict[str, Any]) -> list[dict[str, str]]:
    nodes = report.get("nodes", [])
    if not isinstance(nodes, list):
        return []
    result = []
    for node in nodes:
        if not isinstance(node, dict):
            continue
        node_id = node.get("id")
        role = node.get("semantic_role")
        if isinstance(node_id, str) and isinstance(role, str):
            result.append({"id": node_id, "semantic_role": role})
    return sorted(result, key=lambda item: item["id"])


def source_signature(report: dict[str, Any]) -> dict[str, Any]:
    source = report.get("source", {})
    if not isinstance(source, dict):
        source = {}
    return {
        "kind": source.get("kind", ""),
        "path": source.get("path", ""),
        "sha256": source.get("sha256", ""),
        "source_of_truth": source.get("source_of_truth", ""),
        "counts": source.get("counts", {}),
        "nodes": node_classification(report),
    }


def style_signature(report: dict[str, Any]) -> dict[str, Any]:
    validation = report.get("validation", {})
    if not isinstance(validation, dict):
        validation = {}
    nodes = report.get("nodes", [])
    styles = []
    if isinstance(nodes, list):
        for node in nodes:
            if not isinstance(node, dict):
                continue
            styles.append({
                "id": node.get("id", ""),
                "style": node.get("style", {}),
            })
    return {
        "style_counts": validation.get("style_counts", {}),
        "node_styles": sorted(styles, key=lambda item: str(item.get("id", ""))),
    }


def route_signature(report: dict[str, Any]) -> dict[str, Any]:
    routes = []
    for route in route_map(report).values():
        routes.append({
            "node_id": route.get("node_id", ""),
            "semantic_role": route.get("semantic_role", ""),
            "chosen_route": route.get("chosen_route", ""),
            "requires_js_engine": route.get("requires_js_engine"),
            "fallback_reason": route.get("fallback_reason", ""),
        })
    validation = report.get("validation", {})
    if not isinstance(validation, dict):
        validation = {}
    return {
        "routes": sorted(routes, key=lambda item: str(item.get("node_id", ""))),
        "route_counts": validation.get("route_counts", {}),
        "primitive_counts": validation.get("primitive_counts", {}),
    }


def validation_signature(report: dict[str, Any]) -> dict[str, Any]:
    validation = report.get("validation", {})
    if not isinstance(validation, dict):
        return {}
    return {
        key: validation.get(key)
        for key in ("compile", "binary_dependencies", "screenshots", "proofs")
        if key in validation
    }


def tweaks_preserve_classification(report: dict[str, Any]) -> bool:
    tweaks = report.get("tweaks", [])
    if not isinstance(tweaks, list):
        return True
    for tweak in tweaks:
        if not isinstance(tweak, dict):
            continue
        if tweak.get("classification_preserved") is False:
            return False
        invalidates = tweak.get("invalidates", [])
        if isinstance(invalidates, list) and any(scope in {"source", "route"} for scope in invalidates):
            return False
    return True


def changed_fields(before: dict[str, Any], after: dict[str, Any]) -> list[str]:
    keys = sorted(set(before) | set(after))
    return [key for key in keys if stable(before.get(key)) != stable(after.get(key))]


def diff_map(before: dict[str, dict[str, Any]], after: dict[str, dict[str, Any]]) -> dict[str, Any]:
    before_keys = set(before)
    after_keys = set(after)
    changed = []
    for key in sorted(before_keys & after_keys):
        fields = changed_fields(before[key], after[key])
        if fields:
            changed.append({"id": key, "fields": fields})
    return {
        "added": sorted(after_keys - before_keys),
        "removed": sorted(before_keys - after_keys),
        "changed": changed,
    }


def map_changed(delta: dict[str, Any]) -> bool:
    return bool(delta.get("added") or delta.get("removed") or delta.get("changed"))


def changed_resource_ids(delta: dict[str, Any]) -> list[str]:
    ids = list(delta.get("added", [])) + list(delta.get("removed", []))
    ids.extend(item["id"] for item in delta.get("changed", []) if isinstance(item, dict) and item.get("id"))
    return sorted(set(ids))


def style_resource_changed(before_resources: dict[str, dict[str, Any]],
                           after_resources: dict[str, dict[str, Any]],
                           resource_delta: dict[str, Any]) -> bool:
    for resource_id in changed_resource_ids(resource_delta):
        before = before_resources.get(resource_id)
        after = after_resources.get(resource_id)
        if (isinstance(before, dict) and is_style_resource(before)) or (
            isinstance(after, dict) and is_style_resource(after)
        ):
            return True
    return False


def recommended_reload(scope: list[str], classification_preserved: bool) -> str:
    if not scope:
        return "none"
    if "source_contract" in scope or "routes" in scope or not classification_preserved:
        return "full_reimport"
    if scope == ["resources"]:
        return "resource_reload"
    # A validation-only delta (proofs/screenshots refreshed, no source/style/
    # resource/token/tweak change) only needs a validation refresh. This must
    # be checked before the token/tweak branch below, which would otherwise
    # swallow {"validation"} and make validation_refresh unreachable.
    if set(scope) == {"validation"}:
        return "validation_refresh"
    if all(item in {"tokens", "tweaks", "validation"} for item in scope):
        return "token_tweak_reload"
    if any(item in {"style_contract", "resources", "tokens", "tweaks"} for item in scope):
        return "style_resource_reload"
    return "validation_refresh"


def compare_reports(before: dict[str, Any], after: dict[str, Any],
                    before_path: pathlib.Path | None = None,
                    after_path: pathlib.Path | None = None,
                    repo_root: pathlib.Path | None = None) -> dict[str, Any]:
    before_resources = resource_map(before)
    after_resources = resource_map(after)
    resource_delta = diff_map(before_resources, after_resources)
    route_delta = diff_map(route_map(before), route_map(after))

    source_changed = stable(source_signature(before)) != stable(source_signature(after))
    style_changed = (
        stable(style_signature(before)) != stable(style_signature(after)) or
        style_resource_changed(before_resources, after_resources, resource_delta)
    )
    resources_changed = map_changed(resource_delta)
    routes_changed = map_changed(route_delta)
    tokens_changed = stable(before.get("tokens", {})) != stable(after.get("tokens", {}))
    tweaks_changed = stable(before.get("tweaks", [])) != stable(after.get("tweaks", []))
    validation_changed = stable(validation_signature(before)) != stable(validation_signature(after))
    classification_preserved = (
        stable(node_classification(before)) == stable(node_classification(after)) and
        stable(route_signature(before)) == stable(route_signature(after)) and
        tweaks_preserve_classification(before) and
        tweaks_preserve_classification(after)
    )

    scope = []
    for name, changed in (
        ("source_contract", source_changed),
        ("style_contract", style_changed),
        ("resources", resources_changed),
        ("routes", routes_changed),
        ("tokens", tokens_changed),
        ("tweaks", tweaks_changed),
        ("validation", validation_changed),
    ):
        if changed:
            scope.append(name)

    root = repo_root or pathlib.Path.cwd()
    report: dict[str, Any] = {
        "schema": SCHEMAS["session_diff"],
        "fixture_id": str(after.get("fixture_id") or before.get("fixture_id") or ""),
        "before": {
            "fixture_id": str(before.get("fixture_id", "")),
            "path": repo_relative(before_path, root) if before_path else "",
        },
        "after": {
            "fixture_id": str(after.get("fixture_id", "")),
            "path": repo_relative(after_path, root) if after_path else "",
        },
        "summary": {
            "reload_scope": scope,
            "recommended_reload": recommended_reload(scope, classification_preserved),
            "component_classification_preserved": classification_preserved,
            "narrow_reload_safe": (
                classification_preserved and
                "source_contract" not in scope and
                "routes" not in scope
            ),
        },
        "changes": {
            "source_contract": {
                "changed": source_changed,
                "before_sha256": before.get("source", {}).get("sha256", ""),
                "after_sha256": after.get("source", {}).get("sha256", ""),
            },
            "style_contract": {
                "changed": style_changed,
                "style_resource_changed": style_resource_changed(before_resources, after_resources, resource_delta),
            },
            "resources": resource_delta,
            "routes": route_delta,
            "tokens": {
                "changed": tokens_changed,
                "before_count": len(before.get("tokens", {}) if isinstance(before.get("tokens"), dict) else {}),
                "after_count": len(after.get("tokens", {}) if isinstance(after.get("tokens"), dict) else {}),
            },
            "tweaks": {
                "changed": tweaks_changed,
                "before_count": len(before.get("tweaks", []) if isinstance(before.get("tweaks"), list) else []),
                "after_count": len(after.get("tweaks", []) if isinstance(after.get("tweaks"), list) else []),
                "classification_preserved": (
                    tweaks_preserve_classification(before) and tweaks_preserve_classification(after)
                ),
            },
            "validation": {
                "changed": validation_changed,
            },
        },
    }
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=pathlib.Path)
    parser.add_argument("--after", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)

    report = compare_reports(
        load_json(args.before),
        load_json(args.after),
        args.before,
        args.after,
        args.repo_root,
    )
    write_json(args.output, report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
