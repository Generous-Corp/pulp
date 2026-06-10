"""Desktop automation action helper policy for local CI."""

from __future__ import annotations

from pathlib import Path
import shlex


def count_view_tree_nodes(node: object) -> int:
    if not isinstance(node, dict):
        return 0
    total = 1
    children = node.get("children")
    if isinstance(children, list):
        total += sum(count_view_tree_nodes(child) for child in children)
    return total


def parse_coordinate_pair(value: str, *, flag_name: str) -> tuple[float, float]:
    parts = [segment.strip() for segment in value.split(",", 1)]
    if len(parts) != 2:
        raise ValueError(f"{flag_name} must be in X,Y form.")
    try:
        return float(parts[0]), float(parts[1])
    except ValueError as exc:
        raise ValueError(f"{flag_name} must contain numeric X,Y values.") from exc


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


def screen_point_for_content_point(
    window: dict,
    content_size: tuple[float, float],
    content_point: tuple[float, float],
) -> tuple[float, float]:
    bounds = window.get("bounds", {})
    window_x = float(bounds.get("x", 0.0) or 0.0)
    window_y = float(bounds.get("y", 0.0) or 0.0)
    window_width = float(bounds.get("width", 0.0) or 0.0)
    window_height = float(bounds.get("height", 0.0) or 0.0)
    content_width, content_height = content_size
    point_x, point_y = content_point

    inset_x = max((window_width - content_width) / 2.0, 0.0)
    inset_y = max(window_height - content_height, 0.0)
    return window_x + inset_x + point_x, window_y + inset_y + point_y


def default_desktop_label(command: str | None, *, bundle_id: str | None = None) -> str:
    if bundle_id:
        return bundle_id.split(".")[-1] or bundle_id
    args = shlex.split(command or "")
    if not args:
        return "desktop-run"
    return Path(args[0]).stem or "desktop-run"
