"""Real-engine credibility: run the lab against the ACTUAL Pulp stretch engine.

This is the strongest evidence that the detectors matter — they catch artifacts from
`pulp::signal::OfflineStretch` (the product engine), not just my `reference_pv`. The
test SKIPS when stretchcli isn't built (public CI doesn't build it; the lab's pytest
must stay dependency-free), so it never blocks a basic run.

To exercise it locally:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF
    cmake --build build --target stretchcli
    pytest tools/audio/quality-lab/tests/test_real_engine.py -q
"""
from __future__ import annotations

import os

import pytest

from quality_lab import align, audio_io, engine, generate
from quality_lab.detectors import transient_sharpness

requires_engine = pytest.mark.skipif(
    not engine.available(), reason="stretchcli not built (build it or set PULP_STRETCHCLI)"
)


def test_engine_adapter_skips_cleanly_when_absent(monkeypatch):
    """Absence is graceful (mirrors the perceptual adapter): skipped, with a reason."""
    monkeypatch.setenv(engine.STRETCHCLI_ENV, "/nonexistent/stretchcli-xyz")
    # Even with a bogus env-path, resolve() may still find a build-path binary; only
    # assert the *contract* of stretch() when no binary exists at all.
    if engine.resolve() is None:
        r = engine.stretch("a.wav", "b.wav", 2.0)
        assert r["status"] == "skipped" and "stretchcli" in r["reason"]
        # The skip reason is actionable: names both the build command and the env-path.
        assert "cmake --build" in r["reason"] and engine.STRETCHCLI_ENV in r["reason"]


def _fake_stretchcli(root):
    binary = root / "build" / "examples" / "offline-stretch" / "stretchcli"
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.write_text("#!/bin/sh\n")
    return binary


def test_resolve_walks_up_from_cwd_to_find_built_stretchcli(tmp_path, monkeypatch):
    """A pip-installed lab (its __file__ in a managed venv) still finds a stretchcli
    built in a Pulp checkout when run from inside that checkout — resolve() walks up
    from the current directory, not just from the package location."""
    monkeypatch.delenv(engine.STRETCHCLI_ENV, raising=False)
    binary = _fake_stretchcli(tmp_path)
    nested = tmp_path / "tools" / "audio" / "quality-lab"
    nested.mkdir(parents=True)
    monkeypatch.chdir(nested)
    resolved = engine.resolve()
    assert resolved is not None and os.path.samefile(resolved, str(binary))


def test_env_path_takes_priority_over_cwd_walk(tmp_path, monkeypatch):
    """An explicit PULP_STRETCHCLI override wins over a cwd-discovered build."""
    _fake_stretchcli(tmp_path)  # a cwd-walk candidate also exists
    env_binary = tmp_path / "custom-stretchcli"
    env_binary.write_text("#!/bin/sh\n")
    monkeypatch.setenv(engine.STRETCHCLI_ENV, str(env_binary))
    monkeypatch.chdir(tmp_path)
    assert os.path.samefile(engine.resolve(), str(env_binary))


@requires_engine
def test_transient_detector_fires_on_real_engine_smear(tmp_path):
    """The real OfflineStretch phase vocoder smears percussion attacks; the detector
    must catch it (non-circular, real-product-engine evidence)."""
    sr, ratio = 48000, 2.0
    src_wav = str(tmp_path / "source.wav")
    out_wav = str(tmp_path / "engine_2x.wav")

    src, _ = generate.render_drum_break(sr, 120.0, 1.0, 0)
    audio_io.save_wav(src_wav, src, sr)

    res = engine.stretch(src_wav, out_wav, ratio, character="clean")
    assert res["status"] == "ok", f"stretchcli failed: {res}"
    assert os.path.exists(out_wav)

    reference, _ = generate.render_drum_break(sr, 120.0, ratio, 0)  # transient-faithful 2x
    eng, _ = audio_io.load_wav(out_wav)
    eng = audio_io.level_match(eng, reference)

    ro = align.detect_onsets(reference, sr)
    co = align.detect_onsets(eng, sr)
    pairs = align.map_onsets(ro, co, len(reference) / sr, len(eng) / sr)
    det = transient_sharpness.detect(reference, eng, sr, pairs)
    assert det.fired, f"detector missed real-engine attack smear: scalar={det.scalar}"
    assert det.coverage >= 0.5
