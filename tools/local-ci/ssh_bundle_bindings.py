"""Compatibility installer for SSH bundle facade helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from ssh_bundle_core_bindings import (
    bundle_ref_name,
    config_for_bundle_probe,
    create_job_bundle,
    remote_bundle_name,
    sync_job_bundle_to_ssh_host,
)
from ssh_bundle_probe_bindings import (
    probe_uploaded_bundle_size,
    ssh_host_uses_windows_shell,
    target_name_for_ssh_host,
)


SSH_BUNDLE_EXPORTS = (
    "bundle_ref_name",
    "remote_bundle_name",
    "create_job_bundle",
    "config_for_bundle_probe",
    "sync_job_bundle_to_ssh_host",
    "target_name_for_ssh_host",
    "ssh_host_uses_windows_shell",
    "probe_uploaded_bundle_size",
)


def install_ssh_bundle_helpers(bindings: dict, names: tuple[str, ...] = SSH_BUNDLE_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
