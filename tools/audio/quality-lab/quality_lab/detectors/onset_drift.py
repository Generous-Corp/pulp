"""Onset drift — timing/groove damage (event-time residual). EXPERIMENTAL.

No other detector measures whether events land at the right *place in time*: a
stretch/quantize/latency bug can shift a hit a few ms (loose groove) while every spectral
detector reads "clean". This was deferred once because a body-correlation timing measure
couldn't tell identity from a 7 ms drift against a tonal kick's quasi-periodic body — so
this implementation is (a) percussive-only and (b) measures the EVENT-TIME residual after
removing the common latency, not a raw lag.

Per matched onset, the candidate's fine event time is found by cross-correlating the
reference attack against the candidate searched around the EXPECTED time `ref_t`
(`dsp.local_align`, sub-hop precision — beats the ~2.67 ms onset hop):

    delta_i  = (candidate event time) - (expected event time ref_t)
    residual_i = delta_i - median(delta)      # remove the common, benign latency

A uniform latency → all delta_i equal → residuals ~0 (reported as `common_latency_ms`,
not a defect); jitter or a growing ramp → residuals grow → fires. Headline scalar =
max |residual_i| in ms. If drift exceeds the cross-correlation search window the onset is
dropped (not invented), so heavy drift reads low-coverage UNCERTAIN rather than a
confident-but-wrong number.

Ships `experimental`: it runs and reports but is advisory — its `fired` never moves the
verdict or the regression gate until its calibration is proven and it is promoted.
"""
from __future__ import annotations

import numpy as np

from ..dsp import local_align, theil_sen_slope
from ..schema import DetectorResult, WorstRegion

TOLERANCE_CLASS = "onset_drift.v1"
SEARCH_MS = 12.0  # cross-correlation search half-window; drift beyond this → low coverage


def _empty(expected: int, note: str) -> DetectorResult:
    return DetectorResult(
        name="onset_drift", scalar=0.0, unit="onset_drift_ms", fired=False,
        time_domain="aligned", measured=0, expected=expected, maturity="experimental",
        tolerance_class=TOLERANCE_CLASS, notes=note,
    )


def detect(reference, candidate, sr, onset_pairs=None, fire_threshold_ms: float = 1.5) -> DetectorResult:
    """Max per-onset event-time residual (ms) after removing the common latency.
    Percussive-only: with no onset pairs (identity/tonal families) there is nothing to
    measure → UNCERTAIN-shaped empty result."""
    pairs = list(onset_pairs or [])
    expected = len(pairs)
    if expected == 0:
        return _empty(0, "no onset pairs (onset_drift is percussive-only)")

    search = int(SEARCH_MS * sr / 1000.0)
    saturate = int(0.95 * search)
    ref_ts: list[float] = []
    lags: list[float] = []  # candidate event-time offset from ref_t, in samples
    for ref_t, _cand_t in pairs:
        # Search the candidate around the EXPECTED time ref_t (not the candidate's own
        # detected onset) so the lag IS the absolute event-time error at this onset.
        rseg, _cseg, lag = local_align(reference, candidate, sr, ref_t, ref_t, search_ms=SEARCH_MS)
        if rseg is None:
            continue  # window ran off the array — not measured
        if abs(lag) >= saturate:
            continue  # drift beyond the search window — refuse to invent a timing answer
        ref_ts.append(float(ref_t))
        lags.append(float(lag))

    measured = len(lags)
    if measured == 0:
        return _empty(expected, "no onset measured within the search window")

    lags_a = np.asarray(lags)
    common = float(np.median(lags_a))            # robust common offset = uniform latency
    residual_ms = (lags_a - common) / sr * 1000.0
    scalar = float(np.max(np.abs(residual_ms)))
    common_latency_ms = common / sr * 1000.0
    slope = theil_sen_slope(np.arange(measured), lags_a / sr * 1000.0)  # ms per onset
    jitter_ms = float(np.median(np.abs(residual_ms - np.median(residual_ms))))  # MAD

    wi = int(np.argmax(np.abs(residual_ms)))
    worst = [WorstRegion(time_s=ref_ts[wi], severity=scalar, detector="onset_drift",
                         label=f"{residual_ms[wi]:+.1f}ms vs groove")]
    return DetectorResult(
        name="onset_drift", scalar=scalar, unit="onset_drift_ms",
        fired=scalar >= fire_threshold_ms, time_domain="aligned",
        measured=measured, expected=expected, maturity="experimental",
        tolerance_class=TOLERANCE_CLASS, worst_regions=worst,
        notes=(f"max|residual|={scalar:.2f}ms common_latency={common_latency_ms:+.2f}ms "
               f"slope={slope:+.3f}ms/onset jitter={jitter_ms:.2f}ms"),
    )
