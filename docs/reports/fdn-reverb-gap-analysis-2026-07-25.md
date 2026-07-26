# FDN reverb prompt gap analysis

Status: local closure validated
Source contract: `fdn-reverb-pulp-module-prompt.md` supplied 2026-07-25
Audited revisions: Pulp `e92a7fb2f`; Forge `e3f69bed5b`

## Goal

Close every actionable FDN implementation and acceptance-proof gap without
undoing justified design corrections, and leave each internally inconsistent
prompt requirement recorded with an honest executable contract.

## Verdict

The landed work contains the requested 16-line multirate FDN, all five catalog
modes, all twelve parameters, the full in-loop and output topology, Forge
registry/range contracts, and most behavioral acceptance coverage. It was not
fully complete against the prompt: five implementation details and seven
acceptance proofs were missing or materially weaker than the normative text.
Two prompt requirements are internally inconsistent and cannot be implemented
simultaneously with other mandated constants. A third apparent conflict—the
post-Hadamard ±4 guard versus the tank's ±10 emergency ceiling—is not a logical
contradiction; the narrower matrix boundary is now implemented as specified.

This report is the closure ledger. A row is complete only when the named source
or executable test exists and passes in Release.

## Requirement ledger

| Prompt area | Evidence | Audit verdict |
|---|---|---|
| Header-only `FdnReverbT`, wet-only stereo, zero-allocation lifecycle | `fdn_reverb.hpp`; `forge_fdn_reverb_catalog.hpp`; catalog RT probe | Closed: reset is allocation-probed through the catalog lifecycle, and a planted allocation proves the instrument can fail. |
| Five stamped modes and twelve canonical baked parameters | Pulp mode/parameter tables; Pulp catalog contract tests; Forge generated registry and voice-loader derivation | Complete. Forge uses underscore-form agent aliases while Pulp's stored node IDs retain the required dotted IDs; these are different identifier namespaces. |
| 16 lines, normalized four-stage Hadamard, active-channel scaling | `fdn/tank.hpp`; structural/stability tests | Closed: the normative post-matrix finite kill and ±4 clamp now precede the wider ±10 write-boundary emergency ceiling. |
| Additive prime delay lengths | `fdn/tank.hpp`; structural equation test | Closed: nearest-sample `round(base * rate)` replaces the landed `floor`. |
| Jot decay, proportional damping, diffusion-inclusive loop length | `fdn/tank.hpp`; T60/damping tests | Complete. The loop diffusion delay is correctly included in the Jot round trip. |
| Eight-rate persistent-phase bridge, Hermite interpolation, shared Butterworth coefficients with separate state | `fdn/multirate.hpp`; block-partition test | Complete with one justified correction: exact-position Hermite is used on the downsampling leg instead of the prompt pseudocode's phase-crossing sample hold. It preserves non-integer timing and improves reconstruction. |
| Hard, allocation-free rate switch that flushes every affected state | engine and catalog switch tests | Closed: ducker envelopes, ensemble delay/LFO state, host output-filter state, duck telemetry, and control cadence now reset with the already-flushed bridge/tank/input state. |
| Two diffusion stages, flutter, sine plus fixed-seed walk modulation | `fdn/diffusion.hpp`, `fdn/modulation.hpp`; modulation/determinism tests | Complete. |
| Damping, ten-band EQ, flux, shimmer, Lipschitz saturation, closed-form stability normalization, Bloom | `fdn/loop_eq.hpp`, `fdn/shimmer.hpp`, `fdn/tank.hpp`; stability/Bloom/shimmer tests | Complete with two justified corrections. Flux is absorptive-only because a positive in-loop boost forces the stability normalizer to shorten broadband T60. The Bloom test derives its near-freeze bound from `kGainCeil=0.999`; the prompt's simultaneous “less than 0.5 dB loss in 30 s” cannot follow from that ceiling and the specified delay lengths. |
| Drive perceived-level makeup outside recursion | output path in `fdn_reverb.hpp` | Closed: the named drive makeup is applied at the wet output tap before the downstream output stage, never in feedback. |
| Ducker, width, tank-rate ensemble, limiter, mode lowpass, final rate makeup | `fdn/stages.hpp`, `fdn_reverb.hpp`; ducker/makeup/mode tests | Complete. |
| Defense-in-depth non-finite recovery | stage-local guards plus post-Hadamard/write sanitizers | Closed: allpass, damping, EQ/flux biquad, shimmer tone-filter, resampler-filter, and envelope updates now kill non-finite state locally. |
| Full deterministic stability fuzz | hidden `[fdn-fuzz-full]` cases: 200 vectors × 60 seconds × 8 rates | Closed: all eight exact per-rate sweeps pass. The routine gate preserves the same distribution over 64 vectors at representative rates plus one 60-second render at every rate. |
| T60, damping, density, color, Bloom, makeup, shimmer, ducker | focused engine suite | Closed: the dominant-mode test now reverse-integrates literal FFT-bin energy decays and compares every resolved bin with its third-octave median. |
| Tank-rate bandwidth | engine suite | Closed: the test locates the last -3 dB crossing at 16/20/24 kHz and probes 0.45 times host Nyquist explicitly at 96 kHz. |
| Rate-switch cold parity | engine and catalog suites | Closed: both switch directions compare the entire output bit-for-bit and compare T60 with a cold target-rate render. |
| Saturation behavior | decay-neutrality and analyzer-backed THD tests | Closed: shipped analysis tooling observes monotonic output THD at drive 0, 0.5, and 1. |
| Determinism and timing | 30-second bit-exact/reset/block-partition test; direct bridge test | Closed: determinism uses the required duration, and skew is measured directly against the bridge's honest causal-interpolator bound. |

