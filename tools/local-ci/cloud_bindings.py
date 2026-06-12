"""Bindings from the local_ci facade to cloud/GitHub helpers."""

from __future__ import annotations

from collections.abc import Mapping
from functools import update_wrapper
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def cmd_cloud_workflows(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_workflows(args)


def cmd_cloud_defaults(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_defaults(args)


def cmd_cloud_history(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_history(args)


def cmd_cloud_compare(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_compare(args)


def cmd_cloud_recommend(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_recommend(args)


def cmd_cloud_run(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_run(args)


def cmd_cloud_status(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_status(args)


def cmd_cloud_namespace_doctor(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_namespace_doctor(args)


def cmd_cloud_namespace_setup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_namespace_setup(args)


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


def list_cloud_records(bindings: Mapping[str, Any], limit: int | None = None) -> list[dict]:
    return _binding(bindings, "_cloud").list_cloud_records(limit=limit)


def cloud_record_summary(bindings: Mapping[str, Any], record: dict, config: dict | None = None) -> str:
    return _binding(bindings, "_cloud").cloud_record_summary(record, config)


def format_ci_comment(bindings: Mapping[str, Any], result: dict) -> str:
    return _binding(bindings, "_cloud").format_ci_comment(result)


def open_pr_list_lines(bindings: Mapping[str, Any], prs: list[dict]) -> list[str]:
    return _binding(bindings, "_cloud").open_pr_list_lines(prs)


def bind_cloud_helper(bindings: Mapping[str, Any], name: str):
    helper = getattr(_binding(bindings, "_cloud"), name)

    def _helper(*args, **kwargs):
        return getattr(_binding(bindings, "_cloud"), name)(*args, **kwargs)

    return update_wrapper(_helper, helper)
