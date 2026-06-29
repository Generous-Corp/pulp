"""Layer D — advisory LLM / multimodal reviewer (opt-in, NEVER a gate).

A reviewer reads the run's report (and optionally rendered clips / spectrograms) and
returns a plain-language "what sounds wrong and why" — catching novel or compound
artifacts no fixed detector encodes. It is **advisory only**: it never changes the
`verdict` or any gate (a confidently-wrong model must not be able to fail a good change).

It is reached ONLY across a process boundary, via a developer-supplied subprocess named by
an explicit env-path (`PULP_QLAB_REVIEWER_CMD`); skip-when-absent is the default and the
state in public CI. The lab itself performs no network calls and sends audio nowhere by
default — if the configured provider talks to a remote model, that is the developer's
explicit local configuration choice. The provider reads a JSON `{report, assets}` on stdin
and writes `{summary, suspected_artifacts[], confidence, notes}` JSON on stdout.
"""
from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
from typing import Any

REVIEWER_ENV = "PULP_QLAB_REVIEWER_CMD"


def _resolve(env_var: str) -> tuple[list[str] | None, str]:
    cmd = os.environ.get(env_var, "").strip()
    if not cmd:
        return None, f"{env_var} not set (opt-in advisory reviewer; skipping)"
    parts = shlex.split(cmd)
    binary = shutil.which(parts[0]) or (parts[0] if os.path.exists(parts[0]) else None)
    if not binary:
        return None, f"{env_var} provider {parts[0]!r} not found on disk/PATH; skipping"
    return [binary, *parts[1:]], ""


def _clamp01(v: Any) -> float | None:
    try:
        return max(0.0, min(1.0, float(v)))
    except (TypeError, ValueError):
        return None


def review(report: dict[str, Any], assets: dict[str, Any] | None = None,
           timeout_s: float = 120.0) -> dict[str, Any]:
    """Run the configured reviewer provider on a report (+ optional asset paths). Returns
    an advisory dict; status `skipped` when no provider is configured. NEVER raises and
    NEVER touches the verdict — the caller attaches the result under `advisory.reviewers`."""
    cmd, reason = _resolve(REVIEWER_ENV)
    if cmd is None:
        return {"reviewer": "external", "status": "skipped", "advisory": True, "reason": reason}
    payload = json.dumps({"report": report, "assets": assets or {}})
    try:
        proc = subprocess.run(cmd, input=payload, capture_output=True, text=True, timeout=timeout_s)
        if proc.returncode != 0:
            return {"reviewer": "external", "status": "error", "advisory": True,
                    "reason": (proc.stderr or proc.stdout).strip()[:200], "exit": proc.returncode}
        out = json.loads(proc.stdout)
        return {
            "reviewer": "external", "status": "ok", "advisory": True,
            "summary": str(out.get("summary", ""))[:2000],
            "suspected_artifacts": [str(a) for a in (out.get("suspected_artifacts") or [])][:20],
            "confidence": _clamp01(out.get("confidence")),
            "notes": str(out.get("notes", ""))[:2000],
        }
    except subprocess.TimeoutExpired:
        return {"reviewer": "external", "status": "error", "advisory": True, "reason": "timeout"}
    except json.JSONDecodeError:
        return {"reviewer": "external", "status": "error", "advisory": True,
                "reason": "provider did not return valid JSON"}
    except Exception as exc:  # never let an opt-in tool break the run
        return {"reviewer": "external", "status": "error", "advisory": True, "reason": str(exc)}


def attach(report: dict[str, Any], assets: dict[str, Any] | None = None) -> dict[str, Any]:
    """Run the reviewer and append its (advisory) result under `report['advisory']
    ['reviewers']`. The verdict is never read or changed. Returns the same report."""
    advisory = report.setdefault("advisory", {"detectors": [], "reviewers": [], "perceptual": []})
    advisory.setdefault("reviewers", []).append(review(report, assets))
    return report


def score_agreement(suspected: list[str], ground_truth: list[str]) -> dict[str, float]:
    """Validation harness: how well a reviewer's `suspected_artifacts` match the known
    ground-truth artifact labels of a synthetic case. Set-based precision / recall / F1 on
    case-folded labels — the answer-key metric for promoting the reviewer past experimental."""
    s = {x.strip().lower() for x in suspected if x.strip()}
    t = {x.strip().lower() for x in ground_truth if x.strip()}
    if not s and not t:
        return {"precision": 1.0, "recall": 1.0, "f1": 1.0}
    tp = len(s & t)
    precision = tp / len(s) if s else 0.0
    recall = tp / len(t) if t else 0.0
    f1 = (2 * precision * recall / (precision + recall)) if (precision + recall) else 0.0
    return {"precision": round(precision, 3), "recall": round(recall, 3), "f1": round(f1, 3)}
