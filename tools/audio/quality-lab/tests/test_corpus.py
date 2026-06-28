"""Versioned, license-guarded corpus (P0b)."""
from __future__ import annotations

import os

import pytest

from quality_lab import audio_io, corpus, generate


def test_seed_registers_two_synthetic_families(tmp_path):
    m = corpus.seed(str(tmp_path))
    names = {s["name"] for s in m["sources"]}
    assert {"synthetic_drumbreak", "synthetic_tonalpad"} <= names
    assert all(s["license"] == "synthetic" for s in m["sources"])


def test_add_source_rejects_non_permissive_license(tmp_path):
    wav = str(tmp_path / "x.wav")
    y, _ = generate.render_tonal(48000, 0.5, 0)
    audio_io.save_wav(wav, y, 48000)
    with pytest.raises(ValueError, match="not permissive"):
        corpus.add_source(str(tmp_path), wav, name="bad", material_class="tonal",
                          license_id="GPL-3.0", expected="x", family="tonal")


def test_add_source_records_hash_and_copies(tmp_path):
    wav = str(tmp_path / "src.wav")
    y, _ = generate.render_tonal(48000, 0.5, 0)
    audio_io.save_wav(wav, y, 48000)
    e = corpus.add_source(str(tmp_path), wav, name="myvox", material_class="vocal",
                         license_id="CC0", expected="graininess", family="tonal")
    assert len(e["content_sha256"]) == 64
    assert os.path.exists(os.path.join(str(tmp_path), e["file"]))


def test_add_source_rejects_unknown_material_class(tmp_path):
    wav = str(tmp_path / "x.wav")
    audio_io.save_wav(wav, generate.render_tonal(48000, 0.3, 0)[0], 48000)
    with pytest.raises(ValueError, match="material_class"):
        corpus.add_source(str(tmp_path), wav, name="x", material_class="banana",
                          license_id="CC0", expected="x")


def test_materialize_regenerates_generator_source(tmp_path):
    corpus.seed(str(tmp_path))
    path = corpus.materialize(str(tmp_path), "synthetic_drumbreak")
    assert os.path.exists(path)
    y, sr = audio_io.load_wav(path)
    assert sr == 48000 and len(y) > 48000  # a few seconds of audio


def test_materialize_detects_tampered_file(tmp_path):
    wav = str(tmp_path / "src.wav")
    audio_io.save_wav(wav, generate.render_tonal(48000, 0.5, 0)[0], 48000)
    corpus.add_source(str(tmp_path), wav, name="vox", material_class="vocal",
                     license_id="CC0", expected="x", family="tonal")
    # tamper with the committed copy
    audio_io.save_wav(os.path.join(str(tmp_path), "sources", "vox.wav"),
                      generate.render_tonal(48000, 0.9, 1)[0], 48000)
    with pytest.raises(ValueError, match="hash mismatch"):
        corpus.materialize(str(tmp_path), "vox")


def test_committed_corpus_seeded():
    """The committed corpus ships the synthetic families (never empty)."""
    m = corpus.load_manifest(corpus.default_corpus_dir())
    names = {s["name"] for s in m["sources"]}
    assert "synthetic_drumbreak" in names and "synthetic_tonalpad" in names
