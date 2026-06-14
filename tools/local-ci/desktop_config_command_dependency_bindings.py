"""Dependency assembly for desktop config command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def desktop_config_show_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "desktop_config_show_lines_fn": _binding_attr(bindings, "_desktop_cli", "desktop_config_show_lines"),
    }


def desktop_config_set_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "save_config_fn": _binding(bindings, "save_config"),
        "config_path_fn": _binding(bindings, "config_path"),
        "normalize_publish_mode_fn": _binding(bindings, "normalize_publish_mode"),
        "parse_config_bool_fn": _binding(bindings, "parse_config_bool"),
        "normalize_desktop_config_fn": _binding(bindings, "normalize_desktop_config"),
        "desktop_config_update_lines_fn": _binding_attr(bindings, "_desktop_cli", "desktop_config_update_lines"),
    }
