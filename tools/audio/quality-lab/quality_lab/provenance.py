"""Provenance — make a generated set re-derivable (§7.1).

Records what cheaply maps a liked sample back to how it was made: engine commit,
recipe, versions, determinism context. This is the "same recipe reproduces the sound"
tier; strict bit-for-bit is a separate, stricter tier (§7.1) we do not promise here.
"""
from __future__ import annotations

import subprocess
from typing import Any

from .schema import SCHEMA_VERSION


def _git(*args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", *args], stderr=subprocess.DEVNULL, text=True
        ).strip()
    except Exception:
        return ""


def engine_commit() -> dict[str, Any]:
    sha = _git("rev-parse", "HEAD")
    dirty = bool(_git("status", "--porcelain"))
    describe = _git("describe", "--always", "--dirty")
    return {"sha": sha, "describe": describe, "dirty": dirty}


def build(recipe: dict[str, Any], determinism: dict[str, Any]) -> dict[str, Any]:
    """The provenance block embedded in the report (and, for real renders, the WAV)."""
    return {
        "tier": "same-recipe",  # not bit-for-bit (§7.1)
        "engine": engine_commit(),
        "recipe": recipe,
        "versions": {"report_schema": SCHEMA_VERSION, "quality_lab": "0.0.1-p0a"},
        "determinism": determinism,
    }
