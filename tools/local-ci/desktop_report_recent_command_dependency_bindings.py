"""Dependency assembly for desktop recent report command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def desktop_report_recent_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "desktop_run_manifests_fn": _binding(bindings, "desktop_run_manifests"),
        "desktop_run_summary_fn": _binding(bindings, "desktop_run_summary"),
        "desktop_recent_lines_fn": _binding_attr(bindings, "_desktop_cli", "desktop_recent_lines"),
        "short_sha_fn": _binding(bindings, "short_sha"),
    }
