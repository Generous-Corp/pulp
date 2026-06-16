"""Bindings from the local_ci facade to desktop CLI dispatch helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


CLI_DESKTOP_DISPATCH_EXPORTS = (
    "cmd_desktop_config",
    "cmd_desktop",
)


def cmd_desktop_config(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_config(
        args,
        commands={
            "show": _binding(bindings, "cmd_desktop_config_show"),
            "set": _binding(bindings, "cmd_desktop_config_set"),
        },
    )


def cmd_desktop(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cli_dispatch").dispatch_desktop_command(
        args,
        commands={
            "install": _binding(bindings, "cmd_desktop_install"),
            "doctor": _binding(bindings, "cmd_desktop_doctor"),
            "status": _binding(bindings, "cmd_desktop_status"),
            "config": _binding(bindings, "cmd_desktop_config"),
            "recent": _binding(bindings, "cmd_desktop_recent"),
            "proof": _binding(bindings, "cmd_desktop_proof"),
            "publish": _binding(bindings, "cmd_desktop_publish"),
            "cleanup": _binding(bindings, "cmd_desktop_cleanup"),
            "smoke": _binding(bindings, "cmd_desktop_smoke"),
            "click": _binding(bindings, "cmd_desktop_click"),
            "inspect": _binding(bindings, "cmd_desktop_inspect"),
            "verdict": _binding(bindings, "cmd_desktop_verdict"),
            "review-issue": _binding(bindings, "cmd_desktop_review_issue"),
            "review-status": _binding(bindings, "cmd_desktop_review_status"),
            "review-watch": _binding(bindings, "cmd_desktop_review_watch"),
            "compose-video": _binding(bindings, "cmd_desktop_compose_video"),
            "design-diff": _binding(bindings, "cmd_desktop_design_diff"),
            "design-proof": _binding(bindings, "cmd_desktop_design_proof"),
            "video-matrix": _binding(bindings, "cmd_desktop_video_matrix"),
            "video": _binding(bindings, "cmd_desktop_video"),
        },
    )


def install_cli_desktop_dispatch_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLI_DESKTOP_DISPATCH_EXPORTS,
) -> None:
    known_names = set(CLI_DESKTOP_DISPATCH_EXPORTS)
    dispatch_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), dispatch_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
