"""Bindings from the local_ci facade to cloud GitHub PR helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLOUD_GITHUB_PR_EXPORTS = (
    "gh_pr_create",
    "gh_pr_comment",
    "gh_pr_merge",
    "gh_pr_list_open",
    "gh_pr_head",
)


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


def install_cloud_github_pr_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_GITHUB_PR_EXPORTS,
) -> None:
    known_names = set(CLOUD_GITHUB_PR_EXPORTS)
    pr_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), pr_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
