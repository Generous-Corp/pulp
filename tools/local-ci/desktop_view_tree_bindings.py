"""Bindings from the local_ci facade to desktop view-tree helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


DESKTOP_VIEW_TREE_EXPORTS = (
    "count_view_tree_nodes",
    "iter_view_tree_nodes",
    "resolve_view_tree_click_point",
)


def count_view_tree_nodes(bindings: dict, node: object) -> int:
    return _binding(bindings, "_desktop_actions").count_view_tree_nodes(node)


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


def install_desktop_view_tree_helpers(
    bindings: dict,
    names: tuple[str, ...] = DESKTOP_VIEW_TREE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
