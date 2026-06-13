"""Bindings from the local_ci facade to desktop view/action helpers."""

from __future__ import annotations

from binding_utils import binding as _binding


def count_view_tree_nodes(bindings: dict, node: object) -> int:
    return _binding(bindings, "_desktop_actions").count_view_tree_nodes(node)


def parse_coordinate_pair(bindings: dict, value: str, *, flag_name: str) -> tuple[float, float]:
    return _binding(bindings, "_desktop_actions").parse_coordinate_pair(value, flag_name=flag_name)


def iter_view_tree_nodes(bindings: dict, node: object, *, offset_x: float = 0.0, offset_y: float = 0.0):
    yield from _binding(bindings, "_desktop_actions").iter_view_tree_nodes(
        node,
        offset_x=offset_x,
        offset_y=offset_y,
    )


def resolve_view_tree_click_point(
    bindings: dict,
    view_tree: dict,
    *,
    view_id: str | None,
    view_type: str | None,
    view_text: str | None,
    view_label: str | None,
) -> tuple[float, float]:
    return _binding(bindings, "_desktop_actions").resolve_view_tree_click_point(
        view_tree,
        view_id=view_id,
        view_type=view_type,
        view_text=view_text,
        view_label=view_label,
    )


def screen_point_for_content_point(
    bindings: dict,
    window: dict,
    content_size: tuple[float, float],
    content_point: tuple[float, float],
) -> tuple[float, float]:
    return _binding(bindings, "_desktop_actions").screen_point_for_content_point(
        window,
        content_size,
        content_point,
    )


def default_desktop_label(bindings: dict, command: str | None, *, bundle_id: str | None = None) -> str:
    return _binding(bindings, "_desktop_actions").default_desktop_label(command, bundle_id=bundle_id)
