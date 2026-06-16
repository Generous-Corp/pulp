"""Bindings from the local_ci facade to Windows CMake settings probe helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


WINDOWS_SESSION_CMAKE_PROBE_EXPORTS = ("probe_windows_ssh_cmake_settings",)


def probe_windows_ssh_cmake_settings(
    bindings: Mapping[str, Any],
    host: str,
    cmake_generator: str,
    cmake_platform: str,
    cmake_generator_instance: str,
) -> tuple[str, str]:
    return _binding(bindings, "_windows_probe").probe_windows_ssh_cmake_settings(
        host,
        cmake_generator,
        cmake_platform,
        cmake_generator_instance,
        windows_ssh_powershell_command_fn=_binding(bindings, "windows_ssh_powershell_command"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def install_windows_session_cmake_probe_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_SESSION_CMAKE_PROBE_EXPORTS,
) -> None:
    known_names = set(WINDOWS_SESSION_CMAKE_PROBE_EXPORTS)
    probe_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), probe_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
