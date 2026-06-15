"""Compatibility installer for SSH bundle facade helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from ssh_bundle_build_bindings import (
    SSH_BUNDLE_BUILD_EXPORTS,
    config_for_bundle_probe,
    create_job_bundle,
)
from ssh_bundle_name_bindings import SSH_BUNDLE_NAME_EXPORTS, bundle_ref_name, remote_bundle_name
from ssh_bundle_probe_bindings import (
    SSH_BUNDLE_PROBE_EXPORTS,
    install_ssh_bundle_probe_helpers,
    probe_uploaded_bundle_size,
    ssh_host_uses_windows_shell,
    target_name_for_ssh_host,
)
from ssh_bundle_sync_bindings import SSH_BUNDLE_SYNC_EXPORTS, sync_job_bundle_to_ssh_host


SSH_BUNDLE_EXPORTS = (
    *SSH_BUNDLE_NAME_EXPORTS,
    *SSH_BUNDLE_BUILD_EXPORTS,
    *SSH_BUNDLE_SYNC_EXPORTS,
    *SSH_BUNDLE_PROBE_EXPORTS,
)
SSH_BUNDLE_LOCAL_EXPORTS = (
    *SSH_BUNDLE_NAME_EXPORTS,
    *SSH_BUNDLE_BUILD_EXPORTS,
    *SSH_BUNDLE_SYNC_EXPORTS,
)


def install_ssh_bundle_helpers(bindings: dict, names: tuple[str, ...] = SSH_BUNDLE_EXPORTS) -> None:
    probe_names = tuple(name for name in names if name in SSH_BUNDLE_PROBE_EXPORTS)
    local_names = tuple(name for name in names if name in SSH_BUNDLE_LOCAL_EXPORTS)
    known_names = set(SSH_BUNDLE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), local_names)
    install_ssh_bundle_probe_helpers(bindings, probe_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
