"""Bindings from the local_ci facade to macOS window process helpers."""

from __future__ import annotations

import subprocess

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


MACOS_WINDOW_PROCESS_EXPORTS = (
    "terminate_process",
    "quit_macos_bundle_id",
)


def terminate_process(bindings: dict, proc: subprocess.Popen, timeout_secs: float = 5.0) -> None:
    return _binding(bindings, "_macos_desktop").terminate_process(proc, timeout_secs=timeout_secs)


def quit_macos_bundle_id(bindings: dict, bundle_id: str) -> None:
    return _binding(bindings, "_macos_desktop").quit_macos_bundle_id(
        bundle_id,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def install_macos_window_process_helpers(
    bindings: dict,
    names: tuple[str, ...] = MACOS_WINDOW_PROCESS_EXPORTS,
) -> None:
    known_names = set(MACOS_WINDOW_PROCESS_EXPORTS)
    process_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), process_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
