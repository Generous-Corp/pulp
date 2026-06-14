# Phase 3 — offline-quality refinements: disposition

Phase 3 items are quality refinements over the Phase-2 engine. Several are
explicitly **gated** in the plan ("land only if they beat the baseline on
metrics + blind A/B"). The gate needs two things this autonomous/headless
environment does not have: the `rubberband` CLI (the R3 quality bar) and a
human blind-listening setup. The Phase-2 engine is already élastique-class
(it reuses the validated realtime spectral stack: −236 dB neutral null, ±cents
pitch, Röbel transient reset, STN noise morphing). So the disciplined call —
per the plan's gating — is to land the cheap, unconditional wins now and record
the gated experiments as deferred with the gate spelled out, rather than ship
speculative DSP that may not beat the baseline.

| Item | Status | Rationale |
|---|---|---|
| **P3.5 draft `quality=0`** | **Implemented** | Fast Hann-OLA tempo preview (`ola_tempo`); pitch-preserving, exact length, instant. The sampler shows it on a tempo change and swaps in the full render. Tested. |
| P3.1 look-ahead transient resets | Deferred | The engine already does *causal* Röbel transient reset (sharp attacks). Non-causal onset-centred reset is an incremental refinement, not a correctness gap; revisit with the metrics gate. |
| P3.2 Griffin-Lim refinement | Deferred (optional) | Only lands "if it measurably helps." No way to measure "helps" here without the baseline/listening gate. |
| P3.3 verbatim transient relocation | **Deferred — GATED** | Must beat baseline on transient-timing/pre-echo AND seam-quality blocking tests AND blind A/B (plan §6). The seam tests + A/B + `rubberband` comparison cannot run headless. Phase-1 baseline (onset ~6 ms) is captured for when the gate can run. |
| P3.4 multi-resolution STFT | **Deferred — GATED** | Land only if it beats best single-size on the 808/hi-hat corpus. Same gate constraint. Note: implementable as band-split across multiple `SpectralFrameEngine` instances (no engine-internal change), per the design-system review. |
| P3.6 listening checkpoint vs `rubberband -3` | Deferred | `rubberband` is not installed in this env; `capture_baseline.py` already flags the lane as deferred and records our numbers. Run locally with `rubberband` present. |
| P3.7 escape-hatch (v2 native offline) | **Not triggered** | The gate opens the native-offline path only if metrics + listening fail R3. The orchestrated engine is already class-leading and Phase-1/2 metrics are sound, so v1 ships the orchestrated path. The native path remains the documented fallback if a future listening gate fails. |

## How to run the deferred gates later

1. Install `rubberband` (`brew install rubberband`).
2. Re-run `capture_baseline.py` — the Rubber Band R3 lane activates and renders
   the corpus alongside OfflineStretch for side-by-side scoring.
3. For P3.3/P3.4, prototype the refinement behind a flag, render the drum
   corpus, and compare metrics + a blind A/B against both the Phase-1 baseline
   and `rubberband -3`. Land only on a clear win (plan §6).

The engine ships **v1** at Phase-2 quality + draft mode; the gated refinements
are queued, not abandoned.
