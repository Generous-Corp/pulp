"""The golden-render regression net (the daily-driver 'verify my DSP change' loop).

Pins the policy that IS the whole point: the net fails (exit 1) ONLY on an axis
`regression_suspected`; corroboration disagreement — endemic to time-variant effects — is surfaced
in the table but never fails the run. A pair that could not be measured (a missing/corrupt render →
`invalid`) is a separate ERROR signal (exit 2), never silently greenlit as clean.
"""
from __future__ import annotations

import json

import numpy as np

from quality_lab import audio_io, compare, generate, regression_net


def _drum(seed: int = 0):
    y, _ = generate.render_drum_break(sr=48000, seed=seed)
    return y, 48000


def _write(tmp_path, name, y, sr):
    p = str(tmp_path / name)
    audio_io.save_wav(p, y, sr)
    return p


def test_net_fails_on_an_axis_regression(tmp_path):
    ref, sr = _drum()
    golden = _write(tmp_path, "golden.wav", ref, sr)
    cand = _write(tmp_path, "cand.wav", generate.dull(ref, sr), sr)     # duller -> tonal-balance regression
    rows = regression_net.run_net([("reverb-tone", golden, cand)], profiles=("tonal-balance",))
    assert regression_net.net_failed(rows) is True
    assert any(r.is_regression for r in rows)


def test_net_is_clean_on_identity(tmp_path):
    ref, sr = _drum()
    golden = _write(tmp_path, "g.wav", ref, sr)
    cand = _write(tmp_path, "c.wav", ref.copy(), sr)
    rows = regression_net.run_net([("noop", golden, cand)])            # all four profiles
    assert regression_net.net_failed(rows) is False
    assert all(r.verdict == compare.VERDICT_NO_CHANGE for r in rows)


def test_corroboration_disagreement_does_not_fail_the_net(tmp_path):
    """A time-variant change (a pure delay, standing in for chorus/flanger/bendr): every AXIS reads
    no material change, so the net must NOT fail — even though the sample-domain residual disagrees
    (corroboration `not_corroborated` + an `uncaptured_material_difference` flag). Corroboration is
    informational; the fail keys off axis verdicts only."""
    ref, sr = _drum()
    golden = _write(tmp_path, "g.wav", ref, sr)
    cand = _write(tmp_path, "c.wav", np.roll(ref, 64), sr)             # phase differs, tone identical
    rows = regression_net.run_net([("chorus-analog", golden, cand)], profiles=("tonal-balance",))
    assert regression_net.net_failed(rows) is False                    # NOT failed by disagreement
    row = rows[0]
    assert row.verdict == compare.VERDICT_NO_CHANGE
    assert row.corroboration == compare.NOT_CORROBORATED               # ...but the disagreement is SHOWN
    assert compare.FLAG_UNCAPTURED_DIFF in row.flags


def test_net_runs_every_pair_across_every_profile(tmp_path):
    ref, sr = _drum()
    golden = _write(tmp_path, "g.wav", ref, sr)
    cand = _write(tmp_path, "c.wav", generate.dull(ref, sr), sr)
    profiles = ("tonal-balance", "added-hf", "noise-roughness", "graininess")
    rows = regression_net.run_net([("a", golden, cand), ("b", golden, cand)], profiles=profiles)
    assert len(rows) == 2 * len(profiles)
    assert {r.profile for r in rows} == set(profiles)


def test_format_table_has_corroboration_column(tmp_path):
    ref, sr = _drum()
    golden = _write(tmp_path, "g.wav", ref, sr)
    cand = _write(tmp_path, "c.wav", generate.dull(ref, sr), sr)
    rows = regression_net.run_net([("fx", golden, cand)], profiles=("tonal-balance",))
    table = regression_net.format_table(rows)
    assert "corroboration" in table and "verdict" in table and "fx" in table
    assert regression_net.format_table([]) == "(no pairs)"


