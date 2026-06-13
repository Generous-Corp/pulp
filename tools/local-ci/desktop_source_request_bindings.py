"""Compatibility facade for desktop source request dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_source_request_core_bindings import (
    DESKTOP_SOURCE_REQUEST_CORE_EXPORTS,
    install_desktop_source_request_core_helpers,
    make_desktop_source_request,
)
from desktop_source_request_manifest_bindings import (
    DESKTOP_SOURCE_REQUEST_MANIFEST_EXPORTS,
    attach_desktop_source_to_manifest,
    install_desktop_source_request_manifest_helpers,
)
from desktop_source_request_path_bindings import (
    DESKTOP_SOURCE_REQUEST_PATH_EXPORTS,
    desktop_source_cache_key,
    desktop_source_root,
    install_desktop_source_request_path_helpers,
)
from desktop_source_request_windows_bindings import (
    DESKTOP_SOURCE_REQUEST_WINDOWS_EXPORTS,
    install_desktop_source_request_windows_helpers,
    split_windows_prepare_commands,
    validate_windows_prepare_commands,
)


DESKTOP_SOURCE_REQUEST_EXPORTS = (
    *DESKTOP_SOURCE_REQUEST_CORE_EXPORTS,
    *DESKTOP_SOURCE_REQUEST_PATH_EXPORTS,
    *DESKTOP_SOURCE_REQUEST_WINDOWS_EXPORTS,
    *DESKTOP_SOURCE_REQUEST_MANIFEST_EXPORTS,
)


def install_desktop_source_request_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_SOURCE_REQUEST_EXPORTS,
) -> None:
    core_names = tuple(name for name in names if name in DESKTOP_SOURCE_REQUEST_CORE_EXPORTS)
    path_names = tuple(name for name in names if name in DESKTOP_SOURCE_REQUEST_PATH_EXPORTS)
    windows_names = tuple(name for name in names if name in DESKTOP_SOURCE_REQUEST_WINDOWS_EXPORTS)
    manifest_names = tuple(name for name in names if name in DESKTOP_SOURCE_REQUEST_MANIFEST_EXPORTS)
    known_names = set(DESKTOP_SOURCE_REQUEST_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_source_request_core_helpers(bindings, core_names)
    install_desktop_source_request_path_helpers(bindings, path_names)
    install_desktop_source_request_windows_helpers(bindings, windows_names)
    install_desktop_source_request_manifest_helpers(bindings, manifest_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
