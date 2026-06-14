"""Host/session dependency bindings for Windows desktop actions."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_DESKTOP_ACTION_HOST_DEPENDENCY_EXPORTS = ("windows_desktop_action_host_dependencies",)


def windows_desktop_action_host_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    time_mod = _binding(bindings, "time")

    return {
        "ensure_host_reachable_fn": _binding(bindings, "ensure_host_reachable"),
        "desktop_receipt_for_fn": _binding(bindings, "desktop_receipt_for"),
        "desktop_target_contract_fn": _binding(bindings, "desktop_target_contract"),
        "probe_windows_session_agent_fn": _binding(bindings, "probe_windows_session_agent"),
        "windows_desktop_session_user_fn": _binding(bindings, "windows_desktop_session_user"),
        "time_fn": time_mod.time,
        "sleep_fn": time_mod.sleep,
    }


def install_windows_desktop_action_host_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_DESKTOP_ACTION_HOST_DEPENDENCY_EXPORTS,
) -> None:
    known_names = set(WINDOWS_DESKTOP_ACTION_HOST_DEPENDENCY_EXPORTS)
    host_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), host_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
