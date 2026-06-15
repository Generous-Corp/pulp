"""Dependency assembly for queue job construction bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def queue_job_factory_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "now_iso_fn": _binding(bindings, "now_iso"),
        "uuid_hex_fn": lambda: _binding_attr(bindings, "uuid", "uuid4")().hex,
        "root": _binding(bindings, "ROOT"),
        "validate_branch_fn": _binding(bindings, "validate_ci_branch_name"),
    }
