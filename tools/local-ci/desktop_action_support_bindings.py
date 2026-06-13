"""Compatibility facade for desktop action support dependency bindings."""

from __future__ import annotations

from desktop_target_selection_bindings import resolve_desktop_target
from desktop_view_action_bindings import (
    count_view_tree_nodes,
    default_desktop_label,
    iter_view_tree_nodes,
    parse_coordinate_pair,
    resolve_view_tree_click_point,
    screen_point_for_content_point,
)


DESKTOP_ACTION_SUPPORT_EXPORTS = (
    "resolve_desktop_target",
    "count_view_tree_nodes",
    "parse_coordinate_pair",
    "iter_view_tree_nodes",
    "resolve_view_tree_click_point",
    "screen_point_for_content_point",
    "default_desktop_label",
)

