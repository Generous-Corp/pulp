"""MIR structural oracles (opt-in, license-fenced) — advisory cross-checks, NOT metrics.

This is a *separate* layer from `perceptual.py`. Those tools are full-reference
perceptual **quality** predictors (a MOS/ODG scalar: "is it worse overall?"). aubio is a
feature **extractor** (onset / beat / pitch / MFCC) — it produces structural features,
not a quality score, so it does not belong beside ViSQOL/PEAQ. Its value here is as an
INDEPENDENT second opinion on timing/structure: an external re-implementation that can
expose blind spots in the lab's own (pure-NumPy) onset machinery — e.g. to help graduate
the experimental `onset_drift` detector via non-circular validation.

It is **advisory, never a gate, never a committed baseline** (a GPL/dev-local oracle must
never become required validation evidence). Same license fence as Layer B: reached ONLY
across a process boundary, ONLY via `PULP_AUBIO_BIN`, no bundling/import/auto-download,
and `skipped` when absent (always in public CI).

`PULP_AUBIO_BIN` is expected to be the aubio multi-tool binary; this adapter invokes its
`onset` subcommand (`aubio onset <file>`) on the reference and the candidate and compares
the recovered onset structure. aubio is GPL-3.0 — developer-supplied only.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from typing import Any

AUBIO_ENV = "PULP_AUBIO_BIN"


def _resolve(env_var: str) -> tuple[str | None, str]:
    path = os.environ.get(env_var, "").strip()
    if not path:
        return None, f"{env_var} not set (opt-in MIR oracle; skipping)"
    resolved = shutil.which(path) or (path if os.path.exists(path) else None)
    if not resolved:
        return None, f"{env_var}={path} not found on disk/PATH; skipping"
    return resolved, ""


def count_onsets(text: str) -> int:
    """Count onset events in aubio's stdout. aubio prints one event time (seconds) per
    line; a line counts as an onset if it contains a number. Blank/non-numeric lines are
    ignored. This is a coarse, advisory signal — exact per-onset times are not needed."""
    n = 0
    for line in text.splitlines():
        if re.search(r"-?\d+\.\d+|\b\d+\b", line):
            n += 1
    return n


def _aubio_onsets(binary: str, wav: str, timeout_s: float) -> int:
    proc = subprocess.run(
        [binary, "onset", wav],
        capture_output=True, text=True, timeout=timeout_s,
    )
    return count_onsets(proc.stdout)


def run_aubio(reference_wav: str, candidate_wav: str, timeout_s: float = 180.0) -> dict[str, Any]:
    """Independent onset-structure cross-check via aubio (`PULP_AUBIO_BIN`). Advisory;
    `skipped` when the tool isn't installed. Reports onset counts on the reference and
    candidate and their delta — a coarse, model-independent signal that timing structure
    shifted (a large delta is a hint to investigate, never a verdict)."""
    binary, reason = _resolve(AUBIO_ENV)
    if binary is None:
        return {"tool": "aubio", "status": "skipped", "reason": reason,
                "role": "onset_drift_cross_validation", "onset_count_delta": None}
    try:
        onsets_ref = _aubio_onsets(binary, reference_wav, timeout_s)
        onsets_cand = _aubio_onsets(binary, candidate_wav, timeout_s)
        return {"tool": "aubio", "status": "ok", "advisory": True,
                "role": "onset_drift_cross_validation",
                "onsets_ref": onsets_ref, "onsets_cand": onsets_cand,
                "onset_count_delta": onsets_cand - onsets_ref}
    except subprocess.TimeoutExpired:
        return {"tool": "aubio", "status": "error", "onset_count_delta": None,
                "role": "onset_drift_cross_validation", "reason": "timeout"}
    except Exception as exc:  # never let an opt-in tool break the run
        return {"tool": "aubio", "status": "error", "onset_count_delta": None,
                "role": "onset_drift_cross_validation", "reason": str(exc)}


def evaluate(reference_wav: str, candidate_wav: str) -> list[dict[str, Any]]:
    """All available MIR structural oracles for a (reference, candidate) WAV pair.
    Advisory; each entry degrades to `skipped` independently when its tool isn't
    installed. These are cross-validation signals, not quality metrics or gates."""
    return [run_aubio(reference_wav, candidate_wav)]
