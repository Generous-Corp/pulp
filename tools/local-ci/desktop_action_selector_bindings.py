"""Bindings from the local_ci facade to desktop action selector helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def windows_requires_pulp_app_selectors(bindings: Mapping[str, Any], args: Any) -> bool:
    return _binding(bindings, "_desktop_action_commands_cli").windows_requires_pulp_app_selectors(args)
