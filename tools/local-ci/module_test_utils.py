#!/usr/bin/env python3
"""Shared helpers for local-ci module facade tests."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path
from types import ModuleType


def load_module_from_path(module_path: Path, *, module_name: str | None = None, add_module_dir: bool = False) -> ModuleType:
    name = module_name or f"{module_path.stem}_under_test"
    module_dir = str(module_path.parent)
    inserted_module_dir = False
    if add_module_dir and module_dir not in sys.path:
        sys.path.insert(0, module_dir)
        inserted_module_dir = True
    try:
        spec = importlib.util.spec_from_file_location(name, module_path)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        return module
    finally:
        if inserted_module_dir:
            sys.path.pop(0)
