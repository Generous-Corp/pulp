"""Dependency bindings for SSH bundle naming helpers."""

from __future__ import annotations

from binding_utils import binding as _binding


SSH_BUNDLE_NAME_EXPORTS = (
    "bundle_ref_name",
    "remote_bundle_name",
)


def bundle_ref_name(bindings: dict, job_id: str) -> str:
    return _binding(bindings, "_ssh_bundle").bundle_ref_name(job_id)


def remote_bundle_name(bindings: dict, job_id: str) -> str:
    return _binding(bindings, "_ssh_bundle").remote_bundle_name(job_id)
