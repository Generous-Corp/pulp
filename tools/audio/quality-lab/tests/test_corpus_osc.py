"""Oscillator corpus families (static shapes, hard-sync sweep, TZFM grid).

Extends the versioned, license-guarded corpus (`corpus.py`) with three synthetic,
regenerable oscillator families, wired through the SAME `add_source` /
`register_generated` / `materialize` machinery as the stretch-oriented families
(`synthetic_drumbreak`, `synthetic_tonalpad`) — see `test_corpus.py`. Gated with the
EXISTING `regression_net` ratchet (exit 1 = `regression_suspected`, exit 2 =
unmeasurable) — no hand-rolled ratchet here.

Two stats per family, both already-wired lab primitives:

- **determinism / null-residual** — re-materializing a generator-backed source into a
  fresh corpus dir must reproduce it bit-for-bit: exact array equality, AND
  `dsp.null_residual_db` reading the bit-identical clamp (-160 dB).
- **alias floor** — the `added-hf` compare axis (`quality_lab.compare`, backed by the
  `hf_fizz` detector) as a golden-vs-candidate regression gate: an identity render
  passes clean, and an injected HF defect (`generate.add_fizz`, a controlled stand-in
  for the alias energy a real regression would add) trips `regression_suspected`.
"""
from __future__ import annotations

import numpy as np

from quality_lab import audio_io, corpus, dsp, generate, regression_net

OSC_FAMILIES = ("synthetic_osc_static_shapes", "synthetic_osc_sync_sweep", "synthetic_osc_tzfm_grid")


def _write(tmp_path, name, y, sr):
    p = str(tmp_path / name)
    audio_io.save_wav(p, y, sr)
    return p


def test_seed_registers_oscillator_families(tmp_path):
    m = corpus.seed(str(tmp_path))
    names = {s["name"] for s in m["sources"]}
    assert set(OSC_FAMILIES) <= names
    osc_entries = {s["name"]: s for s in m["sources"] if s["name"] in OSC_FAMILIES}
    assert all(e["license"] == "synthetic" for e in osc_entries.values())
    assert all(e["family"] == "oscillator" for e in osc_entries.values())
    assert all(e["material_class"] == "synth" for e in osc_entries.values())


def test_committed_corpus_has_oscillator_families():
    """The committed corpus (not a tmp_path fixture) ships all three oscillator
    families — the ratchet must see them without a re-seed step."""
    m = corpus.load_manifest(corpus.default_corpus_dir())
    names = {s["name"] for s in m["sources"]}
    assert set(OSC_FAMILIES) <= names


def test_osc_families_materialize_to_valid_audio(tmp_path):
    corpus.seed(str(tmp_path))
    for name in OSC_FAMILIES:
        path = corpus.materialize(str(tmp_path), name)
        y, sr = audio_io.load_wav(path)
        assert sr == 48000
        assert len(y) > 1000
        assert np.all(np.isfinite(y))
        assert np.max(np.abs(y)) <= 1.0 + 1e-6


def test_osc_family_materialize_is_deterministic(tmp_path):
    """Regenerating a generator-backed oscillator source twice reproduces it
    bit-for-bit: the corpus's regenerable-not-committed contract depends on this,
    and it is the determinism/null-residual stat the ratchet leans on."""
    name = "synthetic_osc_static_shapes"
    first = tmp_path / "first"
    corpus.seed(str(first))
    y_a, sr_a = audio_io.load_wav(corpus.materialize(str(first), name))

    # A SEPARATE corpus dir for the second materialize, so this can't just be
    # re-reading the first render's file untouched.
    second = tmp_path / "second"
    corpus.seed(str(second))
    y_b, sr_b = audio_io.load_wav(corpus.materialize(str(second), name))

    assert sr_a == sr_b == 48000
    assert np.array_equal(y_a, y_b)
    nr = dsp.null_residual_db(y_a, y_b)
    assert nr.residual_db == -160.0  # dsp.null_residual_db's bit-identical clamp


def test_osc_family_clean_render_passes_the_added_hf_axis(tmp_path):
    """An identity golden-vs-candidate pair on the static-shape family must NOT trip
    the alias/HF regression axis — the clean-pass half of the ratchet."""
    corpus.seed(str(tmp_path))
    y, sr = audio_io.load_wav(corpus.materialize(str(tmp_path), "synthetic_osc_static_shapes"))
    golden = _write(tmp_path, "golden.wav", y, sr)
    candidate = _write(tmp_path, "candidate.wav", y.copy(), sr)

    rows = regression_net.run_net([("osc-static-shapes", golden, candidate)], profiles=("added-hf",))
    assert regression_net.net_failed(rows) is False
    assert regression_net.net_errored(rows) is False
    assert regression_net.status(rows) == "CLEAN"


def test_osc_family_degraded_render_trips_the_added_hf_regression(tmp_path):
    """A deliberately-degraded candidate (injected HF fizz — a controlled stand-in for
    real alias energy) MUST trip `regression_suspected` on the added-hf axis — the
    exit-1 half of the ratchet, exercised on oscillator material."""
    corpus.seed(str(tmp_path))
    y, sr = audio_io.load_wav(corpus.materialize(str(tmp_path), "synthetic_osc_static_shapes"))
    golden = _write(tmp_path, "golden.wav", y, sr)
    degraded = generate.add_fizz(y, sr, amount=0.25)
    candidate = _write(tmp_path, "candidate.wav", degraded, sr)

    rows = regression_net.run_net([("osc-static-shapes", golden, candidate)], profiles=("added-hf",))
    assert regression_net.net_failed(rows) is True
    assert any(r.is_regression for r in rows)
    assert regression_net.status(rows) == "REGRESSION"


def test_osc_family_sync_sweep_and_tzfm_grid_also_gate_clean(tmp_path):
    """Sanity: the other two oscillator families run through the SAME
    golden-vs-identity clean-pass path, not just static shapes."""
    corpus.seed(str(tmp_path))
    for name in ("synthetic_osc_sync_sweep", "synthetic_osc_tzfm_grid"):
        y, sr = audio_io.load_wav(corpus.materialize(str(tmp_path), name))
        golden = _write(tmp_path, f"{name}.golden.wav", y, sr)
        candidate = _write(tmp_path, f"{name}.candidate.wav", y.copy(), sr)
        rows = regression_net.run_net([(name, golden, candidate)], profiles=("added-hf",))
        assert regression_net.net_failed(rows) is False, name
