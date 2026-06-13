"""Bindings from the local_ci facade to desktop config command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def cmd_desktop_config_show(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_config_show(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_config_show_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_config_show_lines"),
    )


def cmd_desktop_config_set(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_config_set(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        save_config_fn=_binding(bindings, "save_config"),
        config_path_fn=_binding(bindings, "config_path"),
        normalize_publish_mode_fn=_binding(bindings, "normalize_publish_mode"),
        parse_config_bool_fn=_binding(bindings, "parse_config_bool"),
        normalize_desktop_config_fn=_binding(bindings, "normalize_desktop_config"),
        desktop_config_update_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_config_update_lines"),
    )
