"""Bindings from the local_ci facade to the desktop video setup/doctor commands."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_VIDEO_SETUP_COMMAND_EXPORTS = (
    "cmd_desktop_video_doctor",
    "cmd_desktop_video_setup",
)


def _doctor_deps(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "resolve_desktop_target_fn": _binding(bindings, "resolve_desktop_target"),
        "desktop_doctor_checks_fn": _binding(bindings, "desktop_doctor_checks"),
        "normalize_desktop_optional_config_fn": _binding(bindings, "normalize_desktop_optional_config"),
        "video_proof_smoke_fn": _binding(bindings, "video_proof_smoke"),
        "probe_macos_avfoundation_audio_fn": _binding(bindings, "probe_macos_avfoundation_audio"),
    }


def cmd_desktop_video_doctor(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_video_setup_commands_cli").cmd_desktop_video_doctor(
        args,
        **_doctor_deps(bindings),
    )


def cmd_desktop_video_setup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_video_setup_commands_cli").cmd_desktop_video_setup(
        args,
        **_doctor_deps(bindings),
        desktop_video_matrix_payload_fn=_binding(bindings, "_desktop_video_matrix_commands_cli").desktop_video_matrix_payload,
        save_config_fn=_binding(bindings, "save_config"),
    )


def install_desktop_video_setup_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_VIDEO_SETUP_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_VIDEO_SETUP_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
