"""Facade dependency bindings for config and evidence helpers."""

from __future__ import annotations

import os
from collections.abc import Mapping
from pathlib import Path
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def load_config(bindings: Mapping[str, Any]) -> dict:
    return load_config_file(bindings, _binding(bindings, "config_path")())


def load_config_file(bindings: Mapping[str, Any], path: str | os.PathLike[str]) -> dict:
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(
            f"Local CI config not found at {path}. Copy tools/local-ci/config.example.json first."
        )
    return _binding(bindings, "normalize_desktop_config")(
        _binding(bindings, "json").loads(path.read_text())
    )


def load_optional_config(bindings: Mapping[str, Any]) -> dict | None:
    path = _binding(bindings, "config_path")()
    if not path.exists():
        return None
    return _binding(bindings, "json").loads(path.read_text())


def load_evidence_index(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "evidence_index_module").load_evidence_index()


def update_evidence_index(bindings: Mapping[str, Any], result: dict, result_path: Path) -> None:
    return _binding(bindings, "evidence_index_module").update_evidence_index(result, result_path)


def collect_evidence_groups(
    bindings: Mapping[str, Any],
    branch: str | None = None,
    sha: str | None = None,
) -> dict[str, list[dict]]:
    return _binding(bindings, "collect_evidence_groups_from_index")(
        _binding(bindings, "load_evidence_index")(),
        branch=branch,
        sha=sha,
    )


def print_evidence_summary(
    bindings: Mapping[str, Any],
    *,
    branch: str | None = None,
    sha: str | None = None,
    limit: int = 3,
    indent: str = "",
) -> bool:
    return _binding(bindings, "evidence_index_module").print_evidence_summary_from_groups(
        _binding(bindings, "collect_evidence_groups")(branch=branch, sha=sha),
        limit=limit,
        indent=indent,
    )


def evidence_scope_header_line(bindings: Mapping[str, Any], branch: str | None, sha: str | None) -> str | None:
    return _binding(bindings, "evidence_index_module").evidence_scope_header_line(branch, sha)


def evidence_empty_line(bindings: Mapping[str, Any], *, has_header: bool) -> str:
    return _binding(bindings, "evidence_index_module").evidence_empty_line(has_header=has_header)


def save_config(bindings: Mapping[str, Any], config: dict) -> None:
    _binding(bindings, "atomic_write_text")(
        _binding(bindings, "config_path")(),
        _binding(bindings, "json").dumps(config, indent=2) + "\n",
    )
