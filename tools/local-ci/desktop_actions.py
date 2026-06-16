"""Compatibility facade for desktop automation action helper policy."""

from __future__ import annotations

from desktop_action_artifacts import desktop_action_artifact_paths
from desktop_action_geometry import (
    content_size_from_view_tree,
    content_size_from_window,
    parse_coordinate_pair,
    screen_point_for_content_point,
)
from desktop_action_interaction_policy import (
    desktop_click_selector,
    desktop_interaction_requested,
    pulp_app_interaction_summary,
)
from desktop_action_label import default_desktop_label
from desktop_view_tree_helpers import (
    count_view_tree_nodes,
    iter_view_tree_nodes,
    resolve_view_tree_click_point,
    view_tree_inspector_summary,
)


__all__ = (
    "content_size_from_view_tree",
    "content_size_from_window",
    "count_view_tree_nodes",
    "default_desktop_label",
    "desktop_action_artifact_paths",
    "desktop_click_selector",
    "desktop_interaction_requested",
    "iter_view_tree_nodes",
    "parse_coordinate_pair",
    "pulp_app_interaction_summary",
    "resolve_view_tree_click_point",
    "screen_point_for_content_point",
    "view_tree_inspector_summary",
)
