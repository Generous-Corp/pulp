"""Facade dependency bindings for SSH bundle host classification helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


SSH_BUNDLE_HOST_EXPORTS = (
    "target_name_for_ssh_host",
    "ssh_host_uses_windows_shell",
)


def target_name_for_ssh_host(bindings: dict, config: dict, host: str) -> str | None:
    for name, target_cfg in config.get("targets", {}).items():
        if name == host or target_cfg.get("host") == host:
            return name
    return None


def ssh_host_uses_windows_shell(bindings: dict, config: dict, host: str) -> bool:
    target_name = _binding(bindings, "target_name_for_ssh_host")(config, host)
    if target_name:
        target_cfg = dict(config.get("targets", {}).get(target_name, {}))
        repo_path = str(target_cfg.get("repo_path") or "")
        if target_name.lower().startswith("win") or "\\" in repo_path:
            return True
    return host.lower().startswith("win")


def install_ssh_bundle_host_helpers(
    bindings: dict,
    names: tuple[str, ...] = SSH_BUNDLE_HOST_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
