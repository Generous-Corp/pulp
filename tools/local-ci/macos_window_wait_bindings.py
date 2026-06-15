"""Facade dependency bindings for macOS window wait helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


MACOS_WINDOW_WAIT_EXPORTS = (
    "wait_for_macos_window",
    "wait_for_macos_bundle_window",
)


def wait_for_macos_window(bindings: dict, pid: int, timeout_secs: float) -> dict:
    return _binding(bindings, "_macos_desktop").wait_for_macos_window(
        pid,
        timeout_secs,
        macos_window_info_for_pid_fn=_binding(bindings, "macos_window_info_for_pid"),
        time_fn=_binding_attr(bindings, "time", "time"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )


def wait_for_macos_bundle_window(bindings: dict, bundle_id: str, timeout_secs: float) -> tuple[int, dict]:
    return _binding(bindings, "_macos_desktop").wait_for_macos_bundle_window(
        bundle_id,
        timeout_secs,
        macos_window_info_for_bundle_id_fn=_binding(bindings, "macos_window_info_for_bundle_id"),
        activate_macos_bundle_id_fn=_binding(bindings, "activate_macos_bundle_id"),
        time_fn=_binding_attr(bindings, "time", "time"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )


def install_macos_window_wait_helpers(
    bindings: dict,
    names: tuple[str, ...] = MACOS_WINDOW_WAIT_EXPORTS,
) -> None:
    known_names = set(MACOS_WINDOW_WAIT_EXPORTS)
    wait_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), wait_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
