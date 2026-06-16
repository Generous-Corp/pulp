"""Bindings from the local_ci facade to the desktop review verdict command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


DESKTOP_REVIEW_COMMAND_EXPORTS = (
    "cmd_desktop_verdict",
    "cmd_desktop_review_issue",
    "cmd_desktop_review_status",
    "cmd_desktop_review_watch",
)


def cmd_desktop_verdict(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_review_commands_cli").cmd_desktop_verdict(
        args,
        now_iso_fn=_binding(bindings, "now_iso"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def _desktop_review_issue_draft(bindings: Mapping[str, Any], review_package: dict, **kwargs) -> dict:
    return _binding(bindings, "_reporting_review").desktop_review_issue_draft(review_package, **kwargs)


def cmd_desktop_review_issue(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_review_commands_cli").cmd_desktop_review_issue(
        args,
        desktop_review_issue_draft_fn=lambda review_package, **kwargs: _desktop_review_issue_draft(
            bindings, review_package, **kwargs
        ),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def cmd_desktop_review_status(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_review_commands_cli").cmd_desktop_review_status(
        args,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def cmd_desktop_review_watch(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_review_commands_cli").cmd_desktop_review_watch(
        args,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )


def install_desktop_review_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_REVIEW_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_REVIEW_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
