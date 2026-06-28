# OfflineStretch Quality Refinement Notes

These items are quality refinements over the current OfflineStretch engine.
Several are explicitly gated: land them only if they beat the checked-in
baseline on metrics plus blind A/B. The gate needs two things this
autonomous/headless environment does not have: the `rubberband` CLI quality bar
and a human blind-listening setup. The current engine already reuses the
validated realtime spectral stack (neutral null, accurate pitch, Robel
transient reset, STN noise morphing), so the disciplined call is to land the
cheap unconditional wins now and record the gated experiments as deferred rather
than ship speculative DSP that may not beat the baseline.

| Item | Status | Rationale |
|---|---|---|
| **Draft `quality=0`** | **Implemented** | Fast Hann-OLA tempo preview (`ola_tempo`); pitch-preserving, exact length, instant. The sampler shows it on a tempo change and swaps in the full render. Tested. |
| Look-ahead transient resets | Deferred | The engine already does *causal* Robel transient reset (sharp attacks). Non-causal onset-centered reset is an incremental refinement, not a correctness gap; revisit with the metrics gate. |
| Griffin-Lim refinement | Deferred (optional) | Only lands if it measurably helps. No way to measure that here without the baseline/listening gate. |
| Verbatim transient relocation | **Implemented opt-in** | `relocate_transients` / `verbatim_relocate` grafts detected attacks back onto tempo-only spectral renders, with tests covering peak restoration and tonal no-op behavior. Keep it opt-in unless a future quality gate proves it should become a default. |
| Multi-resolution STFT | **Deferred - gated** | Land only if it beats best single-size on the 808/hi-hat corpus. Same gate constraint. Implementable as band-split across multiple `SpectralFrameEngine` instances without changing the engine core. |
| Listening checkpoint vs `rubberband -3` | Deferred | `rubberband` is not installed in this env; `capture_baseline.py` already flags the lane as deferred and records our numbers. Run locally with `rubberband` present. |
| Native-offline escape hatch | **Not triggered** | The gate opens the native-offline path only if metrics plus listening fail against the quality bar. The current engine is already strong enough to ship the orchestrated path. The native path remains the documented fallback if a future listening gate fails. |

## How to run the deferred gates later

1. Install `rubberband` (`brew install rubberband`).
2. Re-run `capture_baseline.py` - the Rubber Band R3 lane activates and renders
   the corpus alongside OfflineStretch for side-by-side scoring.
3. For a gated refinement, prototype behind a flag, render the drum corpus, and
   compare metrics plus a blind A/B against both the checked-in baseline and
   `rubberband -3`. Land only on a clear win.

The engine ships with the current orchestrated quality path, draft mode, and
opt-in verbatim transient relocation; the remaining gated refinements are queued,
not abandoned.
