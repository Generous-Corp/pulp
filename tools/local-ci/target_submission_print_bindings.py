"""Facade bindings for target submission metadata printing."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from binding_utils import print_binding as _print_binding


TARGET_SUBMISSION_PRINT_EXPORTS = (
    "print_submission_metadata",
)


def print_submission_metadata(bindings: Mapping[str, Any], metadata: dict) -> None:
    return _binding(bindings, "_target_preflight").print_submission_metadata(
        metadata,
        short_sha_fn=_binding(bindings, "short_sha"),
        provenance_summary_fn=_binding(bindings, "provenance_summary"),
        print_fn=_print_binding(bindings),
    )


def install_target_submission_print_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = TARGET_SUBMISSION_PRINT_EXPORTS,
) -> None:
    known_names = set(TARGET_SUBMISSION_PRINT_EXPORTS)
    print_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), print_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
