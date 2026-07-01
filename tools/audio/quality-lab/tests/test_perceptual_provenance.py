"""Layer B perceptual adapter (opt-in, license-fenced) + self-describing provenance."""
from __future__ import annotations

import json
import os
import stat

from quality_lab import perceptual, pipeline, provenance


def test_visqol_skips_when_env_unset(monkeypatch):
    """With no PULP_VISQOL_BIN, the adapter SKIPS gracefully — never errors, never a gate."""
    monkeypatch.delenv(perceptual.VISQOL_ENV, raising=False)
    r = perceptual.run_visqol("ref.wav", "cand.wav")
    assert r["status"] == "skipped"
    assert r["mos_lqo"] is None
    assert "not set" in r["reason"]


def test_visqol_skips_when_binary_missing(monkeypatch):
    monkeypatch.setenv(perceptual.VISQOL_ENV, "/nonexistent/visqol-binary-xyz")
    r = perceptual.run_visqol("ref.wav", "cand.wav")
    assert r["status"] == "skipped" and "not found" in r["reason"]


def test_visqol_parse_mos():
    assert perceptual.parse_mos("MOS-LQO: 4.823") == 4.823
    assert perceptual.parse_mos("MOS_LQO = 3.5") == 3.5
    assert abs(perceptual.parse_mos("...\nfinal score 4.21 done") - 4.21) < 1e-9
    assert perceptual.parse_mos("no score here") is None


def test_visqol_with_stub_binary(monkeypatch, tmp_path):
    """A stub 'visqol' that prints a MOS line exercises the real subprocess + parse path
    without installing ViSQOL — proving the env-path adapter works end to end."""
    stub = tmp_path / "visqol_stub.sh"
    stub.write_text("#!/bin/sh\necho 'MOS-LQO: 4.42'\n")
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv(perceptual.VISQOL_ENV, str(stub))
    r = perceptual.run_visqol("ref.wav", "cand.wav")
    assert r["status"] == "ok" and r["mos_lqo"] == 4.42


def test_peaq_and_aquatk_skip_when_env_unset(monkeypatch):
    """PEAQ and AQUA-Tk skip independently when their own env-paths are unset — proving
    per-tool opt-in: setting one env-path does not require the others."""
    for env in (perceptual.PEAQ_ENV, perceptual.AQUATK_ENV):
        monkeypatch.delenv(env, raising=False)
    for run in (perceptual.run_peaq, perceptual.run_aquatk):
        r = run("ref.wav", "cand.wav")
        assert r["status"] == "skipped"
        assert r["odg"] is None
        assert "not set" in r["reason"]


def test_parse_odg():
    assert perceptual.parse_odg("Objective Difference Grade: -0.293") == -0.293
    assert perceptual.parse_odg("ODG = -3.5") == -3.5
    assert perceptual.parse_odg("Objective Difference Grade:  0.00") == 0.0
    assert perceptual.parse_odg("no grade here") is None


def test_peaq_with_stub_binary(monkeypatch, tmp_path):
    """A stub 'peaq' that prints an ODG line exercises the real subprocess + parse path
    without installing a GPL PEAQ — proving the env-path adapter works end to end."""
    stub = tmp_path / "peaq_stub.sh"
    stub.write_text("#!/bin/sh\necho 'Objective Difference Grade: -1.23'\n")
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv(perceptual.PEAQ_ENV, str(stub))
    r = perceptual.run_peaq("ref.wav", "cand.wav")
    assert r["status"] == "ok" and r["odg"] == -1.23


def test_aquatk_with_stub_binary(monkeypatch, tmp_path):
    stub = tmp_path / "aquatk_stub.sh"
    stub.write_text("#!/bin/sh\necho 'ODG: -0.51'\n")
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv(perceptual.AQUATK_ENV, str(stub))
    r = perceptual.run_aquatk("ref.wav", "cand.wav")
    assert r["status"] == "ok" and r["odg"] == -0.51


def test_evaluate_lists_all_three_and_each_skips_independently(monkeypatch):
    """`evaluate()` consults every full-reference model; each skips on its own env-path."""
    for env in (perceptual.VISQOL_ENV, perceptual.PEAQ_ENV, perceptual.AQUATK_ENV):
        monkeypatch.delenv(env, raising=False)
    results = perceptual.evaluate("ref.wav", "cand.wav")
    assert [r["tool"] for r in results] == ["visqol", "peaq", "aquatk"]
    assert all(r["status"] == "skipped" for r in results)


def test_export_writes_provenance_sidecars_and_perceptual(tmp_path, monkeypatch):
    for env in (perceptual.VISQOL_ENV, perceptual.PEAQ_ENV, perceptual.AQUATK_ENV):
        monkeypatch.delenv(env, raising=False)  # ensure perceptual skips
    monkeypatch.delenv("PULP_AUBIO_BIN", raising=False)  # ensure MIR oracle skips
    out = str(tmp_path / "run")
    report = pipeline.run_and_export("smear", out)

    cand_sidecar = os.path.join(out, "candidate.wav.provenance.json")
    assert os.path.exists(cand_sidecar)
    block = json.load(open(cand_sidecar))
    # round-trips the recipe and carries a content hash for the sample
    assert block["recipe"]["degradation"] == "smear"
    assert len(block["sample"]["content_sha256"]) == 64

    # all three perceptual models present and gracefully skipped (none installed)
    assert [r["tool"] for r in report["perceptual"]] == ["visqol", "peaq", "aquatk"]
    assert all(r["status"] == "skipped" for r in report["perceptual"])
    # MIR oracle block present and gracefully skipped (no aubio installed)
    assert report["advisory"]["mir_oracles"][0]["status"] == "skipped"


def test_content_hash_matches_file(tmp_path):
    p = tmp_path / "x.bin"
    p.write_bytes(b"hello world")
    import hashlib
    assert provenance.content_hash(str(p)) == hashlib.sha256(b"hello world").hexdigest()
