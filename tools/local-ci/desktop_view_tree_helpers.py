"""Desktop action view-tree traversal and selection helpers."""

from __future__ import annotations


def count_view_tree_nodes(node: object) -> int:
    if not isinstance(node, dict):
        return 0
    total = 1
    children = node.get("children")
    if isinstance(children, list):
        total += sum(count_view_tree_nodes(child) for child in children)
    return total


def iter_view_tree_nodes(node: object, *, offset_x: float = 0.0, offset_y: float = 0.0):
    if not isinstance(node, dict):
        return
    bounds = node.get("bounds") if isinstance(node.get("bounds"), dict) else {}
    absolute_x = offset_x + float(bounds.get("x", 0.0) or 0.0)
    absolute_y = offset_y + float(bounds.get("y", 0.0) or 0.0)
    absolute_bounds = {
        "x": absolute_x,
        "y": absolute_y,
        "width": float(bounds.get("width", 0.0) or 0.0),
        "height": float(bounds.get("height", 0.0) or 0.0),
    }
    yield node, absolute_bounds
    children = node.get("children")
    if isinstance(children, list):
        for child in children:
            yield from iter_view_tree_nodes(child, offset_x=absolute_x, offset_y=absolute_y)


def resolve_view_tree_click_point(
    view_tree: dict,
    *,
    view_id: str | None,
    view_type: str | None,
    view_text: str | None,
    view_label: str | None,
) -> tuple[float, float]:
    for node, bounds in iter_view_tree_nodes(view_tree):
        if not node.get("visible", True):
            continue
        if view_id and node.get("id") != view_id:
            continue
        if view_type and node.get("type") != view_type:
            continue
        if view_text and node.get("text") != view_text:
            continue
        if view_label and node.get("label") != view_label:
            continue
        if bounds["width"] <= 0 or bounds["height"] <= 0:
            continue
        return bounds["x"] + (bounds["width"] / 2.0), bounds["y"] + (bounds["height"] / 2.0)
    filters = [
        part for part in [
            f"id={view_id}" if view_id else None,
            f"type={view_type}" if view_type else None,
            f"text={view_text}" if view_text else None,
            f"label={view_label}" if view_label else None,
        ] if part
    ]
    joined = ", ".join(filters) or "<none>"
    raise RuntimeError(f"No visible view matched click selector ({joined}).")


def view_tree_inspector_summary(view_tree: dict) -> dict:
    return {
        "root_id": view_tree.get("id"),
        "root_type": view_tree.get("type"),
        "view_count": count_view_tree_nodes(view_tree),
    }
