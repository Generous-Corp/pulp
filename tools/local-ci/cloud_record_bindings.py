"""Bindings from the local_ci facade to cloud record and formatting helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLOUD_RECORD_EXPORTS = (
    "list_cloud_records",
    "cloud_record_summary",
    "format_ci_comment",
    "open_pr_list_lines",
)


def list_cloud_records(bindings: Mapping[str, Any], limit: int | None = None) -> list[dict]:
    return _binding(bindings, "_cloud").list_cloud_records(limit=limit)


def cloud_record_summary(bindings: Mapping[str, Any], record: dict, config: dict | None = None) -> str:
    return _binding(bindings, "_cloud").cloud_record_summary(record, config)


def format_ci_comment(bindings: Mapping[str, Any], result: dict) -> str:
    return _binding(bindings, "_cloud").format_ci_comment(result)


def open_pr_list_lines(bindings: Mapping[str, Any], prs: list[dict]) -> list[str]:
    return _binding(bindings, "_cloud").open_pr_list_lines(prs)


def install_cloud_record_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_RECORD_EXPORTS,
) -> None:
    known_names = set(CLOUD_RECORD_EXPORTS)
    record_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), record_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
