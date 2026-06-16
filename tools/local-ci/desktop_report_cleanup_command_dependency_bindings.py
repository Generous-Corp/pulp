"""Dependency assembly for desktop cleanup report command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def desktop_report_cleanup_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "prune_desktop_run_manifests_fn": _binding(bindings, "prune_desktop_run_manifests"),
        "write_desktop_run_rollups_fn": _binding(bindings, "write_desktop_run_rollups"),
        "desktop_cleanup_empty_line_fn": _binding_attr(bindings, "_desktop_cli", "desktop_cleanup_empty_line"),
        "desktop_cleanup_lines_fn": _binding_attr(bindings, "_desktop_cli", "desktop_cleanup_lines"),
    }
