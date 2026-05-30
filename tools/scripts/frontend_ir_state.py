#!/usr/bin/env python3
"""State evidence helpers for PulpFrontendIR report assembly."""

from __future__ import annotations

from typing import Any

from frontend_ir_routes import route_name
from frontend_ir_validation import is_finite_number


def state_for_row(row: dict[str, Any]) -> dict[str, Any]:
    parameters = []
    for binding in row.get("parameter_bindings", []) or []:
        if not isinstance(binding, dict):
            continue
        param_id = binding.get("param_key") or binding.get("binding_contract_id")
        if not isinstance(param_id, str) or not param_id:
            continue
        parameter: dict[str, Any] = {
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
        }
        for source_key, target_key in (
            ("binding_contract_id", "source_binding_id"),
            ("module", "module"),
            ("param", "param"),
        ):
            value = binding.get(source_key)
            if isinstance(value, str) and value:
                parameter[target_key] = value
        for source_key, target_key in (
            ("value", "value"),
            ("initial_value", "initial_value"),
        ):
            value = row.get(source_key)
            if is_finite_number(value):
                parameter[target_key] = float(value)
        default_value = row.get("default_value")
        if is_finite_number(default_value):
            parameter["range"] = {"default": float(default_value)}
        default_source = row.get("default_value_source")
        if isinstance(default_source, str) and default_source:
            parameter["default_source"] = default_source
        parameters.append(parameter)

    derived = {}
    label = row.get("label")
    if isinstance(label, str) and label:
        derived["label"] = label

    local_ui = {}
    for contract in row.get("state_contracts", []) or []:
        if not isinstance(contract, dict):
            continue
        state_key = contract.get("state_key")
        if not isinstance(state_key, str) or not state_key:
            continue
        kind = contract.get("kind")
        local_ui[state_key] = kind if isinstance(kind, str) and kind else "state"

    return {
        "parameters": parameters,
        "meters": [],
        "local_ui": local_ui,
        "derived": derived,
        "dynamic_risk": [],
    }


def state_counts(nodes: list[dict[str, Any]]) -> dict[str, int]:
    counts = {
        "parameters": 0,
        "parameters_with_value": 0,
        "parameters_with_initial_value": 0,
        "parameters_with_default": 0,
        "parameters_with_source_binding_id": 0,
        "parameters_with_module_param": 0,
        "meters": 0,
        "local_ui_state_keys": 0,
    }
    for node in nodes:
        state = node.get("state", {})
        if not isinstance(state, dict):
            continue
        parameters = state.get("parameters", [])
        if isinstance(parameters, list):
            counts["parameters"] += len(parameters)
            for parameter in parameters:
                if not isinstance(parameter, dict):
                    continue
                if is_finite_number(parameter.get("value")):
                    counts["parameters_with_value"] += 1
                if is_finite_number(parameter.get("initial_value")):
                    counts["parameters_with_initial_value"] += 1
                if isinstance(parameter.get("range"), dict) and is_finite_number(parameter["range"].get("default")):
                    counts["parameters_with_default"] += 1
                if isinstance(parameter.get("source_binding_id"), str) and parameter["source_binding_id"]:
                    counts["parameters_with_source_binding_id"] += 1
                if (isinstance(parameter.get("module"), str) and parameter["module"] and
                        isinstance(parameter.get("param"), str) and parameter["param"]):
                    counts["parameters_with_module_param"] += 1
        meters = state.get("meters", [])
        if isinstance(meters, list):
            counts["meters"] += len(meters)
        local_ui = state.get("local_ui", {})
        if isinstance(local_ui, dict):
            counts["local_ui_state_keys"] += len(local_ui)
    return counts
