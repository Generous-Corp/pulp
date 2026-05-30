#!/usr/bin/env python3
"""NodeIR assembly helpers for PulpFrontendIR reports."""

from __future__ import annotations

from typing import Any

from frontend_ir_routes import row_node_id, semantic_role
from frontend_ir_sources import source_span
from frontend_ir_state import state_for_row
from frontend_ir_styles import style_for_row


def nodes_from_rows(rows: list[Any]) -> list[dict[str, Any]]:
    nodes = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            continue
        node_id = row_node_id(row, index)
        node: dict[str, Any] = {
            "id": node_id,
            "semantic_role": semantic_role(row),
            "style": style_for_row(row),
            "state": state_for_row(row),
            "resources": [],
        }
        span = source_span(row, node_id)
        if span:
            node["source_span"] = span
        nodes.append(node)
    return nodes
