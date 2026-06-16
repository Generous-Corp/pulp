"""Dependency helpers for queue target-state payload bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_target_log_path(bindings: Mapping[str, Any], job_id: str, target_name: str) -> str:
    return str(_binding(bindings, "target_log_path")(job_id, target_name))
