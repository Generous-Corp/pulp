"""Autonomous tuning loop (experimental) — the safety properties that make 'autonomous'
acceptable: it proposes, a human decides; it refuses metric-gaming; it is deterministic.
"""
from __future__ import annotations

import json
import os

from quality_lab import corpus, loop, pipeline
from quality_lab.loop import CandidateScore


def _cs(label, scores, confidence=1.0):
    return CandidateScore(label=label, scores=scores, confidence=confidence)


# ── Goodhart guard: the critical anti-gaming test ─────────────────────────

def test_gaming_one_detector_while_regressing_another_is_refused():
    champ = _cs("champ", {"spectral_centroid": 0.10, "hf_fizz": 0.10})
    # lowers centroid a lot but regresses hf_fizz — classic Goodhart gaming
    gamer = _cs("gamer", {"spectral_centroid": 0.00, "hf_fizz": 0.30})
    assert not loop.pareto_improves(gamer, champ)
    verdict = loop.goodhart_guard(gamer, champ)
    assert verdict["accepted"] is False and "Pareto" in verdict["reason"]


def test_genuine_pareto_improvement_is_proposed():
    champ = _cs("champ", {"spectral_centroid": 0.10, "hf_fizz": 0.10})
    better = _cs("better", {"spectral_centroid": 0.04, "hf_fizz": 0.04})  # better on both
    hochamp = _cs("hc", {"spectral_centroid": 0.10, "hf_fizz": 0.10})
    hobetter = _cs("hb", {"spectral_centroid": 0.05, "hf_fizz": 0.05})    # also better on holdout
    verdict = loop.goodhart_guard(better, champ, hobetter, hochamp)
    assert verdict["accepted"] is True


def test_improvement_that_fails_the_holdout_is_refused():
    champ = _cs("champ", {"spectral_centroid": 0.10})
    better = _cs("better", {"spectral_centroid": 0.04})       # wins on working set
    hochamp = _cs("hc", {"spectral_centroid": 0.10})
    howorse = _cs("hw", {"spectral_centroid": 0.13})          # but regresses the holdout
    verdict = loop.goodhart_guard(better, champ, howorse, hochamp)
    assert verdict["accepted"] is False and "held-out" in verdict["reason"]


def test_low_confidence_pareto_win_is_needs_ear_not_accepted():
    champ = _cs("champ", {"spectral_centroid": 0.10})
    better = _cs("better", {"spectral_centroid": 0.04}, confidence=0.3)  # win but unsure
    verdict = loop.goodhart_guard(better, champ)
    assert verdict["accepted"] is False and verdict["needs_ear"] is True


# ── label proposal transaction: never touches MANIFEST.json ───────────────

def test_proposals_write_sidecar_and_never_touch_manifest(tmp_path):
    cdir = str(tmp_path / "corpus")
    os.makedirs(cdir)
    # seed a manifest and record its bytes
    corpus._write_manifest(cdir, {"schema_version": 1, "sources": [
        {"name": "vox1", "expected_artifacts": "graininess"}]})
    before = open(corpus.manifest_path(cdir), "rb").read()

    path = loop.propose_labels(cdir, [
        {"name": "vox1", "proposed_expected_artifacts": "graininess + roughness",
         "evidence": "hnr drop 5.2 dB on iteration 7"}])

    assert os.path.basename(path) == "LABEL_PROPOSALS.json"
    payload = json.load(open(path))
    assert payload["proposals"][0]["name"] == "vox1"
    # MANIFEST.json is byte-for-byte unchanged — the loop proposes, a human applies.
    assert open(corpus.manifest_path(cdir), "rb").read() == before


# ── determinism + convergence smoke ───────────────────────────────────────

def test_iteration_is_deterministic():
    cands = [score for score in (
        loop.score_case("identity", "identity"),
        loop.score_case("smear", "smear"),
        loop.score_case("dull", "dull"),
    )]
    a = loop.run_iteration(cands)
    b = loop.run_iteration(cands)
    assert a == b


def test_convergence_smoke_ranks_the_clean_candidate_top():
    """Smoke only (not promotion evidence): the loop should rank the artifact-free
    candidate as champion over degraded ones."""
    cands = [
        loop.score_case("identity", "identity"),
        loop.score_case("smear", "smear"),
        loop.score_case("dull", "dull"),
    ]
    result = loop.run_iteration(cands)
    assert result["champion"] == "identity"


def test_loop_scores_only_verdict_participating_detectors():
    """onset_drift is experimental → advisory; the loop must not optimize against it."""
    cs = loop.score_case("smear", "smear", case=pipeline.P0A_CASE)
    assert "onset_drift" not in cs.scores       # excluded (experimental)
    assert "transient_sharpness" in cs.scores   # included (stable)
