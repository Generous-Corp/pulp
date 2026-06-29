"""Real-engine regression gate (§5/§7 — guard the invariant, on the product engine).

The lab's core promise is to catch when a DSP change makes the sound *worse*. This
module makes that concrete for the stretch engine: it captures the detector scalars the
*current* `OfflineStretch` produces across a small ratio x character matrix, freezes them
as a committed baseline, and flags when a future engine build deviates beyond tolerance.

Deterministic (the engine is deterministic and the corpus is synthetic), so a deviation
is a real engine change, not analyzer noise. Opt-in: requires a built stretchcli; the
check skips cleanly when it isn't present (public CI doesn't build it).
"""
from __future__ import annotations

import json
import os
from typing import Any

from . import pipeline

# The matrix the baseline pins. Small + fast; covers two ratios and two characters whose
# documented behaviors differ (clean = attack smear; varispeed = brightness dulling that
# grows with ratio).
MATRIX = [(1.5, "clean"), (1.5, "varispeed"), (2.0, "clean"), (2.0, "varispeed")]

_BASELINE_PATH = os.path.join(os.path.dirname(__file__), "baselines", "stretchcli.json")
TOLERANCE = 0.03  # absolute scalar tolerance (same-engine deterministic; catches real shifts)


def capture() -> dict[str, dict[str, float]]:
    """Run the real engine across the matrix; return {case_key: {detector: scalar}}."""
    out: dict[str, dict[str, float]] = {}
    for ratio, char in MATRIX:
        report = pipeline.run_real_engine(ratio=ratio, character=char)
        if report.get("verdict") in ("SKIPPED", "ERROR"):
            raise RuntimeError(f"engine not available/failed: {report}")
        # Only baseline-participating detectors enter the regression gate — experimental
        # (and beta) detectors are advisory and must never fail it. The flag defaults True
        # so pre-maturity-gate reports/baselines still load.
        out[f"ratio={ratio},character={char}"] = {
            d["name"]: round(d["scalar"], 4)
            for d in report["detectors"]
            if d.get("participates_in_engine_baseline", True)
        }
    return out


def load_baseline() -> dict[str, Any]:
    with open(_BASELINE_PATH) as f:
        return json.load(f)


def write_baseline(matrix: dict[str, dict[str, float]]) -> str:
    os.makedirs(os.path.dirname(_BASELINE_PATH), exist_ok=True)
    payload = {"schema_version": 1, "tolerance": TOLERANCE,
               "note": "real OfflineStretch detector scalars; regenerate with `engine-baseline --capture`",
               "matrix": matrix}
    with open(_BASELINE_PATH, "w") as f:
        json.dump(payload, f, indent=2)
    return _BASELINE_PATH


def compare(
    baseline: dict[str, dict[str, float]],
    current: dict[str, dict[str, float]],
    tolerance: float = TOLERANCE,
) -> list[dict[str, Any]]:
    """Pure comparison (no engine) — list deviations beyond tolerance. For
    transient_sharpness a higher deficit is *worse*; any deviation is surfaced for
    review (guard the invariant, not the exact sound)."""
    deviations: list[dict[str, Any]] = []
    for key, dets in baseline.items():
        cur = current.get(key, {})
        for name, base_val in dets.items():
            cur_val = cur.get(name)
            if cur_val is None:
                deviations.append({"case": key, "detector": name, "issue": "missing in current"})
                continue
            delta = cur_val - base_val
            if abs(delta) > tolerance:
                deviations.append({
                    "case": key, "detector": name, "baseline": base_val, "current": cur_val,
                    "delta": round(delta, 4), "worse": name == "transient_sharpness" and delta > 0,
                })
    return deviations


def check(tolerance: float = TOLERANCE) -> list[dict[str, Any]]:
    """Compare the CURRENT engine to the committed baseline (needs a built stretchcli)."""
    return compare(load_baseline()["matrix"], capture(), tolerance)
