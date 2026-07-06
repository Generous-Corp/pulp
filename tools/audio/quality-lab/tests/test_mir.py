"""MIR structural oracle (aubio, opt-in, license-fenced) — advisory cross-check."""
from __future__ import annotations

import stat

from quality_lab import mir


def test_aubio_skips_when_env_unset(monkeypatch):
    """With no PULP_AUBIO_BIN, the oracle SKIPS gracefully — never errors, never a gate."""
    monkeypatch.delenv(mir.AUBIO_ENV, raising=False)
    r = mir.run_aubio("ref.wav", "cand.wav")
    assert r["status"] == "skipped"
    assert r["onset_count_delta"] is None
    assert r["role"] == "onset_drift_cross_validation"
    assert "not set" in r["reason"]


def test_aubio_skips_when_binary_missing(monkeypatch):
    monkeypatch.setenv(mir.AUBIO_ENV, "/nonexistent/aubio-binary-xyz")
    r = mir.run_aubio("ref.wav", "cand.wav")
    assert r["status"] == "skipped" and "not found" in r["reason"]


def test_count_onsets():
    # aubio prints one event time (seconds) per line; blank/non-numeric lines ignored.
    assert mir.count_onsets("0.125000\n0.500000\n1.024000\n") == 3
    assert mir.count_onsets("") == 0
    assert mir.count_onsets("\n\n") == 0


def test_aubio_with_stub_binary(monkeypatch, tmp_path):
    """A stub 'aubio' whose `onset` subcommand prints event times exercises the real
    subprocess + parse path without installing GPL aubio. The stub emits 3 onsets for the
    reference and 5 for the candidate, so the delta is +2."""
    stub = tmp_path / "aubio_stub.sh"
    stub.write_text(
        "#!/bin/sh\n"
        '# $1=subcommand (onset), $2=wav path\n'
        'case "$2" in\n'
        '  *cand*) printf "0.10\\n0.20\\n0.30\\n0.40\\n0.50\\n" ;;\n'
        '  *)      printf "0.10\\n0.20\\n0.30\\n" ;;\n'
        'esac\n'
    )
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv(mir.AUBIO_ENV, str(stub))
    r = mir.run_aubio("some_ref.wav", "some_cand.wav")
    assert r["status"] == "ok"
    assert r["onsets_ref"] == 3
    assert r["onsets_cand"] == 5
    assert r["onset_count_delta"] == 2
    assert r["advisory"] is True


def test_evaluate_returns_aubio_entry(monkeypatch):
    monkeypatch.delenv(mir.AUBIO_ENV, raising=False)
    results = mir.evaluate("ref.wav", "cand.wav")
    assert [r["tool"] for r in results] == ["aubio"]
    assert results[0]["status"] == "skipped"
