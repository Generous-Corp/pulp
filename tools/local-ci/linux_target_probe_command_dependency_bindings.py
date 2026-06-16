"""Dependency assembly for Linux target probe command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def linux_target_probe_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "ssh_command_result_fn": _binding(bindings, "ssh_command_result"),
    }
