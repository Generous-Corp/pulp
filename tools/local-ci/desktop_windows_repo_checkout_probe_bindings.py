"""Bindings from the local_ci facade to Windows repo checkout probe helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_WINDOWS_REPO_CHECKOUT_PROBE_EXPORTS = ("probe_windows_repo_checkout",)


def probe_windows_repo_checkout(bindings: Mapping[str, Any], host: str, repo_path: str | None) -> dict:
    return _binding(bindings, "_windows_probe").probe_windows_repo_checkout(
        host,
        repo_path,
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        windows_repo_path_is_unsafe_fn=_binding(bindings, "windows_repo_path_is_unsafe"),
        parse_windows_ssh_json_fn=_binding(bindings, "parse_windows_ssh_json"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
    )


def install_desktop_windows_repo_checkout_probe_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_WINDOWS_REPO_CHECKOUT_PROBE_EXPORTS,
) -> None:
    known_names = set(DESKTOP_WINDOWS_REPO_CHECKOUT_PROBE_EXPORTS)
    probe_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), probe_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
