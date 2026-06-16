"""Bindings from the local_ci facade to the desktop video compose/design commands."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_VIDEO_COMPOSE_COMMAND_EXPORTS = (
    "cmd_desktop_compose_video",
    "cmd_desktop_design_diff",
    "cmd_desktop_design_proof",
)


def _design_parity_diff_summary(bindings: Mapping[str, Any], *args, **kwargs) -> dict:
    return _binding(bindings, "_io_utils_design_parity").design_parity_diff_summary(*args, **kwargs)


def cmd_desktop_compose_video(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_video_compose_commands_cli").cmd_desktop_compose_video(
        args,
        compose_desktop_video_proof_fn=_binding(bindings, "compose_desktop_video_proof"),
        create_issue_video_variant_fn=_binding(bindings, "create_issue_video_variant"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def cmd_desktop_design_diff(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_video_compose_commands_cli").cmd_desktop_design_diff(
        args,
        design_parity_diff_summary_fn=lambda *a, **k: _design_parity_diff_summary(bindings, *a, **k),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def cmd_desktop_design_proof(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_video_compose_commands_cli").cmd_desktop_design_proof(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        design_parity_diff_summary_fn=lambda *a, **k: _design_parity_diff_summary(bindings, *a, **k),
        compose_desktop_video_proof_fn=_binding(bindings, "compose_desktop_video_proof"),
        create_issue_video_variant_fn=_binding(bindings, "create_issue_video_variant"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def install_desktop_video_compose_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_VIDEO_COMPOSE_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_VIDEO_COMPOSE_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
