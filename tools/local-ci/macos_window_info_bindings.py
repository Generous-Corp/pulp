"""Facade dependency bindings for macOS window info helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


MACOS_WINDOW_INFO_EXPORTS = (
    "macos_window_info_for_pid",
    "macos_window_info_for_bundle_id",
    "macos_accessibility_trusted",
)


def macos_window_info_for_pid(bindings: dict, pid: int) -> dict:
    return _binding(bindings, "_macos_desktop").macos_window_info_for_pid(
        pid,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def macos_window_info_for_bundle_id(bindings: dict, bundle_id: str) -> dict:
    return _binding(bindings, "_macos_desktop").macos_window_info_for_bundle_id(
        bundle_id,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def macos_accessibility_trusted(bindings: dict) -> bool:
    return _binding(bindings, "_macos_desktop").macos_accessibility_trusted(
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def install_macos_window_info_helpers(
    bindings: dict,
    names: tuple[str, ...] = MACOS_WINDOW_INFO_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
