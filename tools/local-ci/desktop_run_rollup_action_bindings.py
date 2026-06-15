"""Compatibility facade for desktop run rollup write/prune dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_run_rollup_prune_bindings import (
    DESKTOP_RUN_ROLLUP_PRUNE_EXPORTS,
    install_desktop_run_rollup_prune_helpers,
    prune_desktop_run_manifests,
)
from desktop_run_rollup_write_bindings import (
    DESKTOP_RUN_ROLLUP_WRITE_EXPORTS,
    install_desktop_run_rollup_write_helpers,
    write_desktop_run_rollups,
)


DESKTOP_RUN_ROLLUP_ACTION_EXPORTS = (
    *DESKTOP_RUN_ROLLUP_WRITE_EXPORTS,
    *DESKTOP_RUN_ROLLUP_PRUNE_EXPORTS,
)


def install_desktop_run_rollup_action_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_RUN_ROLLUP_ACTION_EXPORTS,
) -> None:
    write_names = tuple(name for name in names if name in DESKTOP_RUN_ROLLUP_WRITE_EXPORTS)
    prune_names = tuple(name for name in names if name in DESKTOP_RUN_ROLLUP_PRUNE_EXPORTS)
    known_names = set(DESKTOP_RUN_ROLLUP_ACTION_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_run_rollup_write_helpers(bindings, write_names)
    install_desktop_run_rollup_prune_helpers(bindings, prune_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
