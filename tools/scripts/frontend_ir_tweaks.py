#!/usr/bin/env python3
"""Tweak sidecar helpers for PulpFrontendIR reports."""

from __future__ import annotations

import json
import pathlib
from typing import Any

from frontend_ir_validation import SCHEMAS


VALID_INVALIDATIONS = {"source", "style", "resource", "route", "validation"}


def load_json(path: pathlib.Path) -> dict[str, Any] | list[Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, (dict, list)):
        raise ValueError(f"{path} must contain a JSON object or array")
    return data


def default_invalidates(property_name: str) -> list[str]:
    if property_name.startswith("source."):
        return ["source"]
    if property_name.startswith("route."):
        return ["route"]
    if property_name.startswith("resource."):
        return ["resource"]
    if property_name.startswith("validation."):
        return ["validation"]
    return ["style"]


def normalize_invalidates(value: Any, property_name: str, field: str) -> list[str]:
    if value is None:
        return default_invalidates(property_name)
    if not isinstance(value, list):
        raise ValueError(f"{field}.invalidates must be an array")
    invalidates = []
    for index, item in enumerate(value):
        if item not in VALID_INVALIDATIONS:
            raise ValueError(f"{field}.invalidates[{index}] is invalid")
        invalidates.append(item)
    return sorted(set(invalidates))


def normalize_tweak(item: Any, index: int, valid_node_ids: set[str] | None = None) -> dict[str, Any]:
    field = f"tweaks[{index}]"
    if not isinstance(item, dict):
        raise ValueError(f"{field} must be an object")
    node_id = item.get("node_id")
    if not isinstance(node_id, str) or not node_id:
        raise ValueError(f"{field}.node_id must be a string")
    if valid_node_ids is not None and node_id not in valid_node_ids:
        raise ValueError(f"{field}.node_id is not present in the FrontendIR nodes")
    property_name = item.get("property")
    if not isinstance(property_name, str) or not property_name:
        raise ValueError(f"{field}.property must be a string")
    if "value" not in item:
        raise ValueError(f"{field}.value is required")

    invalidates = normalize_invalidates(item.get("invalidates"), property_name, field)
    classification_preserved = item.get("classification_preserved")
    if classification_preserved is None:
        classification_preserved = not any(scope in {"source", "route"} for scope in invalidates)
    if not isinstance(classification_preserved, bool):
        raise ValueError(f"{field}.classification_preserved must be a boolean")

    return {
        "node_id": node_id,
        "property": property_name,
        "value": item["value"],
        "invalidates": invalidates,
        "classification_preserved": classification_preserved,
    }


def tweaks_from_sidecar(path: pathlib.Path, valid_node_ids: set[str] | None = None) -> list[dict[str, Any]]:
    data = load_json(path)
    if isinstance(data, list):
        raw_tweaks = data
    else:
        schema = data.get("schema")
        if schema is not None and schema not in {SCHEMAS["tweaks"], SCHEMAS["tweaks_namespaced"]}:
            raise ValueError(f"{path} has unsupported tweak sidecar schema: {schema}")
        raw_tweaks = data.get("tweaks", [])
    if not isinstance(raw_tweaks, list):
        raise ValueError(f"{path}.tweaks must be an array")
    return [
        normalize_tweak(item, index, valid_node_ids)
        for index, item in enumerate(raw_tweaks)
    ]
