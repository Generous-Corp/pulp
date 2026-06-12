#!/usr/bin/env python3
"""Shared helpers for local-ci module facade tests."""

from __future__ import annotations

import importlib.util
from pathlib import Path
from types import ModuleType


def load_module_from_path(module_path: Path) -> ModuleType:
    spec = importlib.util.spec_from_file_location(f"{module_path.stem}_under_test", module_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module
