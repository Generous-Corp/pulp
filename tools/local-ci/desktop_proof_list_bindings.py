"""Bindings from the local_ci facade to desktop proof listing helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_PROOF_LIST_EXPORTS = ("desktop_proof_summaries",)


def desktop_proof_summaries(
    bindings: Mapping[str, Any],
    config: dict,
    *,
    target_name: str | None = None,
    action: str | None = None,
    source_mode: str | None = None,
    sha: str | None = None,
    branch: str | None = None,
    limit: int | None = None,
) -> list[dict]:
    return _binding(bindings, "_reporting").desktop_proof_summaries(
        config,
        target_name=target_name,
        action=action,
        source_mode=source_mode,
        sha=sha,
        branch=branch,
        limit=limit,
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        desktop_run_summary_fn=_binding(bindings, "desktop_run_summary"),
    )


def install_desktop_proof_list_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_PROOF_LIST_EXPORTS,
) -> None:
    known_names = set(DESKTOP_PROOF_LIST_EXPORTS)
    list_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), list_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
