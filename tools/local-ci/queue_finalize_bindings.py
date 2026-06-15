"""Bindings from the local_ci facade to queue finalize/cleanup helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from queue_finalize_dependency_bindings import queue_finalize_dependencies


QUEUE_FINALIZE_EXPORTS = ("finalize_job",)


def finalize_job(bindings: Mapping[str, Any], job_id: str, result: dict, result_path: Path) -> None:
    _binding(bindings, "_queue_lifecycle").finalize_job_locked(
        job_id,
        result,
        result_path,
        **queue_finalize_dependencies(bindings),
    )


def install_queue_finalize_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_FINALIZE_EXPORTS,
) -> None:
    known_names = set(QUEUE_FINALIZE_EXPORTS)
    finalize_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), finalize_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
