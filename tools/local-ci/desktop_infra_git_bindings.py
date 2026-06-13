"""Compatibility facade for desktop git infrastructure bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_infra_git_origin_bindings import (
    DESKTOP_INFRA_GIT_ORIGIN_EXPORTS,
    git_origin_clone_url,
    git_origin_http_url,
    install_desktop_infra_git_origin_helpers,
)
from desktop_infra_git_remote_bindings import (
    DESKTOP_INFRA_GIT_REMOTE_EXPORTS,
    install_desktop_infra_git_remote_helpers,
    normalize_git_remote_for_clone,
    normalize_git_remote_for_http,
)
from desktop_infra_git_run_bindings import (
    DESKTOP_INFRA_GIT_RUN_EXPORTS,
    install_desktop_infra_git_run_helpers,
    run_git,
)


DESKTOP_INFRA_GIT_EXPORTS = (
    *DESKTOP_INFRA_GIT_REMOTE_EXPORTS,
    *DESKTOP_INFRA_GIT_ORIGIN_EXPORTS,
    *DESKTOP_INFRA_GIT_RUN_EXPORTS,
)


def install_desktop_infra_git_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_INFRA_GIT_EXPORTS,
) -> None:
    remote_names = tuple(name for name in names if name in DESKTOP_INFRA_GIT_REMOTE_EXPORTS)
    origin_names = tuple(name for name in names if name in DESKTOP_INFRA_GIT_ORIGIN_EXPORTS)
    run_names = tuple(name for name in names if name in DESKTOP_INFRA_GIT_RUN_EXPORTS)
    known_names = set(DESKTOP_INFRA_GIT_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_infra_git_remote_helpers(bindings, remote_names)
    install_desktop_infra_git_origin_helpers(bindings, origin_names)
    install_desktop_infra_git_run_helpers(bindings, run_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
