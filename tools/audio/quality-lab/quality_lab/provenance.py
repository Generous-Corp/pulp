"""Provenance — make a generated set re-derivable (§7.1).

Records what cheaply maps a liked sample back to how it was made: engine commit,
recipe, versions, determinism context. This is the "same recipe reproduces the sound"
tier; strict bit-for-bit is a separate, stricter tier (§7.1) we do not promise here.
"""
from __future__ import annotations

import hashlib
import json
import os
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


def content_hash(path: str) -> str:
    """SHA-256 of a file's bytes — the key that lets even a stripped WAV be matched back
    to its provenance record."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def write_sidecar(wav_path: str, provenance_block: dict[str, Any]) -> str:
    """Make a rendered WAV self-describing (§7.1, the user's stronger ask: get back to a
    sound you liked from the sample alone). Writes `<wav>.provenance.json` carrying the
    provenance block plus the WAV's content hash, so the sample maps back to its
    commit + recipe even if separated from the run folder. Returns the sidecar path."""
    block = dict(provenance_block)
    block["sample"] = {"file": os.path.basename(wav_path), "content_sha256": content_hash(wav_path)}
    sidecar = wav_path + ".provenance.json"
    with open(sidecar, "w") as f:
        json.dump(block, f, indent=2)
    return sidecar
