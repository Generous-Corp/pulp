"""Desktop action coordinate and content geometry helpers."""

from __future__ import annotations


def parse_coordinate_pair(value: str, *, flag_name: str) -> tuple[float, float]:
    parts = [segment.strip() for segment in value.split(",", 1)]
    if len(parts) != 2:
        raise ValueError(f"{flag_name} must be in X,Y form.")
    try:
        return float(parts[0]), float(parts[1])
    except ValueError as exc:
        raise ValueError(f"{flag_name} must contain numeric X,Y values.") from exc


def content_size_from_window(window: dict) -> tuple[float, float]:
    bounds = window.get("bounds", {})
    return (
        float(bounds.get("width", 0.0) or 0.0),
        float(bounds.get("height", 0.0) or 0.0),
    )


def content_size_from_view_tree(
    view_tree: dict,
    fallback: tuple[float, float],
) -> tuple[float, float]:
    root_bounds = view_tree.get("bounds") if isinstance(view_tree.get("bounds"), dict) else {}
    return (
        float(root_bounds.get("width", fallback[0]) or fallback[0]),
        float(root_bounds.get("height", fallback[1]) or fallback[1]),
    )


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
