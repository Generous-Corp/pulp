"""Bindings from the local_ci facade to desktop source request creation helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_SOURCE_REQUEST_CORE_EXPORTS = ("make_desktop_source_request",)


def make_desktop_source_request(bindings: Mapping[str, Any], args: Any) -> dict:
    return _binding(bindings, "_source_prep").make_desktop_source_request(
        args,
        normalize_desktop_source_mode_fn=_binding(bindings, "normalize_desktop_source_mode"),
        current_branch_fn=_binding(bindings, "current_branch"),
        current_sha_fn=_binding(bindings, "current_sha"),
    )


def install_desktop_source_request_core_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_SOURCE_REQUEST_CORE_EXPORTS,
) -> None:
    known_names = set(DESKTOP_SOURCE_REQUEST_CORE_EXPORTS)
    core_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), core_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
