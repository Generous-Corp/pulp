"""Compatibility facade for desktop run manifest and rollup dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_run_manifest_bindings import (
    DESKTOP_RUN_MANIFEST_EXPORTS,
    desktop_rollup_dir,
    desktop_run_manifests,
    install_desktop_run_manifest_helpers,
)
from desktop_run_rollup_action_bindings import (
    DESKTOP_RUN_ROLLUP_ACTION_EXPORTS,
    install_desktop_run_rollup_action_helpers,
    prune_desktop_run_manifests,
    write_desktop_run_rollups,
)


DESKTOP_RUN_ROLLUP_EXPORTS = (
    *DESKTOP_RUN_MANIFEST_EXPORTS,
    *DESKTOP_RUN_ROLLUP_ACTION_EXPORTS,
)


def install_desktop_run_rollup_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_RUN_ROLLUP_EXPORTS,
) -> None:
    manifest_names = tuple(name for name in names if name in DESKTOP_RUN_MANIFEST_EXPORTS)
    action_names = tuple(name for name in names if name in DESKTOP_RUN_ROLLUP_ACTION_EXPORTS)
    known_names = set(DESKTOP_RUN_ROLLUP_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_run_manifest_helpers(bindings, manifest_names)
    install_desktop_run_rollup_action_helpers(bindings, action_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
