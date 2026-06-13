"""Compatibility facade for desktop artifact helper bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from desktop_publish_artifact_bindings import (
    DESKTOP_PUBLISH_ARTIFACT_EXPORTS,
    create_desktop_publish_bundle,
    desktop_publish_root,
    install_desktop_publish_artifact_helpers,
)
from desktop_receipt_artifact_bindings import (
    DESKTOP_RECEIPT_ARTIFACT_EXPORTS,
    desktop_receipt_for,
    desktop_target_receipt_path,
    install_desktop_receipt_artifact_helpers,
)
from desktop_run_artifact_bindings import (
    DESKTOP_RUN_ARTIFACT_EXPORTS,
    create_desktop_run_bundle,
    desktop_artifact_root,
    install_desktop_run_artifact_helpers,
)


DESKTOP_ARTIFACT_EXPORTS = (
    *DESKTOP_RECEIPT_ARTIFACT_EXPORTS,
    *DESKTOP_RUN_ARTIFACT_EXPORTS,
    *DESKTOP_PUBLISH_ARTIFACT_EXPORTS,
)


def install_desktop_artifact_helpers(
    bindings: dict,
    names: tuple[str, ...] = DESKTOP_ARTIFACT_EXPORTS,
) -> None:
    receipt_names = tuple(name for name in names if name in DESKTOP_RECEIPT_ARTIFACT_EXPORTS)
    run_names = tuple(name for name in names if name in DESKTOP_RUN_ARTIFACT_EXPORTS)
    publish_names = tuple(name for name in names if name in DESKTOP_PUBLISH_ARTIFACT_EXPORTS)
    known_names = set(DESKTOP_ARTIFACT_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_receipt_artifact_helpers(bindings, receipt_names)
    install_desktop_run_artifact_helpers(bindings, run_names)
    install_desktop_publish_artifact_helpers(bindings, publish_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
