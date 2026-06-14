"""Compatibility facade for queue job policy helpers."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_job_factory_bindings import (
    QUEUE_JOB_FACTORY_EXPORTS,
    install_queue_job_factory_helpers,
    make_job,
)
from queue_job_identity_policy_bindings import (
    QUEUE_JOB_IDENTITY_POLICY_EXPORTS,
    default_priority_for,
    install_queue_job_identity_policy_helpers,
    make_fingerprint,
    validate_ci_branch_name,
)


QUEUE_JOB_POLICY_EXPORTS = (
    *QUEUE_JOB_IDENTITY_POLICY_EXPORTS[:2],
    *QUEUE_JOB_FACTORY_EXPORTS,
    QUEUE_JOB_IDENTITY_POLICY_EXPORTS[2],
)


def install_queue_job_policy_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_JOB_POLICY_EXPORTS,
) -> None:
    identity_names = tuple(name for name in names if name in QUEUE_JOB_IDENTITY_POLICY_EXPORTS)
    factory_names = tuple(name for name in names if name in QUEUE_JOB_FACTORY_EXPORTS)
    known_names = set(QUEUE_JOB_POLICY_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_job_identity_policy_helpers(bindings, identity_names)
    install_queue_job_factory_helpers(bindings, factory_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
