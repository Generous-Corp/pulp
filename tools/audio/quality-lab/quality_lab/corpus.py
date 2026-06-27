"""Versioned, license-guarded corpus (§5 P0b — the keystone dataset).

A corpus is a directory with a `MANIFEST.json` listing every source by material class,
license, and the artifact(s) it should expose. Two source kinds:

- **generator-backed** — synthetic, regenerable (drum break, tonal pad). No WAV is
  committed; `materialize()` regenerates it deterministically. License "synthetic".
- **file-backed** — real audio the developer supplies. The WAV lives under `sources/`
  with a recorded SHA-256, and its license MUST be permissive.

License fence (the plan's §4, enforced in code): the COMMITTED corpus stays license-
clean. `add_source` rejects any non-permissive license, and no GPL/proprietary audio can
enter the manifest. R3/PEAQ outputs and copyleft material stay developer-local, never
committed here.
"""
from __future__ import annotations

import json
import os
import shutil
from typing import Any

from . import audio_io, generate, provenance

# Permissive licenses allowed in the committed corpus (plus the synthetic marker).
PERMISSIVE_LICENSES = {
    "synthetic", "CC0", "CC0-1.0", "public-domain", "CC-BY-4.0", "CC-BY-3.0",
    "CC-BY", "MIT", "BSD-3-Clause", "Apache-2.0",
}
MATERIAL_CLASSES = {
    "percussive", "bass", "vocal", "tonal", "pad", "mix", "noise", "synth", "poly",
}


def default_corpus_dir() -> str:
    return os.path.join(os.path.dirname(__file__), "..", "corpus")


def manifest_path(corpus_dir: str) -> str:
    return os.path.join(corpus_dir, "MANIFEST.json")


def load_manifest(corpus_dir: str) -> dict[str, Any]:
    path = manifest_path(corpus_dir)
    if not os.path.exists(path):
        return {"schema_version": 1, "sources": []}
    with open(path) as f:
        return json.load(f)


def _write_manifest(corpus_dir: str, manifest: dict[str, Any]) -> None:
    os.makedirs(corpus_dir, exist_ok=True)
    tmp = manifest_path(corpus_dir) + ".tmp"
    with open(tmp, "w") as f:
        json.dump(manifest, f, indent=2)
    os.replace(tmp, manifest_path(corpus_dir))  # atomic


def _validate(name: str, material_class: str, license_id: str) -> None:
    if license_id not in PERMISSIVE_LICENSES:
        raise ValueError(
            f"license '{license_id}' is not permissive — the committed corpus stays "
            f"license-clean (§4). Allowed: {sorted(PERMISSIVE_LICENSES)}"
        )
    if material_class not in MATERIAL_CLASSES:
        raise ValueError(f"unknown material_class '{material_class}'; use {sorted(MATERIAL_CLASSES)}")
    if not name:
        raise ValueError("source name is required")


def _upsert(manifest: dict[str, Any], entry: dict[str, Any]) -> None:
    manifest["sources"] = [s for s in manifest.get("sources", []) if s["name"] != entry["name"]]
    manifest["sources"].append(entry)
    manifest["sources"].sort(key=lambda s: s["name"])


def add_source(
    corpus_dir: str, wav_path: str, *, name: str, material_class: str,
    license_id: str, expected: str, family: str = "tonal",
) -> dict[str, Any]:
    """Register a real (file-backed) audio source. Copies the WAV under sources/ and
    records its SHA-256. Rejects non-permissive licenses."""
    _validate(name, material_class, license_id)
    if not os.path.exists(wav_path):
        raise FileNotFoundError(wav_path)
    os.makedirs(os.path.join(corpus_dir, "sources"), exist_ok=True)
    dest_rel = os.path.join("sources", f"{name}.wav")
    dest = os.path.join(corpus_dir, dest_rel)
    shutil.copyfile(wav_path, dest)
    entry = {
        "name": name, "kind": "file", "file": dest_rel,
        "material_class": material_class, "license": license_id,
        "expected_artifacts": expected, "family": family,
        "content_sha256": provenance.content_hash(dest),
    }
    manifest = load_manifest(corpus_dir)
    _upsert(manifest, entry)
    _write_manifest(corpus_dir, manifest)
    return entry


def register_generated(
    corpus_dir: str, *, name: str, generator: str, material_class: str,
    expected: str, family: str, params: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Register a synthetic, regenerable source (no WAV committed). License 'synthetic'."""
    _validate(name, material_class, "synthetic")
    entry = {
        "name": name, "kind": "generator", "generator": generator,
        "material_class": material_class, "license": "synthetic",
        "expected_artifacts": expected, "family": family, "params": params or {},
    }
    manifest = load_manifest(corpus_dir)
    _upsert(manifest, entry)
    _write_manifest(corpus_dir, manifest)
    return entry


_GENERATORS = {
    "drum_break": lambda p: generate.render_drum_break(
        int(p.get("sr", 48000)), p.get("bpm", 120.0), p.get("ratio", 1.0), int(p.get("seed", 0)))[0],
    "tonal_pad": lambda p: generate.render_tonal(
        int(p.get("sr", 48000)), p.get("dur_s", 2.5), int(p.get("seed", 0)))[0],
}


def materialize(corpus_dir: str, name: str) -> str:
    """Produce a source's WAV on disk (regenerating generator-backed ones). Returns the
    absolute path. Verifies the recorded hash for file-backed sources."""
    manifest = load_manifest(corpus_dir)
    src = next((s for s in manifest["sources"] if s["name"] == name), None)
    if src is None:
        raise KeyError(f"no source '{name}' in corpus")
    if src["kind"] == "file":
        path = os.path.join(corpus_dir, src["file"])
        if provenance.content_hash(path) != src["content_sha256"]:
            raise ValueError(f"content hash mismatch for {name} — corpus file changed")
        return path
    gen = _GENERATORS.get(src["generator"])
    if gen is None:
        raise ValueError(f"unknown generator '{src['generator']}'")
    out = os.path.join(corpus_dir, "sources", f"{name}.wav")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    audio_io.save_wav(out, gen(src["params"]), int(src["params"].get("sr", 48000)))
    return out


def seed(corpus_dir: str | None = None) -> dict[str, Any]:
    """Seed the committed corpus with the two synthetic families (regenerable, license-
    clean) so it is never empty and the workflow is demonstrated."""
    corpus_dir = corpus_dir or default_corpus_dir()
    register_generated(corpus_dir, name="synthetic_drumbreak", generator="drum_break",
                       material_class="percussive", family="time-stretch",
                       expected="transient attack smear under time-stretch",
                       params={"sr": 48000, "bpm": 120.0, "ratio": 1.0, "seed": 0})
    register_generated(corpus_dir, name="synthetic_tonalpad", generator="tonal_pad",
                       material_class="tonal", family="tonal",
                       expected="graininess / dulling on sustained material",
                       params={"sr": 48000, "dur_s": 2.5, "seed": 0})
    return load_manifest(corpus_dir)
