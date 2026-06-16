"""Dependency assembly for the local-CI PR list command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def local_ci_pr_list_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "gh_available_fn": _binding(bindings, "gh_available"),
        "gh_pr_list_open_fn": _binding(bindings, "gh_pr_list_open"),
        "open_pr_list_lines_fn": _binding(bindings, "open_pr_list_lines"),
    }
