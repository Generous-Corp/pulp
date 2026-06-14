"""Facade dependency bindings for Linux target tool constants."""

from __future__ import annotations

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


LINUX_TARGET_CONSTANT_EXPORTS = (
    "linux_required_remote_tools",
    "linux_optional_remote_tools",
)


def linux_required_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_linux_target").LINUX_REQUIRED_REMOTE_TOOLS


def linux_optional_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_linux_target").LINUX_OPTIONAL_REMOTE_TOOLS


def install_linux_target_constant_helpers(
    bindings: dict,
    names: tuple[str, ...] = LINUX_TARGET_CONSTANT_EXPORTS,
) -> None:
    known_names = set(LINUX_TARGET_CONSTANT_EXPORTS)
    constant_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), constant_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
