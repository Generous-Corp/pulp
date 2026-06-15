"""Compatibility facade for generic desktop support dependency bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from desktop_action_support_bindings import (
    DESKTOP_ACTION_SUPPORT_EXPORTS,
    count_view_tree_nodes,
    default_desktop_label,
    install_desktop_action_support_helpers,
    iter_view_tree_nodes,
    parse_coordinate_pair,
    resolve_desktop_target,
    resolve_view_tree_click_point,
    screen_point_for_content_point,
)
from desktop_artifact_bindings import (
    DESKTOP_ARTIFACT_EXPORTS,
    create_desktop_publish_bundle,
    create_desktop_run_bundle,
    desktop_artifact_root,
    desktop_publish_root,
    desktop_receipt_for,
    desktop_target_receipt_path,
    install_desktop_artifact_helpers,
)
from desktop_doctor_bindings import (
    DESKTOP_DOCTOR_EXPORTS,
    check_writable_dir,
    desktop_capabilities_for,
    desktop_check,
    desktop_optional_capabilities,
    install_desktop_doctor_helpers,
    webdriver_status_url,
)


DESKTOP_SUPPORT_EXPORTS = (
    *DESKTOP_ARTIFACT_EXPORTS,
    *DESKTOP_DOCTOR_EXPORTS,
    *DESKTOP_ACTION_SUPPORT_EXPORTS,
)


def install_desktop_support_helpers(bindings: dict, names: tuple[str, ...] = DESKTOP_SUPPORT_EXPORTS) -> None:
    artifact_names = tuple(name for name in names if name in DESKTOP_ARTIFACT_EXPORTS)
    doctor_names = tuple(name for name in names if name in DESKTOP_DOCTOR_EXPORTS)
    action_names = tuple(name for name in names if name in DESKTOP_ACTION_SUPPORT_EXPORTS)
    known_names = set(DESKTOP_SUPPORT_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_artifact_helpers(bindings, artifact_names)
    install_desktop_doctor_helpers(bindings, doctor_names)
    install_desktop_action_support_helpers(bindings, action_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
