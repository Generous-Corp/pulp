"""Bindings from the local_ci facade to desktop source manifest helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_SOURCE_REQUEST_MANIFEST_EXPORTS = ("attach_desktop_source_to_manifest",)


def attach_desktop_source_to_manifest(
    bindings: Mapping[str, Any],
    manifest: dict,
    source_context: dict | None,
) -> None:
    return _binding(bindings, "_source_prep").attach_desktop_source_to_manifest(manifest, source_context)


def install_desktop_source_request_manifest_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_SOURCE_REQUEST_MANIFEST_EXPORTS,
) -> None:
    known_names = set(DESKTOP_SOURCE_REQUEST_MANIFEST_EXPORTS)
    manifest_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), manifest_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
