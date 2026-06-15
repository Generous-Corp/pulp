"""Compatibility facade for target submission metadata bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from target_submission_build_bindings import (
    TARGET_SUBMISSION_BUILD_EXPORTS,
    build_submission_metadata,
    install_target_submission_build_helpers,
)
from target_submission_print_bindings import (
    TARGET_SUBMISSION_PRINT_EXPORTS,
    install_target_submission_print_helpers,
    print_submission_metadata,
)


TARGET_SUBMISSION_EXPORTS = (
    *TARGET_SUBMISSION_BUILD_EXPORTS,
    *TARGET_SUBMISSION_PRINT_EXPORTS,
)


def install_target_submission_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = TARGET_SUBMISSION_EXPORTS,
) -> None:
    known_names = set(TARGET_SUBMISSION_EXPORTS)
    build_names = tuple(name for name in names if name in TARGET_SUBMISSION_BUILD_EXPORTS)
    print_names = tuple(name for name in names if name in TARGET_SUBMISSION_PRINT_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_target_submission_build_helpers(bindings, build_names)
    install_target_submission_print_helpers(bindings, print_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
