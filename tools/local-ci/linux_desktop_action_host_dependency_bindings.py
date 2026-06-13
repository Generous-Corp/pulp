"""Host/process dependency bindings for Linux desktop actions."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


LINUX_DESKTOP_ACTION_HOST_DEPENDENCY_EXPORTS = ("linux_desktop_action_host_dependencies",)


def linux_desktop_action_host_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    subprocess_mod = _binding(bindings, "subprocess")

    return {
        "ensure_host_reachable_fn": _binding(bindings, "ensure_host_reachable"),
        "probe_linux_launch_backend_fn": _binding(bindings, "probe_linux_launch_backend"),
        "run_fn": subprocess_mod.run,
    }


def install_linux_desktop_action_host_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LINUX_DESKTOP_ACTION_HOST_DEPENDENCY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
