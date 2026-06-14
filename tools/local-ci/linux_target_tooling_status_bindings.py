"""Facade dependency bindings for Linux target tooling status helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


LINUX_TARGET_TOOLING_STATUS_EXPORTS = (
    "linux_tooling_detail",
    "linux_remote_tooling_ready",
)


def linux_tooling_detail(
    bindings: dict,
    probe: dict,
    tool_name: str,
    *,
    missing_hint: str | None = None,
) -> str:
    return _binding(bindings, "_linux_target").linux_tooling_detail(
        probe,
        tool_name,
        missing_hint=missing_hint,
    )


def linux_remote_tooling_ready(bindings: dict, probe: dict) -> bool:
    return _binding(bindings, "_linux_target").linux_remote_tooling_ready(
        probe,
        required_tools=_binding(bindings, "LINUX_REQUIRED_REMOTE_TOOLS"),
    )


def install_linux_target_tooling_status_helpers(
    bindings: dict,
    names: tuple[str, ...] = LINUX_TARGET_TOOLING_STATUS_EXPORTS,
) -> None:
    known_names = set(LINUX_TARGET_TOOLING_STATUS_EXPORTS)
    status_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), status_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
