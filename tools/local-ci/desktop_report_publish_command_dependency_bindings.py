"""Dependency assembly for desktop publish report command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def desktop_report_publish_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "desktop_run_manifests_fn": _binding(bindings, "desktop_run_manifests"),
        "stage_desktop_publish_report_fn": _binding(bindings, "stage_desktop_publish_report"),
        "desktop_publish_lines_fn": _binding_attr(bindings, "_desktop_cli", "desktop_publish_lines"),
    }
