"""Video dependency bindings for macOS desktop smoke actions.

Supplies the video-recording / focus / compose / terminal-proof / window-select
callables that run_macos_local_smoke needs when recording a video proof.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


MACOS_DESKTOP_SMOKE_VIDEO_DEPENDENCY_EXPORTS = ("macos_desktop_smoke_video_dependencies",)


def macos_desktop_smoke_video_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "wait_for_macos_bundle_window_title_fn": _binding(bindings, "wait_for_macos_bundle_window_title"),
        "wait_for_macos_bundle_secondary_window_fn": _binding(bindings, "wait_for_macos_bundle_secondary_window"),
        "cwd_path_fn": _binding(bindings, "Path").cwd,
        "launch_macos_terminal_proof_command_fn": _binding(bindings, "launch_macos_terminal_proof_command"),
        "close_macos_terminal_windows_with_title_fn": _binding(bindings, "close_macos_terminal_windows_with_title"),
        "start_macos_window_video_recording_fn": _binding(bindings, "start_macos_window_video_recording"),
        "stop_macos_window_video_recording_fn": _binding(bindings, "stop_macos_window_video_recording"),
        "mux_desktop_video_audio_fn": _binding(bindings, "mux_desktop_video_audio"),
        "generate_interaction_focus_fn": _binding(bindings, "generate_interaction_focus"),
        "compose_desktop_video_proof_fn": _binding(bindings, "compose_desktop_video_proof"),
        "create_issue_video_variant_fn": _binding(bindings, "create_issue_video_variant"),
    }


def install_macos_desktop_smoke_video_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = MACOS_DESKTOP_SMOKE_VIDEO_DEPENDENCY_EXPORTS,
) -> None:
    known_names = set(MACOS_DESKTOP_SMOKE_VIDEO_DEPENDENCY_EXPORTS)
    video_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), video_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
