"""Bindings from the local_ci facade to desktop action geometry helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


DESKTOP_ACTION_GEOMETRY_EXPORTS = (
    "parse_coordinate_pair",
    "screen_point_for_content_point",
)


def parse_coordinate_pair(bindings: dict, value: str, *, flag_name: str) -> tuple[float, float]:
    return _binding(bindings, "_desktop_actions").parse_coordinate_pair(value, flag_name=flag_name)


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


def install_desktop_action_geometry_helpers(
    bindings: dict,
    names: tuple[str, ...] = DESKTOP_ACTION_GEOMETRY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
