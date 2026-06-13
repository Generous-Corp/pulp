"""Facade dependency bindings for config file helpers."""

from __future__ import annotations

import os
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CONFIG_FILE_EXPORTS = (
    "load_config",
    "load_config_file",
    "load_optional_config",
    "save_config",
)


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


def save_config(bindings: Mapping[str, Any], config: dict) -> None:
    _binding(bindings, "atomic_write_text")(
        _binding(bindings, "config_path")(),
        _binding(bindings, "json").dumps(config, indent=2) + "\n",
    )


def install_config_file_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CONFIG_FILE_EXPORTS,
) -> None:
    known_names = set(CONFIG_FILE_EXPORTS)
    config_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), config_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
