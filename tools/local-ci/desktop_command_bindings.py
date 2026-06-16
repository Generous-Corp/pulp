"""Compatibility facade for desktop command dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_action_command_bindings import (
    DESKTOP_ACTION_COMMAND_EXPORTS,
    cmd_desktop_click,
    cmd_desktop_inspect,
    cmd_desktop_smoke,
    install_desktop_action_command_helpers,
    windows_requires_pulp_app_selectors,
)
from desktop_management_command_bindings import (
    DESKTOP_MANAGEMENT_COMMAND_EXPORTS,
    cmd_desktop_cleanup,
    cmd_desktop_config_set,
    cmd_desktop_config_show,
    cmd_desktop_proof,
    cmd_desktop_publish,
    cmd_desktop_recent,
    cmd_desktop_status,
    install_desktop_management_command_helpers,
)
from desktop_review_command_bindings import (
    DESKTOP_REVIEW_COMMAND_EXPORTS,
    cmd_desktop_verdict,
    install_desktop_review_command_helpers,
)
from desktop_video_compose_command_bindings import (
    DESKTOP_VIDEO_COMPOSE_COMMAND_EXPORTS,
    cmd_desktop_compose_video,
    cmd_desktop_design_diff,
    cmd_desktop_design_proof,
    install_desktop_video_compose_command_helpers,
)
from desktop_serve_command_bindings import (
    DESKTOP_SERVE_COMMAND_EXPORTS,
    cmd_desktop_serve,
    install_desktop_serve_command_helpers,
)
from desktop_video_setup_command_bindings import (
    DESKTOP_VIDEO_SETUP_COMMAND_EXPORTS,
    cmd_desktop_video_doctor,
    cmd_desktop_video_setup,
    install_desktop_video_setup_command_helpers,
)
from desktop_video_action_command_bindings import (
    DESKTOP_VIDEO_ACTION_COMMAND_EXPORTS,
    cmd_desktop_video,
    install_desktop_video_action_command_helpers,
)
from desktop_video_info_command_bindings import (
    DESKTOP_VIDEO_INFO_COMMAND_EXPORTS,
    cmd_desktop_video_matrix,
    install_desktop_video_info_command_helpers,
)
from desktop_setup_command_bindings import (
    DESKTOP_SETUP_COMMAND_EXPORTS,
    cmd_desktop_doctor,
    cmd_desktop_install,
    install_desktop_setup_command_helpers,
)


DESKTOP_COMMAND_EXPORTS = (
    *DESKTOP_SETUP_COMMAND_EXPORTS,
    *DESKTOP_MANAGEMENT_COMMAND_EXPORTS,
    *DESKTOP_ACTION_COMMAND_EXPORTS,
    *DESKTOP_REVIEW_COMMAND_EXPORTS,
    *DESKTOP_VIDEO_COMPOSE_COMMAND_EXPORTS,
    *DESKTOP_VIDEO_INFO_COMMAND_EXPORTS,
    *DESKTOP_VIDEO_ACTION_COMMAND_EXPORTS,
    *DESKTOP_SERVE_COMMAND_EXPORTS,
    *DESKTOP_VIDEO_SETUP_COMMAND_EXPORTS,
)


def install_desktop_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_COMMAND_EXPORTS,
) -> None:
    setup_names = tuple(name for name in names if name in DESKTOP_SETUP_COMMAND_EXPORTS)
    management_names = tuple(name for name in names if name in DESKTOP_MANAGEMENT_COMMAND_EXPORTS)
    action_names = tuple(name for name in names if name in DESKTOP_ACTION_COMMAND_EXPORTS)
    review_names = tuple(name for name in names if name in DESKTOP_REVIEW_COMMAND_EXPORTS)
    compose_names = tuple(name for name in names if name in DESKTOP_VIDEO_COMPOSE_COMMAND_EXPORTS)
    info_names = tuple(name for name in names if name in DESKTOP_VIDEO_INFO_COMMAND_EXPORTS)
    video_action_names = tuple(name for name in names if name in DESKTOP_VIDEO_ACTION_COMMAND_EXPORTS)
    serve_names = tuple(name for name in names if name in DESKTOP_SERVE_COMMAND_EXPORTS)
    video_setup_names = tuple(name for name in names if name in DESKTOP_VIDEO_SETUP_COMMAND_EXPORTS)
    known_names = set(DESKTOP_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_setup_command_helpers(bindings, setup_names)
    install_desktop_management_command_helpers(bindings, management_names)
    install_desktop_action_command_helpers(bindings, action_names)
    install_desktop_review_command_helpers(bindings, review_names)
    install_desktop_video_compose_command_helpers(bindings, compose_names)
    install_desktop_video_info_command_helpers(bindings, info_names)
    install_desktop_video_action_command_helpers(bindings, video_action_names)
    install_desktop_serve_command_helpers(bindings, serve_names)
    install_desktop_video_setup_command_helpers(bindings, video_setup_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
