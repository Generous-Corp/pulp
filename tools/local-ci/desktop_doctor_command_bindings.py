"""Bindings from the local_ci facade to desktop doctor command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def cmd_desktop_doctor(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_setup_commands_cli").cmd_desktop_doctor(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        resolve_desktop_target_fn=_binding(bindings, "resolve_desktop_target"),
        desktop_doctor_checks_fn=_binding(bindings, "desktop_doctor_checks"),
    )
