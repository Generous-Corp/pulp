"""Bindings from the local_ci facade to macOS window activation helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


MACOS_WINDOW_ACTIVATION_EXPORTS = (
    "activate_macos_pid",
    "activate_macos_bundle_id",
)


def activate_macos_pid(bindings: dict, pid: int) -> dict:
    return _binding(bindings, "_macos_desktop").activate_macos_pid(
        pid,
        probe_path_fn=_binding(bindings, "macos_window_probe_path"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def activate_macos_bundle_id(bindings: dict, bundle_id: str) -> dict:
    return _binding(bindings, "_macos_desktop").activate_macos_bundle_id(
        bundle_id,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def install_macos_window_activation_helpers(
    bindings: dict,
    names: tuple[str, ...] = MACOS_WINDOW_ACTIVATION_EXPORTS,
) -> None:
    known_names = set(MACOS_WINDOW_ACTIVATION_EXPORTS)
    activation_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), activation_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