## Prompt contradictions retained as explicit exceptions

### Bloom

With a per-pass ceiling of `0.999` and 10–90 ms loop times, 30 seconds contains
hundreds of passes. The resulting loss is far greater than 0.5 dB. The engine
retains the prompt's mathematically derived strict ceiling and tests a
near-freeze relative to the requested Jot decay, rather than weakening the
stability proof.

### Interpolation skew

A causal four-point Hermite read at tank position `p` needs source samples
through `floor(p)+2`. On the tank-to-host leg at 16 kHz into a 48 kHz host,
those two future tank samples alone span six host samples. Therefore the
prompt's universal “at most four host samples” bound is incompatible with its
mandated interpolator and lowest ratio. The API continues to report zero
bufferable host latency and exposes an honest ratio-dependent interpolation
bound. The closure test measures the bridge directly against that bound; it
does not erase the skew by subtracting two predelays.

## Closure checklist

- [x] Change delay-length conversion to nearest-sample rounding and pin it.
- [x] Add the specified post-Hadamard finite kill and ±4 overflow clamp.
- [x] Reset every rate-domain and downstream wet state on a live rate change.
- [x] Add named output-tap drive makeup outside the feedback recursion.
- [x] Add local finite-state recovery to recursive allpass/filter/damping paths.
- [x] Add reset coverage and a planted-allocation negative control to the RT test.
- [x] Add dominant-mode lifetime coverage.
- [x] Extend bit-determinism to the required 30-second render.
- [x] Measure monotonic output THD across the drive sweep.
- [x] Compare switched state bit-for-bit and by T60 with a cold target-rate render.
- [x] Replace the latency proxy with a direct bridge-skew measurement.
- [x] Locate and gate the actual low-rate -3 dB wet-bandwidth edge.
- [x] Gate the 96 kHz mode explicitly at 0.45 times host Nyquist.
- [x] Pass the focused Release suites and the full owner manifest.
- [x] Execute the opt-in 200-vector × 60-second × 8-rate sweep when host
      admission control permits it.
- [x] Complete adversarial review.

## Measured closure evidence

All local C++ measurements use the Release build in the dedicated closure
worktree.

| Contract | Result |
|---|---|
| Post-Hadamard recovery | Exact ±4 finite clamp plus NaN/+Inf → 0, pinned by 5 structural assertions. |
| Stage-local recovery | 37 assertions directly inject non-finite values into allpass, damping, EQ/flux, shimmer, ducker, Butterworth, both multirate extremes, and the complete engine. |
| Dominant modes | 1,166 literal FFT bins compared; worst T60 / third-octave median = 1.32617, below the 2.0 limit. |
| Low-rate bandwidth | Measured last -3 dB crossings: 0.390026 × 16 kHz, 0.372595 × 20 kHz, and 0.363259 × 24 kHz; all within 15% of the shipped 0.42 target. |
| 96 kHz bandwidth | Level at 0.45 × host Nyquist (10.8 kHz) = -4.83959 dB relative to the low-band reference. |
| Drive THD | 0.0000788626, 0.00250954, 0.00480737 at drive 0, 0.5, 1; strictly increasing. |
| Pulp engine suite | 193,096 assertions in 23 cases; the CTest owner manifest passes 31/31 registered tests. |
| Exhaustive stability sweep | All 8 rates pass 800 assertions each: 1,600 deterministic vectors, 6,400 assertions, and 26.7 rendered audio-hours. |
| Pulp catalog lifecycle | 804,597 assertions in 8 cases, including allocation-free reset and the planted probe control. |
| Forge parameter ranges | 808 assertions in 7 cases. |
| Forge capability contract | 2,502 assertions in 24 cases. |
| Adversarial review | Final diff-wide automated review found no actionable defects and rated the patch correct with 0.89 confidence. |
