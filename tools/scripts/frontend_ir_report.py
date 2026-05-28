#!/usr/bin/env python3
"""Build a PulpFrontendIR v0 report from existing import validation artifacts."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
from typing import Any


ROUTE_NATIVE_CPP = "native_cpp"
ROUTE_LIVE_JS = "live_js"
ROUTE_HYBRID = "hybrid"
ROUTE_UNSUPPORTED = "unsupported"
ROUTES = {ROUTE_LIVE_JS, "native_html", ROUTE_NATIVE_CPP, "recorded_paint", ROUTE_HYBRID, ROUTE_UNSUPPORTED}
NATIVE_ROUTES = {"native_html", ROUTE_NATIVE_CPP, "recorded_paint"}
SHA256_RE = re.compile(r"^[a-f0-9]{64}$")


def load_json(path: pathlib.Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return data


def write_json(path: pathlib.Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_sha(value: Any, field: str) -> None:
    if value is None:
        return
    expect(isinstance(value, str) and bool(SHA256_RE.fullmatch(value)), f"{field} must be a lowercase sha256")


def validate_count_map(value: Any, field: str) -> None:
    expect(isinstance(value, dict), f"{field} must be an object")
    for key, count in value.items():
        expect(isinstance(key, str) and bool(key), f"{field} keys must be non-empty strings")
        expect(isinstance(count, int) and count >= 0, f"{field}.{key} must be a non-negative integer")


def validate_artifact_ref(value: Any, field: str) -> None:
    expect(isinstance(value, dict), f"{field} must be an object")
    expect(isinstance(value.get("path"), str), f"{field}.path must be a string")
    validate_sha(value.get("sha256"), f"{field}.sha256")


def validate_frontend_ir(report: dict[str, Any]) -> None:
    expect(report.get("schema") == "pulp-frontend-ir-v0", "schema must be pulp-frontend-ir-v0")
    for key in ("source", "design_ir", "nodes", "routes", "validation"):
        expect(key in report, f"missing required field: {key}")

    source = report["source"]
    expect(isinstance(source, dict), "source must be an object")
    expect(source.get("kind") in {"jsx", "html", "design_json", "runtime_snapshot", "pulp_js"}, "source.kind is invalid")
    expect(isinstance(source.get("path"), str), "source.path must be a string")
    expect(source.get("source_of_truth") in {"archived_fixture", "local_file", "mcp_payload", "generated", "runtime_capture"},
           "source.source_of_truth is invalid")
    validate_sha(source.get("sha256"), "source.sha256")
    validate_count_map(source.get("counts"), "source.counts")

    validate_artifact_ref(report["design_ir"], "design_ir")
    if "route_manifest" in report:
        validate_artifact_ref(report["route_manifest"], "route_manifest")

    expect(isinstance(report["nodes"], list), "nodes must be an array")
    for index, node in enumerate(report["nodes"]):
        expect(isinstance(node, dict), f"nodes[{index}] must be an object")
        expect(isinstance(node.get("id"), str) and bool(node["id"]), f"nodes[{index}].id is required")
        expect(isinstance(node.get("semantic_role"), str) and bool(node["semantic_role"]),
               f"nodes[{index}].semantic_role is required")
        expect(isinstance(node.get("style"), dict), f"nodes[{index}].style must be an object")
        expect(isinstance(node.get("state"), dict), f"nodes[{index}].state must be an object")

    expect(isinstance(report["routes"], list), "routes must be an array")
    for index, route in enumerate(report["routes"]):
        expect(isinstance(route, dict), f"routes[{index}] must be an object")
        chosen = route.get("chosen_route")
        expect(chosen in ROUTES, f"routes[{index}].chosen_route is invalid")
        expect(isinstance(route.get("node_id"), str) and bool(route["node_id"]), f"routes[{index}].node_id is required")
        expect(isinstance(route.get("reason"), str) and bool(route["reason"]), f"routes[{index}].reason is required")
        if chosen in NATIVE_ROUTES:
            expect(route.get("requires_js_engine") is False,
                   f"routes[{index}] native route must set requires_js_engine=false")
            expect(isinstance(route.get("validation_refs"), list) and bool(route["validation_refs"]),
                   f"routes[{index}] native route must include validation_refs")
        if chosen in {ROUTE_LIVE_JS, ROUTE_HYBRID, ROUTE_UNSUPPORTED}:
            expect(isinstance(route.get("fallback_reason"), str) and bool(route["fallback_reason"]),
                   f"routes[{index}] fallback route must include fallback_reason")

    validation = report["validation"]
    expect(isinstance(validation, dict), "validation must be an object")
    validate_count_map(validation.get("source_counts"), "validation.source_counts")
    validate_count_map(validation.get("style_counts"), "validation.style_counts")


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def route_name(value: str | None) -> str:
    normalized = (value or "").strip().lower().replace("-", "_")
    if normalized in {ROUTE_NATIVE_CPP, "native", "cpp"}:
        return ROUTE_NATIVE_CPP
    if normalized in {ROUTE_LIVE_JS, "live", "runtime_js"}:
        return ROUTE_LIVE_JS
    if normalized == ROUTE_HYBRID:
        return ROUTE_HYBRID
    if normalized in {"recorded", "recorded_paint"}:
        return "recorded_paint"
    return ROUTE_UNSUPPORTED


def source_kind(path: str) -> str:
    suffix = pathlib.Path(path).suffix.lower()
    if suffix in {".jsx", ".tsx", ".js", ".ts"}:
        return "jsx"
    if suffix in {".html", ".htm"}:
        return "html"
    if suffix == ".json":
        return "design_json"
    return "pulp_js"


def count_map(source_audit: dict[str, Any]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for key in ("lines", "bytes"):
        value = source_audit.get(key)
        if isinstance(value, int) and value >= 0:
            counts[key] = value

    template_counts = source_audit.get("sourceTemplateCounts", {})
    if isinstance(template_counts, dict):
        for key, value in template_counts.items():
            if isinstance(key, str) and isinstance(value, int) and value >= 0:
                counts[key] = value

    component_invocations = source_audit.get("componentInvocationTemplates", {})
    if isinstance(component_invocations, dict):
        counts["component_invocations"] = sum(
            value for value in component_invocations.values() if isinstance(value, int) and value >= 0
        )

    component_props = source_audit.get("componentProps", {})
    if isinstance(component_props, dict):
        counts["component_prop_names"] = sum(
            len(value) for value in component_props.values() if isinstance(value, list)
        )

    return counts


def dynamic_risks(counts: dict[str, int]) -> list[str]:
    risks: list[str] = []
    if counts.get("useState", 0) > 0:
        risks.append("react_state_hooks")
    if counts.get("useEffect", 0) > 0:
        risks.append("react_effects")
    if counts.get("useRef", 0) > 0:
        risks.append("react_refs")
    if counts.get("useCallback", 0) > 0:
        risks.append("react_callbacks")
    if counts.get("mapCalls", 0) > 0:
        risks.append("runtime_array_maps")
    return risks


def source_span(row: dict[str, Any]) -> dict[str, Any] | None:
    path = row.get("stable_source_path")
    if not isinstance(path, str) or not path:
        return None
    span: dict[str, Any] = {
        "node_id": str(row.get("id", "")),
        "path": path,
    }
    line = row.get("source_line")
    if isinstance(line, int) and line > 0:
        span["line"] = line
    return span


def support_for_route(route: str) -> dict[str, str]:
    support = {
        "live_js": "planned",
        "native_html": "planned",
        "native_cpp": "planned",
        "recorded_paint": "planned",
        "hybrid": "planned",
    }
    if route in support:
        support[route] = "present"
    return support


def style_for_row(row: dict[str, Any]) -> dict[str, Any]:
    layout: dict[str, Any] = {}
    if isinstance(row.get("size"), (int, float)):
        layout["size"] = {
            "value": row["size"],
            "unit": "px",
            "provenance": "inline_style",
            "support": support_for_route(route_name(row.get("route_type"))),
        }

    paint_layers = []
    for token in row.get("style_token_references", []) or []:
        if isinstance(token, str):
            paint_layers.append({
                "value": token,
                "provenance": "token",
                "support": support_for_route(route_name(row.get("route_type"))),
            })

    style: dict[str, Any] = {
        "layout": layout,
        "paint_layers": paint_layers,
        "typography": {},
        "variants": {},
    }
    fallback = row.get("fallback_reason")
    if isinstance(fallback, str) and fallback:
        style["fallback_reasons"] = [fallback]
    return style


def state_for_row(row: dict[str, Any]) -> dict[str, Any]:
    parameters = []
    for binding in row.get("parameter_bindings", []) or []:
        if not isinstance(binding, dict):
            continue
        param_id = binding.get("param_key") or binding.get("binding_contract_id")
        if not isinstance(param_id, str) or not param_id:
            continue
        parameters.append({
            "id": param_id,
            "kind": "parameter",
            "gesture_policy": ",".join(
                boundary
                for gesture in row.get("gesture_contracts", []) or []
                if isinstance(gesture, dict)
                for boundary in gesture.get("boundaries", []) or []
                if isinstance(boundary, str)
            ),
            "route_usage": [route_name(row.get("route_type"))],
        })

    derived = {}
    label = row.get("label")
    if isinstance(label, str) and label:
        derived["label"] = label

    return {
        "parameters": parameters,
        "meters": [],
        "local_ui": {},
        "derived": derived,
        "dynamic_risk": [],
    }


def semantic_role(row: dict[str, Any]) -> str:
    primitive = row.get("required_native_primitive")
    if isinstance(primitive, str) and primitive:
        return primitive
    family = row.get("source_component_family") or row.get("source_component_name")
    if isinstance(family, str) and family:
        return family.strip().lower().replace(" ", "_")
    return "unknown"


def nodes_from_rows(rows: list[Any]) -> list[dict[str, Any]]:
    nodes = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        node_id = row.get("id")
        if not isinstance(node_id, str) or not node_id:
            continue
        node: dict[str, Any] = {
            "id": node_id,
            "semantic_role": semantic_role(row),
            "style": style_for_row(row),
            "state": state_for_row(row),
            "resources": [],
        }
        span = source_span(row)
        if span:
            node["source_span"] = span
        nodes.append(node)
    return nodes


def routes_from_rows(rows: list[Any]) -> list[dict[str, Any]]:
    routes = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        node_id = row.get("id")
        if not isinstance(node_id, str) or not node_id:
            continue
        chosen = route_name(row.get("route_type"))
        fallback = row.get("fallback_reason")
        route: dict[str, Any] = {
            "node_id": node_id,
            "semantic_role": semantic_role(row),
            "chosen_route": chosen,
            "candidate_routes": [{
                "route": chosen,
                "support": "present",
                "confidence": row.get("confidence", 0.0) if isinstance(row.get("confidence"), (int, float)) else 0.0,
            }],
            "reason": f"source contract maps to {semantic_role(row)}",
            "requires_js_engine": chosen in {ROUTE_LIVE_JS, ROUTE_HYBRID},
            "requires_gpu": chosen in {ROUTE_NATIVE_CPP, "native_html", "recorded_paint", ROUTE_HYBRID},
        }
        if chosen in {ROUTE_NATIVE_CPP, "native_html", "recorded_paint"}:
            route["requires_js_engine"] = False
            route["validation_refs"] = [f"route_manifest:{node_id}"]
        if chosen in {ROUTE_LIVE_JS, ROUTE_HYBRID, ROUTE_UNSUPPORTED}:
            route["fallback_reason"] = fallback if isinstance(fallback, str) and fallback else "requires live or unsupported behavior"
        routes.append(route)
    return routes


def style_counts(nodes: list[dict[str, Any]]) -> dict[str, int]:
    supported = 0
    unsupported = 0
    for node in nodes:
        style = node.get("style", {})
        if not isinstance(style, dict):
            continue
        layout = style.get("layout", {})
        if isinstance(layout, dict):
            supported += len(layout)
        paint = style.get("paint_layers", [])
        if isinstance(paint, list):
            supported += len(paint)
        fallback = style.get("fallback_reasons", [])
        if isinstance(fallback, list):
            unsupported += len(fallback)
    return {
        "supported": supported,
        "unsupported": unsupported,
    }


def artifact_ref_from_manifest(route_manifest: dict[str, Any], key: str, kind: str) -> dict[str, Any] | None:
    value = route_manifest.get("inputs", {}).get(key)
    if not isinstance(value, dict):
        return None
    path = value.get("path")
    if not isinstance(path, str) or not path:
        return None
    ref = {
        "path": path,
        "kind": kind,
    }
    sha = value.get("sha256")
    if isinstance(sha, str) and sha:
        ref["sha256"] = sha
    return ref


def build_frontend_ir(
    route_manifest: dict[str, Any],
    source_audit: dict[str, Any],
    route_manifest_path: pathlib.Path,
    repo_root: pathlib.Path,
) -> dict[str, Any]:
    overlay = route_manifest.get("source_contract_overlay", {})
    if not isinstance(overlay, dict):
        overlay = {}
    rows = overlay.get("node_route_rows", [])
    if not isinstance(rows, list):
        rows = []

    source_jsx = route_manifest.get("inputs", {}).get("sourceJsx", {})
    if not isinstance(source_jsx, dict):
        source_jsx = {}
    source_path = source_jsx.get("path") or source_audit.get("input") or ""
    if not isinstance(source_path, str):
        source_path = ""

    counts = count_map(source_audit)
    nodes = nodes_from_rows(rows)
    source_of_truth = overlay.get("source", {}).get("source_of_truth")
    if not isinstance(source_of_truth, str) or not source_of_truth:
        source_of_truth = "local_file"

    design_ref = artifact_ref_from_manifest(route_manifest, "ir", "design_ir") or {
        "path": "",
        "kind": "design_ir",
    }
    design_ref.setdefault("schema", "pulp-design-ir-v1")

    report: dict[str, Any] = {
        "schema": "pulp-frontend-ir-v0",
        "fixture_id": str(route_manifest.get("fixture") or overlay.get("fixture_id") or ""),
        "source": {
            "kind": source_kind(source_path),
            "path": source_path,
            "source_of_truth": source_of_truth,
            "counts": counts,
            "dynamic_risks": dynamic_risks(counts),
        },
        "design_ir": design_ref,
        "route_manifest": {
            "path": repo_relative(route_manifest_path, repo_root),
            "schema": str(route_manifest.get("schema", "")),
            "kind": "route_manifest",
        },
        "nodes": nodes,
        "resources": [],
        "tokens": {},
        "tweaks": [],
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
            "style_counts": style_counts(nodes),
            "compile": {
                "status": "not_run",
            },
            "binary_dependencies": {
                "js_engine_present": bool(route_manifest.get("route_metrics", {}).get("js_engine_initialized", False)),
            },
            "screenshots": [],
            "notes": [
                "frontend-ir v0 wraps existing import evidence; compile and binary audits must be supplied by route validators before production gating."
            ],
        },
    }
    sha = source_jsx.get("sha256")
    if isinstance(sha, str) and sha:
        report["source"]["sha256"] = sha
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--route-manifest", required=True, type=pathlib.Path)
    parser.add_argument("--source-audit", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)

    route_manifest = load_json(args.route_manifest)
    source_audit = load_json(args.source_audit)
    report = build_frontend_ir(route_manifest, source_audit, args.route_manifest, args.repo_root)
    validate_frontend_ir(report)
    write_json(args.output, report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
