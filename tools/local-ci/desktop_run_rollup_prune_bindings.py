"""Bindings from the local_ci facade to desktop run manifest prune helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_RUN_ROLLUP_PRUNE_EXPORTS = ("prune_desktop_run_manifests",)


def prune_desktop_run_manifests(
    bindings: Mapping[str, Any],
    config: dict,
    *,
    target_name: str | None = None,
    older_than_days: int | None = None,
    keep_last: int | None = None,
) -> list[Path]:
    return _binding(bindings, "_reporting").prune_desktop_run_manifests(
        config,
        target_name=target_name,
        older_than_days=older_than_days,
        keep_last=keep_last,
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
    )


def install_desktop_run_rollup_prune_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_RUN_ROLLUP_PRUNE_EXPORTS,
) -> None:
    known_names = set(DESKTOP_RUN_ROLLUP_PRUNE_EXPORTS)
    prune_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), prune_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
