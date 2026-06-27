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


def test_export_writes_provenance_sidecars_and_perceptual(tmp_path, monkeypatch):
    monkeypatch.delenv(perceptual.VISQOL_ENV, raising=False)  # ensure perceptual skips
    out = str(tmp_path / "run")
    report = pipeline.run_and_export("smear", out)

    cand_sidecar = os.path.join(out, "candidate.wav.provenance.json")
    assert os.path.exists(cand_sidecar)
    block = json.load(open(cand_sidecar))
    # round-trips the recipe and carries a content hash for the sample
    assert block["recipe"]["degradation"] == "smear"
    assert len(block["sample"]["content_sha256"]) == 64

    # perceptual block present and gracefully skipped (no ViSQOL installed)
    assert report["perceptual"][0]["status"] == "skipped"


def test_content_hash_matches_file(tmp_path):
    p = tmp_path / "x.bin"
    p.write_bytes(b"hello world")
    import hashlib
    assert provenance.content_hash(str(p)) == hashlib.sha256(b"hello world").hexdigest()
