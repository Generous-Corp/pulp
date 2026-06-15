"""Bindings for Windows target remote-tooling probe helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_TARGET_TOOLING_PROBE_EXPORTS = (
    "windows_tooling_detail",
    "windows_remote_tooling_ready",
)


def windows_tooling_detail(
    bindings: dict,
    probe: dict,
    tool_name: str,
    *,
    missing_hint: str | None = None,
) -> str:
    return _binding(bindings, "_windows_target").windows_tooling_detail(
        probe,
        tool_name,
        missing_hint=missing_hint,
    )


def windows_remote_tooling_ready(bindings: dict, probe: dict) -> bool:
    return _binding(bindings, "_windows_target").windows_remote_tooling_ready(
        probe,
        required_tools=_binding(bindings, "WINDOWS_REQUIRED_REMOTE_TOOLS"),
    )


def install_windows_target_tooling_probe_helpers(
    bindings: dict,
    names: tuple[str, ...] = WINDOWS_TARGET_TOOLING_PROBE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
