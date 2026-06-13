"""Compatibility composer for cloud GitHub facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from cloud_github_pr_bindings import (
    CLOUD_GITHUB_PR_EXPORTS,
    gh_pr_comment,
    gh_pr_create,
    gh_pr_head,
    gh_pr_list_open,
    gh_pr_merge,
    install_cloud_github_pr_helpers,
)
from cloud_github_workflow_bindings import (
    CLOUD_GITHUB_WORKFLOW_EXPORTS,
    gh_available,
    gh_run_view,
    gh_workflow_dispatch,
    install_cloud_github_workflow_helpers,
)


CLOUD_GITHUB_EXPORTS = (
    *CLOUD_GITHUB_WORKFLOW_EXPORTS,
    *CLOUD_GITHUB_PR_EXPORTS,
)


def install_cloud_github_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_GITHUB_EXPORTS,
) -> None:
    workflow_names = tuple(name for name in names if name in CLOUD_GITHUB_WORKFLOW_EXPORTS)
    pr_names = tuple(name for name in names if name in CLOUD_GITHUB_PR_EXPORTS)
    known_names = set(CLOUD_GITHUB_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_cloud_github_workflow_helpers(bindings, workflow_names)
    install_cloud_github_pr_helpers(bindings, pr_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
