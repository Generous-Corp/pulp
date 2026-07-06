"""Layer C-tail — autonomous tuning loop (EXPERIMENTAL). NEVER auto-promotes.

Closes the loop so an agent can tune DSP across many iterations: score candidates with the
gate-participating detectors, rank regressions, and *propose* label updates that a human
applies — the loop decides nothing on its own. Three hardened safety properties (plan §4):

1. **Proposal-only.** Label changes are written to a sidecar `LABEL_PROPOSALS.json`; the
   loop never edits `MANIFEST.json` (the ground truth). A human applies proposals.
2. **Goodhart guard.** A candidate may only be promoted over the champion if it is a
   *normalized Pareto improvement* (better-or-equal on every detector in delta/threshold
   units, strictly better on at least one) on BOTH the working set AND a held-out slice,
   AND its confidence clears a bar — otherwise it is refused or flagged `NEEDS-EAR`. A
   candidate that games one detector while regressing another is refused.
3. **Determinism.** A given set of candidates yields the same ranking/decision every run.

Only verdict-participating detectors are scored (experimental ones are advisory; the loop
optimizes the real gate, not advisory signals). All current detectors measure degradation,
so lower scalar = better — encoded explicitly, not assumed.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from typing import Any, Callable

from . import pipeline

# Per-detector normalization for the Pareto comparison: a delta is meaningful when it is a
# nontrivial fraction of the detector's own threshold. All directions are lower-is-better.
DEFAULT_THRESHOLDS: dict[str, float] = {
    "transient_sharpness": 0.30,
    "spectral_centroid": 0.15,
    "hf_fizz": 0.05,
    "spectral_flux": 0.15,
    "hnr": 3.0,            # hnr scalar is an HNR *drop* in dB (higher = worse)
}
PARETO_EPS = 0.05         # normalized noise floor (5% of a threshold)
MIN_CONFIDENCE = 0.6      # below this a Pareto win is held for a human ear (NEEDS-EAR)


@dataclass
class CandidateScore:
    """A candidate (a DSP config / render) scored by the gate-participating detectors.
    `scores` maps detector -> scalar 'badness' (lower is better for all current detectors)."""
    label: str
    scores: dict[str, float] = field(default_factory=dict)
    confidence: float = 1.0

    def to_dict(self) -> dict[str, Any]:
        return {"label": self.label, "scores": {k: round(v, 6) for k, v in self.scores.items()},
                "confidence": round(self.confidence, 3)}


def score_case(label: str, degradation: str, case=None, confidence: float = 1.0) -> CandidateScore:
    """Score a synthetic candidate deterministically (no engine needed): run the pipeline
    and keep the VERDICT-PARTICIPATING detector scalars (experimental ones are advisory)."""
    report = pipeline.run(degradation, case=case or pipeline.P0A_CASE)
    scores = {d["name"]: float(d["scalar"]) for d in report["detectors"]
              if d.get("participates_in_verdict", True)}
    return CandidateScore(label=label, scores=scores, confidence=confidence)


def _normalized_deltas(cand: CandidateScore, champ: CandidateScore,
                       thresholds: dict[str, float]) -> dict[str, float]:
    """(cand - champ) / threshold per detector. Positive = candidate is WORSE."""
    keys = set(cand.scores) & set(champ.scores)
    out = {}
    for k in keys:
        thr = thresholds.get(k, 1.0) or 1.0
        out[k] = (cand.scores[k] - champ.scores[k]) / thr
    return out


def pareto_improves(cand: CandidateScore, champ: CandidateScore,
                    thresholds: dict[str, float] | None = None, eps: float = PARETO_EPS) -> bool:
    """True iff `cand` is a normalized Pareto improvement over `champ`: no detector is
    meaningfully worse (delta/threshold > eps) and at least one is meaningfully better."""
    nd = _normalized_deltas(cand, champ, thresholds or DEFAULT_THRESHOLDS)
    if not nd:
        return False
    if any(d > eps for d in nd.values()):
        return False                       # regressed a detector → not an improvement (anti-gaming)
    return any(d < -eps for d in nd.values())


def goodhart_guard(cand: CandidateScore, champ: CandidateScore,
                   holdout_cand: CandidateScore | None = None,
                   holdout_champ: CandidateScore | None = None,
                   thresholds: dict[str, float] | None = None,
                   min_confidence: float = MIN_CONFIDENCE) -> dict[str, Any]:
    """Decide whether `cand` may be PROPOSED for promotion over `champ`. Requires a Pareto
    improvement on the working set AND (when provided) the held-out slice, and confidence
    >= the bar. Never returns an auto-apply — `accepted` means 'propose to a human'."""
    if not pareto_improves(cand, champ, thresholds):
        return {"accepted": False, "needs_ear": False, "reason": "not a Pareto improvement on the working set"}
    if holdout_cand is not None and holdout_champ is not None:
        if not pareto_improves(holdout_cand, holdout_champ, thresholds):
            return {"accepted": False, "needs_ear": False,
                    "reason": "improves the working set but NOT the held-out slice (overfit risk)"}
    if cand.confidence < min_confidence:
        return {"accepted": False, "needs_ear": True,
                "reason": f"Pareto win but confidence {cand.confidence:.2f} < {min_confidence} — NEEDS-EAR"}
    return {"accepted": True, "needs_ear": False, "reason": "Pareto improvement on working + held-out slices"}


# ── label proposal transaction (sidecar; never touches MANIFEST.json) ─────

PROPOSALS_NAME = "LABEL_PROPOSALS.json"


def proposals_path(corpus_dir: str) -> str:
    return os.path.join(corpus_dir, PROPOSALS_NAME)


def propose_labels(corpus_dir: str, proposals: list[dict[str, Any]]) -> str:
    """Atomically write label PROPOSALS to a sidecar. This NEVER edits `MANIFEST.json` —
    a human reviews the sidecar and applies changes explicitly. Each proposal is
    `{name, proposed_expected_artifacts, evidence}`. Returns the sidecar path."""
    os.makedirs(corpus_dir, exist_ok=True)
    payload = {"schema_version": 1, "kind": "label-proposals",
               "note": "auto-proposed by the tuning loop; apply to MANIFEST.json by hand after review",
               "proposals": proposals}
    path = proposals_path(corpus_dir)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(payload, f, indent=2)
    os.replace(tmp, path)  # atomic
    return path


# ── one iteration ─────────────────────────────────────────────────────────

def run_iteration(candidates: list[CandidateScore],
                  thresholds: dict[str, float] | None = None) -> dict[str, Any]:
    """One deterministic loop pass: rank candidates by total normalized badness (lower =
    better), pick the champion, and report. Pure — no side effects, no promotion."""
    thr = thresholds or DEFAULT_THRESHOLDS

    def total_badness(c: CandidateScore) -> float:
        return sum(v / (thr.get(k, 1.0) or 1.0) for k, v in c.scores.items())

    ranked = sorted(candidates, key=lambda c: (total_badness(c), c.label))
    champion = ranked[0] if ranked else None
    return {
        "schema_version": 1,
        "champion": champion.label if champion else None,
        "ranked": [{"label": c.label, "total_badness": round(total_badness(c), 6)} for c in ranked],
        "candidates": [c.to_dict() for c in candidates],
    }
