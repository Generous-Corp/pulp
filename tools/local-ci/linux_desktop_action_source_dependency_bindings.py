"""Source/remote-command dependency bindings for Linux desktop actions."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


LINUX_DESKTOP_ACTION_SOURCE_DEPENDENCY_EXPORTS = ("linux_desktop_action_source_dependencies",)


def linux_desktop_action_source_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "prepare_linux_exact_sha_source_fn": _binding(bindings, "prepare_linux_exact_sha_source"),
        "remote_linux_bundle_relpath_fn": _binding(bindings, "remote_linux_bundle_relpath"),
        "build_linux_xvfb_remote_command_fn": _binding(bindings, "build_linux_xvfb_remote_command"),
        "build_linux_window_driver_remote_command_fn": _binding(bindings, "build_linux_window_driver_remote_command"),
        "attach_desktop_source_to_manifest_fn": _binding(bindings, "attach_desktop_source_to_manifest"),
    }


def install_linux_desktop_action_source_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LINUX_DESKTOP_ACTION_SOURCE_DEPENDENCY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
