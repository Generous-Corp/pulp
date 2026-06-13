"""Compatibility facade for SSH bundle host and size-probe bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from ssh_bundle_host_bindings import (
    SSH_BUNDLE_HOST_EXPORTS,
    install_ssh_bundle_host_helpers,
    ssh_host_uses_windows_shell,
    target_name_for_ssh_host,
)
from ssh_bundle_size_probe_bindings import (
    SSH_BUNDLE_SIZE_PROBE_EXPORTS,
    install_ssh_bundle_size_probe_helpers,
    probe_uploaded_bundle_size,
)


SSH_BUNDLE_PROBE_EXPORTS = (
    *SSH_BUNDLE_HOST_EXPORTS,
    *SSH_BUNDLE_SIZE_PROBE_EXPORTS,
)


def install_ssh_bundle_probe_helpers(
    bindings: dict,
    names: tuple[str, ...] = SSH_BUNDLE_PROBE_EXPORTS,
) -> None:
    host_names = tuple(name for name in names if name in SSH_BUNDLE_HOST_EXPORTS)
    size_probe_names = tuple(name for name in names if name in SSH_BUNDLE_SIZE_PROBE_EXPORTS)
    known_names = set(SSH_BUNDLE_PROBE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_ssh_bundle_host_helpers(bindings, host_names)
    install_ssh_bundle_size_probe_helpers(bindings, size_probe_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
