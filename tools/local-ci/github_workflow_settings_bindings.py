"""Bindings from the local_ci facade to GitHub workflow settings helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def github_actions_settings_for_display(bindings: Mapping[str, Any], config: dict | None) -> dict:
    return _binding(bindings, "_github_workflows").github_actions_settings_for_display(config)


def resolve_github_actions_settings(bindings: Mapping[str, Any], config: dict | None) -> dict:
    return _binding(bindings, "_github_workflows").resolve_github_actions_settings(config)


def normalize_runs_on_json(bindings: Mapping[str, Any], raw: str, *, setting_name: str) -> str:
    return _binding(bindings, "_github_workflows").normalize_runs_on_json(raw, setting_name=setting_name)
