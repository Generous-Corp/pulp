"""Compatibility facade for desktop infrastructure dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_infra_git_bindings import (
    DESKTOP_INFRA_GIT_EXPORTS,
    git_origin_clone_url,
    git_origin_http_url,
    install_desktop_infra_git_helpers,
    normalize_git_remote_for_clone,
    normalize_git_remote_for_http,
    run_git,
)
from desktop_infra_reporting_bindings import (
    DESKTOP_INFRA_REPORTING_EXPORTS,
    clear_directory_contents,
    copy_directory_contents,
    install_desktop_infra_reporting_helpers,
    slugify_token,
)
from desktop_infra_wait_bindings import (
    DESKTOP_INFRA_WAIT_EXPORTS,
    install_desktop_infra_wait_helpers,
    wait_for_path,
)


DESKTOP_INFRA_EXPORTS = (
    *DESKTOP_INFRA_GIT_EXPORTS,
    *DESKTOP_INFRA_REPORTING_EXPORTS,
    *DESKTOP_INFRA_WAIT_EXPORTS,
)


def install_desktop_infra_helpers(bindings: dict[str, Any], names: tuple[str, ...] = DESKTOP_INFRA_EXPORTS) -> None:
    git_names = tuple(name for name in names if name in DESKTOP_INFRA_GIT_EXPORTS)
    reporting_names = tuple(name for name in names if name in DESKTOP_INFRA_REPORTING_EXPORTS)
    wait_names = tuple(name for name in names if name in DESKTOP_INFRA_WAIT_EXPORTS)
    known_names = set(DESKTOP_INFRA_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_infra_git_helpers(bindings, git_names)
    install_desktop_infra_reporting_helpers(bindings, reporting_names)
    install_desktop_infra_wait_helpers(bindings, wait_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
