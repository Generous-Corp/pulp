"""Compatibility facade for core state path dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from state_path_config_bindings import (
    STATE_PATH_CONFIG_EXPORTS,
    config_path,
    install_state_path_config_helpers,
    shared_config_path,
    state_dir,
    worktree_config_path,
)
from state_path_store_bindings import (
    STATE_PATH_STORE_EXPORTS,
    cloud_runs_dir,
    ensure_state_dirs,
    evidence_path,
    install_state_path_store_helpers,
    logs_dir,
    queue_path,
    results_dir,
)


STATE_PATH_CORE_EXPORTS = (
    *STATE_PATH_CONFIG_EXPORTS,
    *STATE_PATH_STORE_EXPORTS,
)


def install_state_path_core_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = STATE_PATH_CORE_EXPORTS,
) -> None:
    config_names = tuple(name for name in names if name in STATE_PATH_CONFIG_EXPORTS)
    store_names = tuple(name for name in names if name in STATE_PATH_STORE_EXPORTS)
    known_names = set(STATE_PATH_CORE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_state_path_config_helpers(bindings, config_names)
    install_state_path_store_helpers(bindings, store_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
