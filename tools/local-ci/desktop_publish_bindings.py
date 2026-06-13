"""Compatibility composer for desktop publish dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_publish_branch_bindings import (
    DESKTOP_PUBLISH_BRANCH_EXPORTS,
    install_desktop_publish_branch_helpers,
    publish_report_to_branch,
)
from desktop_publish_stage_bindings import (
    DESKTOP_PUBLISH_STAGE_EXPORTS,
    install_desktop_publish_stage_helpers,
    stage_desktop_publish_report,
)
from desktop_publish_list_bindings import (
    DESKTOP_PUBLISH_LIST_EXPORTS,
    desktop_publish_reports,
    install_desktop_publish_list_helpers,
    write_desktop_publish_rollups,
)


DESKTOP_PUBLISH_EXPORTS = (
    *DESKTOP_PUBLISH_BRANCH_EXPORTS,
    *DESKTOP_PUBLISH_STAGE_EXPORTS,
    *DESKTOP_PUBLISH_LIST_EXPORTS,
)


def install_desktop_publish_helpers(bindings: dict[str, Any], names: tuple[str, ...] = DESKTOP_PUBLISH_EXPORTS) -> None:
    branch_names = tuple(name for name in names if name in DESKTOP_PUBLISH_BRANCH_EXPORTS)
    stage_names = tuple(name for name in names if name in DESKTOP_PUBLISH_STAGE_EXPORTS)
    list_names = tuple(name for name in names if name in DESKTOP_PUBLISH_LIST_EXPORTS)
    known_names = set(DESKTOP_PUBLISH_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_publish_branch_helpers(bindings, branch_names)
    install_desktop_publish_stage_helpers(bindings, stage_names)
    install_desktop_publish_list_helpers(bindings, list_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
