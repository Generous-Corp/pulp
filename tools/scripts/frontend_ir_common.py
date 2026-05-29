#!/usr/bin/env python3
"""Shared helpers for the frontend-IR tooling.

These small utilities were previously copy-pasted (byte-identically) across a
dozen frontend_ir_* scripts. Centralizing them here removes the drift risk and
keeps the I/O / type-coercion contract in one place. Route-taxonomy and schema
validation live in frontend_ir_validation.py; this module is generic.
"""

from __future__ import annotations

import json
import pathlib
from typing import Any


def as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def non_negative_int(value: Any) -> int:
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else 0


def load_json(path: pathlib.Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return data


def load_json_any(path: pathlib.Path) -> dict[str, Any] | list[Any]:
    """Like load_json but also accepts a top-level JSON array."""
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, (dict, list)):
        raise ValueError(f"{path} must contain a JSON object or array")
    return data


def write_json(path: pathlib.Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
