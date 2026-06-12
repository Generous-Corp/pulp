"""Bindings from the local_ci facade to cloud GitHub helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def gh_available(bindings: Mapping[str, Any]) -> bool:
    return _binding(bindings, "_cloud").gh_available()


def gh_workflow_dispatch(
    bindings: Mapping[str, Any],
    repository: str,
    workflow_file: str,
    ref: str,
    fields: dict[str, str],
) -> None:
    return _binding(bindings, "_cloud").gh_workflow_dispatch(repository, workflow_file, ref, fields)


def gh_run_view(bindings: Mapping[str, Any], repository: str, run_id: int) -> dict | None:
    return _binding(bindings, "_cloud").gh_run_view(repository, run_id)


def gh_pr_create(bindings: Mapping[str, Any], branch: str, base: str = "main") -> int | None:
    return _binding(bindings, "_cloud").gh_pr_create(branch, base)


def gh_pr_comment(bindings: Mapping[str, Any], pr_number: int, body: str) -> bool:
    return _binding(bindings, "_cloud").gh_pr_comment(pr_number, body)


def gh_pr_merge(bindings: Mapping[str, Any], pr_number: int, method: str = "squash") -> bool:
    return _binding(bindings, "_cloud").gh_pr_merge(pr_number, method)


def gh_pr_list_open(bindings: Mapping[str, Any]) -> list[dict]:
    return _binding(bindings, "_cloud").gh_pr_list_open()


def gh_pr_head(bindings: Mapping[str, Any], pr_ref: str) -> tuple[int, str, str] | None:
    return _binding(bindings, "_cloud").gh_pr_head(pr_ref)
