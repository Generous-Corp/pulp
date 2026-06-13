"""Process/app dependency bindings for macOS desktop smoke actions."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


MACOS_DESKTOP_SMOKE_PROCESS_DEPENDENCY_EXPORTS = ("macos_desktop_smoke_process_dependencies",)


def macos_desktop_smoke_process_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    subprocess_mod = _binding(bindings, "subprocess")
    time_mod = _binding(bindings, "time")
    shlex_mod = _binding(bindings, "shlex")
    os_mod = _binding(bindings, "os")

    return {
        "macos_accessibility_trusted_fn": _binding(bindings, "macos_accessibility_trusted"),
        "quit_macos_bundle_id_fn": _binding(bindings, "quit_macos_bundle_id"),
        "sleep_fn": time_mod.sleep,
        "run_fn": subprocess_mod.run,
        "activate_macos_bundle_id_fn": _binding(bindings, "activate_macos_bundle_id"),
        "wait_for_macos_bundle_window_fn": _binding(bindings, "wait_for_macos_bundle_window"),
        "split_command_fn": shlex_mod.split,
        "detect_macos_app_bundle_fn": _binding(bindings, "detect_macos_app_bundle"),
        "macos_bundle_id_for_app_path_fn": _binding(bindings, "macos_bundle_id_for_app_path"),
        "environ_copy_fn": os_mod.environ.copy,
        "popen_fn": subprocess_mod.Popen,
        "terminate_process_fn": _binding(bindings, "terminate_process"),
    }


def install_macos_desktop_smoke_process_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = MACOS_DESKTOP_SMOKE_PROCESS_DEPENDENCY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