def test_missing_render_is_an_error_not_a_clean_pass(tmp_path):
    """A missing/corrupt candidate render -> compare `invalid` -> the net must ERROR (not fail, not
    pass): a regression net that greenlights a broken render defeats its own purpose."""
    ref, sr = _drum()
    golden = _write(tmp_path, "g.wav", ref, sr)
    rows = regression_net.run_net([("plate", golden, str(tmp_path / "MISSING.wav"))],
                                  profiles=("tonal-balance",))
    assert rows[0].verdict == compare.VERDICT_INVALID
    assert regression_net.net_failed(rows) is False        # not an axis regression...
    assert regression_net.net_errored(rows) is True        # ...but a could-not-measure ERROR
    assert regression_net.status(rows) == "ERROR"


def test_zero_checks_is_an_error():
    """An empty run (no pairs) must not read as CLEAN — zero checks is a broken net."""
    assert regression_net.net_errored([]) is True
    assert regression_net.status([]) == "ERROR"


def test_cli_missing_render_exits_2(tmp_path, capsys):
    from quality_lab import cli
    ref, sr = _drum()
    _write(tmp_path, "golden.wav", ref, sr)
    manifest = tmp_path / "net.json"
    manifest.write_text(json.dumps({
        "profiles": ["tonal-balance"],
        "pairs": [{"name": "plate", "golden": "golden.wav", "candidate": "missing.wav"}],
    }))
    rc = cli.main(["regression-net", "--manifest", str(manifest)])
    assert rc == 2                                          # ERROR exit, distinct from regression (1)
    assert "ERROR" in capsys.readouterr().out


def test_cli_malformed_manifest_exits_2_not_traceback(tmp_path, capsys):
    from quality_lab import cli
    for body in ("{ not json", json.dumps({"pairs": []}),
                 json.dumps({"pairs": [{"name": "x", "golden": "g.wav"}]})):   # missing 'candidate'
        manifest = tmp_path / "bad.json"
        manifest.write_text(body)
        rc = cli.main(["regression-net", "--manifest", str(manifest)])
        assert rc == 2
        assert "ERROR" in capsys.readouterr().out


def test_status_reports_regression_over_error(tmp_path):
    """A real regression outranks an error in the single-word status."""
    ref, sr = _drum()
    golden = _write(tmp_path, "g.wav", ref, sr)
    cand = _write(tmp_path, "c.wav", generate.dull(ref, sr), sr)
    rows = regression_net.run_net([("fx", golden, cand), ("broken", golden, str(tmp_path / "no.wav"))],
                                  profiles=("tonal-balance",))
    assert regression_net.status(rows) == "REGRESSION"


def test_run_manifest_resolves_paths_and_reports(tmp_path):
    ref, sr = _drum()
    _write(tmp_path, "golden.wav", ref, sr)
    _write(tmp_path, "cand.wav", generate.dull(ref, sr), sr)
    manifest = tmp_path / "net.json"
    manifest.write_text(json.dumps({
        "reference_role": "golden",
        "profiles": ["tonal-balance"],
        "pairs": [{"name": "reverb", "golden": "golden.wav", "candidate": "cand.wav"}],  # relative
    }))
    rows, failed = regression_net.run_manifest(str(manifest))
    assert failed is True and rows[0].name == "reverb"


def test_cli_regression_net_exit_code_and_table(tmp_path, capsys):
    from quality_lab import cli
    ref, sr = _drum()
    _write(tmp_path, "golden.wav", ref, sr)
    _write(tmp_path, "cand.wav", generate.dull(ref, sr), sr)
    out_p = str(tmp_path / "results.json")
    manifest = tmp_path / "net.json"
    manifest.write_text(json.dumps({
        "profiles": ["tonal-balance"],
        "pairs": [{"name": "reverb", "golden": "golden.wav", "candidate": "cand.wav"}],
    }))
    rc = cli.main(["regression-net", "--manifest", str(manifest), "--json", out_p])
    assert rc == 1                                                     # a regression fails the net
    out = capsys.readouterr().out
    assert "REGRESSION" in out and "corroboration" in out
    import os
    assert os.path.exists(out_p)
    with open(out_p) as fh:
        assert json.load(fh)["failed"] is True
