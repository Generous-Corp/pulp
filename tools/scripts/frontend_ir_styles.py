#!/usr/bin/env python3
"""Style evidence helpers for PulpFrontendIR report assembly."""

from __future__ import annotations

from typing import Any

from frontend_ir_routes import route_name, support_for_route
from frontend_ir_sources import metric_key
from frontend_ir_validation import is_finite_number, is_non_negative_int


def style_for_row(row: dict[str, Any]) -> dict[str, Any]:
    layout: dict[str, Any] = {}
    if is_finite_number(row.get("size")):
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


def iter_style_values(style: dict[str, Any]) -> list[dict[str, Any]]:
    values = []
    for bucket in ("layout", "typography"):
        entries = style.get(bucket, {})
        if isinstance(entries, dict):
            values.extend(value for value in entries.values() if isinstance(value, dict))

    paint = style.get("paint_layers", [])
    if isinstance(paint, list):
        values.extend(value for value in paint if isinstance(value, dict))

    variants = style.get("variants", {})
    if isinstance(variants, dict):
        for entries in variants.values():
            if isinstance(entries, dict):
                values.extend(value for value in entries.values() if isinstance(value, dict))
    return values


def style_counts(nodes: list[dict[str, Any]], source_counts: dict[str, int] | None = None) -> dict[str, int]:
    supported = 0
    unsupported = 0
    support_counts: dict[str, int] = {}
    for node in nodes:
        style = node.get("style", {})
        if not isinstance(style, dict):
            continue
        values = iter_style_values(style)
        supported += len(values)
        for value in values:
            support = value.get("support", {})
            if not isinstance(support, dict):
                continue
            for backend, status in support.items():
                if not isinstance(backend, str) or not isinstance(status, str):
                    continue
                key = f"support_{metric_key(backend)}_{metric_key(status)}"
                support_counts[key] = support_counts.get(key, 0) + 1
        fallback = style.get("fallback_reasons", [])
        if isinstance(fallback, list):
            unsupported += len(fallback)
    counts = {
        "supported": supported,
        "unsupported": unsupported,
    }
    counts.update(dict(sorted(support_counts.items())))
    if source_counts:
        for source_keys, style_key in (
            (("css_values",), "source_css_values"),
            (("css_values_valid",), "source_css_values_valid_syntax"),
            (("css_values_invalid",), "source_css_values_invalid_syntax"),
            (("style_objects", "inlineStyleObjects"), "source_style_objects"),
            (("styleAttributes", "style_attributes", "inline_style_attributes"), "source_style_attributes"),
            (("style_keys",), "source_style_keys"),
            (("materiality_style_values_normalized",), "source_style_values_normalized"),
            (("materiality_css_lexer_matches",), "source_css_lexer_matches"),
            (("materiality_dynamic_style_values",), "source_dynamic_style_values"),
            (("materiality_conditional_style_values",), "source_conditional_style_values"),
        ):
            for source_key in source_keys:
                value = source_counts.get(source_key)
                if is_non_negative_int(value):
                    counts[style_key] = value
                    break
    return counts
