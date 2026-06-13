"""Compatibility facade for focused SSH bundle dependency bindings."""

from __future__ import annotations

from ssh_bundle_build_bindings import SSH_BUNDLE_BUILD_EXPORTS, config_for_bundle_probe, create_job_bundle
from ssh_bundle_name_bindings import SSH_BUNDLE_NAME_EXPORTS, bundle_ref_name, remote_bundle_name
from ssh_bundle_sync_bindings import SSH_BUNDLE_SYNC_EXPORTS, sync_job_bundle_to_ssh_host


SSH_BUNDLE_CORE_EXPORTS = (
    *SSH_BUNDLE_NAME_EXPORTS,
    *SSH_BUNDLE_BUILD_EXPORTS,
    *SSH_BUNDLE_SYNC_EXPORTS,
)
