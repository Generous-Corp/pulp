"""Advisory LLM/multimodal reviewer — opt-in, never a gate.

It must: skip cleanly when no provider is configured; run a developer-supplied subprocess
provider and attach its output under `advisory.reviewers`; NEVER change the verdict; never
raise on a bad/slow/garbage provider; and expose an answer-key scoring harness so a real
reviewer's accuracy can be measured before promotion.
"""
from __future__ import annotations

import json
import sys

from quality_lab import pipeline, reviewer


def _fired_artifacts_provider(tmp_path):
    """A deterministic fake provider: reads the report on stdin and names the fired
    detectors as suspected artifacts. Stands in for a real model in tests."""
    p = tmp_path / "fake_reviewer.py"
    p.write_text(
        "import sys, json\n"
        "d = json.load(sys.stdin)\n"
        "fired = [x['name'] for x in d['report']['detectors'] if x['fired']]\n"
        "print(json.dumps({'summary': 'auto', 'suspected_artifacts': fired,\n"
        "                  'confidence': 0.8, 'notes': 'fake'}))\n"
    )
    return f"{sys.executable} {p}"


def test_skips_when_no_provider_configured(monkeypatch):
    monkeypatch.delenv(reviewer.REVIEWER_ENV, raising=False)
    r = reviewer.review(pipeline.run("smear", case=pipeline.P0A_CASE))
    assert r["status"] == "skipped" and r["advisory"] is True


def test_runs_provider_and_attaches_under_advisory(monkeypatch, tmp_path):
    monkeypatch.setenv(reviewer.REVIEWER_ENV, _fired_artifacts_provider(tmp_path))
    report = pipeline.run("smear", case=pipeline.P0A_CASE)
    verdict_before = report["verdict"]
    reviewer.attach(report)
    rv = report["advisory"]["reviewers"][-1]
    assert rv["status"] == "ok" and rv["advisory"] is True
    assert "transient_sharpness" in rv["suspected_artifacts"]  # smear fires it
    assert rv["confidence"] == 0.8
    # FALSE-AUTHORITY GUARD: attaching a reviewer never changes the verdict.
    assert report["verdict"] == verdict_before


def test_wrong_or_broken_provider_never_changes_verdict_or_raises(monkeypatch, tmp_path):
    # A provider that screams a (wrong) catastrophic verdict + exits nonzero.
    bad = tmp_path / "bad.py"
    bad.write_text("import sys; sys.stderr.write('FIRED EVERYTHING'); sys.exit(3)\n")
    monkeypatch.setenv(reviewer.REVIEWER_ENV, f"{sys.executable} {bad}")
    report = pipeline.run("identity", case=pipeline.P0A_CASE)
    assert report["verdict"] == "CLEAN"
    reviewer.attach(report)  # must not raise
    assert report["advisory"]["reviewers"][-1]["status"] == "error"
    assert report["verdict"] == "CLEAN"  # unchanged by a hostile reviewer


def test_non_json_provider_is_an_error_not_a_crash(monkeypatch, tmp_path):
    junk = tmp_path / "junk.py"
    junk.write_text("print('not json at all')\n")
    monkeypatch.setenv(reviewer.REVIEWER_ENV, f"{sys.executable} {junk}")
    r = reviewer.review({"verdict": "CLEAN", "detectors": []})
    assert r["status"] == "error" and "JSON" in r["reason"]


def test_confidence_is_clamped(monkeypatch, tmp_path):
    p = tmp_path / "overconf.py"
    p.write_text("import json; print(json.dumps({'confidence': 9.0, 'suspected_artifacts': []}))\n")
    monkeypatch.setenv(reviewer.REVIEWER_ENV, f"{sys.executable} {p}")
    r = reviewer.review({"verdict": "CLEAN", "detectors": []})
    assert r["confidence"] == 1.0  # clamped to [0,1]


# ── answer-key scoring (the promotion metric) ─────────────────────────────

def test_score_agreement_precision_recall():
    s = reviewer.score_agreement(suspected=["Smear", "fizz"], ground_truth=["smear"])
    assert s["precision"] == 0.5 and s["recall"] == 1.0  # case-folded set match
    perfect = reviewer.score_agreement(["dull"], ["dull"])
    assert perfect["f1"] == 1.0
    miss = reviewer.score_agreement([], ["grainy"])
    assert miss["recall"] == 0.0
