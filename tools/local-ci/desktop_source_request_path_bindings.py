"""Bindings from the local_ci facade to desktop source cache/root helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_SOURCE_REQUEST_PATH_EXPORTS = (
    "desktop_source_cache_key",
    "desktop_source_root",
)


def desktop_source_cache_key(bindings: Mapping[str, Any], source_request: dict) -> str:
    return _binding(bindings, "_source_prep").desktop_source_cache_key(source_request)


def desktop_source_root(bindings: Mapping[str, Any], target_name: str, source_request: dict) -> Path:
    return _binding(bindings, "_source_prep").desktop_source_root(
        target_name,
        source_request,
        state_dir_fn=_binding(bindings, "state_dir"),
    )


def install_desktop_source_request_path_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_SOURCE_REQUEST_PATH_EXPORTS,
) -> None:
    known_names = set(DESKTOP_SOURCE_REQUEST_PATH_EXPORTS)
    path_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), path_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
