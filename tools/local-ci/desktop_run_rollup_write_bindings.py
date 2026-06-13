"""Bindings from the local_ci facade to desktop run rollup write helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_RUN_ROLLUP_WRITE_EXPORTS = ("write_desktop_run_rollups",)


def write_desktop_run_rollups(
    bindings: Mapping[str, Any],
    config: dict,
    *,
    target_name: str | None = None,
) -> None:
    return _binding(bindings, "_reporting").write_desktop_run_rollups(
        config,
        target_name=target_name,
        desktop_rollup_dir_fn=_binding(bindings, "desktop_rollup_dir"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        desktop_run_summary_fn=_binding(bindings, "desktop_run_summary"),
        desktop_proof_summaries_fn=_binding(bindings, "desktop_proof_summaries"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def install_desktop_run_rollup_write_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_RUN_ROLLUP_WRITE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
