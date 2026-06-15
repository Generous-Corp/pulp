"""Bindings from the local_ci facade to desktop branch-publish helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_PUBLISH_BRANCH_EXPORTS = ("publish_report_to_branch",)


def publish_report_to_branch(bindings: Mapping[str, Any], config: dict, report: dict) -> dict:
    return _binding(bindings, "_reporting").publish_report_to_branch(
        config,
        report,
        root=_binding(bindings, "ROOT"),
        run_git_fn=_binding(bindings, "_run_git"),
        reset_local_worktree_fn=_binding(bindings, "_reset_local_worktree"),
        clear_directory_contents_fn=_binding(bindings, "_clear_directory_contents"),
        git_origin_http_url_fn=_binding(bindings, "git_origin_http_url"),
    )


def install_desktop_publish_branch_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_PUBLISH_BRANCH_EXPORTS,
) -> None:
    known_names = set(DESKTOP_PUBLISH_BRANCH_EXPORTS)
    branch_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), branch_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
