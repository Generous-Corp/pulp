"""Bindings from the local_ci facade to CLI parser construction."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLI_PARSER_EXPORTS = (
    "build_local_ci_parser",
    "build_parser",
)


def build_local_ci_parser(
    bindings: Mapping[str, Any],
    *,
    priority_values: dict,
    keep_completed_jobs: int,
    epilog: str | None,
):
    return _binding(bindings, "_cli_parser").build_local_ci_parser(
        priority_values=priority_values,
        keep_completed_jobs=keep_completed_jobs,
        epilog=epilog,
    )


def build_parser(bindings: Mapping[str, Any]):
    return build_local_ci_parser(
        bindings,
        priority_values=_binding(bindings, "PRIORITY_VALUES"),
        keep_completed_jobs=_binding(bindings, "KEEP_COMPLETED_JOBS"),
        epilog=_binding(bindings, "__doc__"),
    )


def install_cli_parser_helpers(bindings: dict[str, Any], names: tuple[str, ...] = CLI_PARSER_EXPORTS) -> None:
    known_names = set(CLI_PARSER_EXPORTS)
    parser_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), parser_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
