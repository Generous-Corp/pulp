# Fast-trigonometry evaluation

This note records why Pulp ships bounded-cycle minimax sine profiles, where
they help, and where they do not. It is evidence for maintainers, not a promise
that every `sin` call should change. Reference math remains the default.

## Selected paths

Pulp adapts the degree-5 and degree-9 normalized-cycle sine expressions from
Lasse Schlör's [Fast MiniMax Polynomial Approximations of Sine and
Cosine](https://publik-void.github.io/sin-cos-approximations/), pinned and
attributed as described in [Licensing and acknowledgements](../reference/licensing.md).

- `FmOperatorEngineT<float>` may opt into degree 5 (`realtime_efficient`) or
  degree 9 (`realtime_precise`). The two-operator efficient consumer measured a
  13.26% whole-engine gain; reference remains the compatibility default.
- `AdditiveBankT<float>` may opt into `realtime_precise`. On Apple arm64/Clang it
  evaluates four partials together while retaining double phase accumulators,
  wrap-event behavior, gain/frequency ramps, and envelope state. Other targets
  preserve the scalar profile semantics but do not claim acceleration.
- FM8 research evaluated a degree-13 candidate because its harmonic reader can
  execute 32 double sine calls per sample. It passed performance but failed the
  predeclared broadened hit-quality gate, so no FM8 selector ships.
- Double FM6, all drum engines, scalar additive evaluation, setup-time trig,
  and test oracles remain on reference math because their consumer evidence did
  not justify or authorize a replacement.

## Adoption census

| Pulp surface | Current disposition | Why |
|---|---|---|
| Float FM operators | Opt-in degree 5 or 9; reference default | The efficient two-operator voice cleared the whole-consumer gate |
| Float additive banks | Opt-in degree 9; reference default | Apple arm64 four-wide evaluation cleared performance and audio gates; other targets preserve semantics without a speed claim |
| Double FM6 drum voices | `deferred_no_win` | The actual voice improved, but missed the 10% whole-consumer gate |
| FM8 drums | Reference; `deferred_quality_no_go` | Degree 13 cleared performance, but a broadened 0.75-second hit corpus found a −38.63% peak change, outside the predeclared ±25% bound |
| Other kick, snare, clap, cymbal, and percussion voices | Reference; not evaluated | FM6 did not clear its gate and FM8's distinct density does not authorize a call-site cascade |
| SSB/frequency shifting | `deferred_more_evidence` | A paired degree-13 experiment screened well, but lacks a second primary target and a platform paired/vector baseline |
| Non-Apple CPU and WebAssembly additive paths | Reference or scalar profile semantics | No whole-consumer acceleration has been established on those targets |
| GPU kernels | Native shader trig | CPU evidence does not establish a shader/device win; two named backend/device families are required |
| Setup, filters, FFT preparation, and test oracles | Reference | They are not demonstrated realtime bottlenecks, and oracles must remain independent |
| SDK/component authoring | Specific opt-in profiles | Public C++ consumer APIs own selection, and capability metadata advertises accepted trig support or effective state where implemented; there is no global fast-trig switch |
| Generated DSP | No separate fast-trig generator control | Generated consumers can use accepted component APIs, and the vocabulary exposes the FastMath primitive signature; it adds no top-level component selector or forked coefficient tables |
| Live Unified Control broker control | `deferred_missing_exact_live_consumer` | Add a granted live selector only when a named product binds an accepted component and can report its effective profile |
| Swift/Surge | `covered`, not adopted for realtime | [Surge at inspected commit `ac638794`](https://github.com/Jounce/Surge/blob/ac638794d4b02377e3628c1c8b1e07c2a15f1d52/Sources/Surge/Trigonometry/Trigonometric.swift) is an MIT Swift convenience layer over Accelerate/vForce; direct vForce already lost the float-additive realtime screen |

The existing [audio-harness skill](../../.agents/skills/audio-harness/SKILL.md)
owns the repeatable DSP measurement workflow. A fast-trig-specific skill would
duplicate it, so the executable benchmarks and this decision record are the
maintenance surface. Production degree-5/9 attribution lives in `NOTICE.md`;
its source and permission details live in
[Licensing and acknowledgements](../reference/licensing.md).

## How the additive decision was tested

The benchmark is advisory and Release-only; timing never gates heterogeneous
CI. The recorded run used an Apple M3 Ultra, macOS 26.6.2, Apple Clang 21.0.0,
arm64, and verified both the CMake cache and target flags contained
`-O3 -DNDEBUG`.

Screening first rendered 64 float partials for 4,096 frames, 15 trials and five
passes per trial. It compared scalar `sinf`, scalar degree 9, Apple
`simd::sinpi`, four-wide degree 9, Accelerate/vForce `vvsinpif`, and oscillator
recurrence. The four-wide polynomial was 41.5–45.5% faster than scalar `sinf`
across three independent processes. `simd::sinpi` gained only 9–14%, vForce was
57–69% slower, and recurrence accumulated `1.82e-4` maximum error within 4,096
frames despite a 29–38% speed gain.

The winning candidate was then measured inside the real `AdditiveBankT<float>`
consumer after profile dispatch was hoisted out of the partial loop so the
reference lane retained its original hot expression. A 64-partial organ at
48 kHz produced 47.8%, 47.8%, and 48.9% whole-bank gains across three
independent processes. The pilot's maximum rendered difference was one float
ULP.

Finally, three independent 54-cell matrices covered:

- organ and bell voices (the bell uses detuned doublets);
- 44.1, 48, and 96 kHz;
- 16, 64, and 128 partials; and
- 32, 128, and 512-sample host blocks.

Each cell used 4,096 frames, nine trials, three passes, weighted full-buffer
checksums emitted for both lanes, and alternating reference/candidate order.
Every cell won. Minimum gains were
37.8–40.4%, median gains 49.2–49.9%, and p95 gains 64.4–65.3% across three
corrected processes. The final rebased full-checksum rerun remained consistent:
39.80% minimum, 47.59% median, and 63.83% p95 across the 54 cell-median gains.
The benchmark also emits each lane's p95 render cost rather than conflating it
with that across-cell summary. Focused
acceptance additionally covers odd remainders, organ/bell outputs, doublets,
the public 128-partial maximum (256 rendered bell-doublet slots), allocation
freedom, a maximum two-float-LSB rendered-output deviation, and the existing
−100 dB THD requirement.

The deployed Forge-style `next()` shape was also measured directly against
unmodified Pulp `dcd739c18db`. Seven independent, alternating-order Release
pairs rendered a 64-partial organ for 32,768 frames, 21 trials and five passes.
The patched reference-default median was 434.80 ns/frame versus 447.90 ns/frame
on main; every pair was neutral or faster (1.38–8.10%, median 2.89%). The
profile dispatch therefore introduces no measured reference regression in the
per-sample caller that the block benchmark does not model.

Build and run locally with:

```sh
cmake -S . -B build-fast-trig -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BENCHMARK=ON -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF
cmake --build build-fast-trig \
  --target pulp-fast-trig-benchmark pulp-fast-trig-apple-bank-benchmark
./build-fast-trig/test/pulp-fast-trig-apple-bank-benchmark
```

The rejected FM8 implementation, benchmark method, exact-base hash probe,
broadened quality corpus, and rerun commands are preserved on immutable
research snapshot
[`a860739e50c`](https://github.com/Generous-Corp/pulp/tree/a860739e50cc796702c339288a3b3f723c851d30/test/research/fast_trig_fm8).
They are deliberately absent from `main`.

## Periodic reassessment

Rerun after a material compiler, standard-library, SDK, CPU, or shader change,
or when a new candidate library offers a plausible consumer advantage. Record
the date, Pulp source SHA, dirty state, CPU/device, OS, compiler, Release flags,
benchmark schema, three independent summaries, quality result, and resulting
`adopt`, `covered`, `deferred_no_win`, `deferred_more_evidence`,
`deferred_quality_no_go`, or `not_applicable` verdict. Append a new dated
finding; do not rewrite the historical row to make an old decision look current.

Nathan Blair's 2026
[root-factored sine candidate](https://x.com/nthnblair/status/2092672129338630389)
is one bounded reassessment input, not adopted Pulp code. Its published Apple M4
chart measures scalar float with signed phase already reduced and Clang
`-O3 -ffast-math`. The degree-11 form reports `2.80e-7` maximum float error and
about `-140.2 dB` THD; those are different metrics, and the peak error is
slightly outside Pulp's existing `realtime_precise` `2.5e-7` contract. Its
`x * (x^2 - 0.25)` factor does make the cycle-boundary roots exact.

If revisited, inject root-11 Horner and Estrin forms as local benchmark overlays
under normal Pulp production flags. Compare Horner in independent additive
lanes and Estrin in serial float FM/PM—or a named drum voice only when profiling
shows sine dependency latency is hot—against the shipped degree-9 backend. Keep
the existing error, audio, state-continuity, and 10% whole-consumer gates. A
winner may replace an implementation behind an existing semantic profile; it
does not add a Horner/Estrin control, weaken the error budget, or authorize
compiler-wide fast-math. The published degree-7 form (`4.02e-4` maximum error,
about `-71.3 dB` THD) has no clean general-audio role.

An unredistributed candidate can join the general primitive/FM/64-partial screen
without entering Pulp source. Supply a local header that defines
`pulp_fast_trig_local::report_candidates(report)` and calls `report(name,
callable)` for each bounded-cycle float sine, then configure with:

```sh
cmake -S . -B build-fast-trig-candidate -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BENCHMARK=ON \
  -DPULP_FAST_TRIG_LOCAL_CANDIDATES_HEADER=/absolute/path/candidates.hpp
cmake --build build-fast-trig-candidate --target pulp-fast-trig-benchmark
./build-fast-trig-candidate/test/pulp-fast-trig-benchmark
```

A screening win only authorizes an actual-consumer experiment. It does not
change defaults or supersede the audio, state-continuity, platform, licensing,
and whole-consumer gates below.

The paired SSB experiment is preserved on the non-production
[`64150c552359` research snapshot](https://github.com/Generous-Corp/pulp/tree/64150c55235957f8df3d8f36a9ca7720d7fa2af3/test/research/fast_trig_pair).
Its README contains exact benchmark/test rerun commands, raw summaries,
quantile conventions, quality coverage, and preserved historical selector-probe
outputs. The compile-time probe source is retained, but the rejected runtime
patch and an exact probe runner are not; do not describe those historical probe
results as fully rerunnable. Keep the snapshot as an overlay; its degree-13
pair and public aliases are deliberately absent from `main`.

## Dated findings

| Date | Consumer and candidate | Evidence | Decision |
|---|---|---|---|
| 2026-08-26 | Two-operator float FM, degree 5 | 13.26% whole-engine gain | Adopt as opt-in `realtime_efficient` |
| 2026-08-26 | Double FM6/drum voice, degree 13 | 6.79–7.09% median and 10.62–12.14% p95 across 36 cell medians | `deferred_no_win`; do not cascade into lower-density drums |
| 2026-08-26 | Apple arm64 float additive bank, four-wide degree 9 | 39.80% minimum, 47.59% median, and 63.83% p95 across 54 final cell medians | Adopt as opt-in `realtime_precise` |
| 2026-08-26 | Double FM8 drum voice, degree 13 | Immutable research snapshot: 24.16–25.07% minimum, 33.00–33.30% median, and 38.69–40.83% p95 across 36 cell medians; exact-base sample hash matched in seven pairs; broadened hit corpus found peak ratio `0.613740` for algorithm 9/wave base 12/48 kHz | `deferred_quality_no_go`; preserve research, ship no selector |
| 2026-08-26 | Double SSB pair, degree 13 | 9.91% minimum, 12.38% median, and 19.02% p95 across 36 cell medians; maximum error `1.05e-13`; image, carrier, DC, determinism, and 10-second stability screens passed | `deferred_more_evidence`; preserve research harness, not production code |
| 2026-08-26 | External root-factored degree-11 Horner/Estrin intake | Published M4 primitive chart only: already-ranged scalar float with `-ffast-math`; `2.80e-7` maximum error and about `-140.2 dB` THD | Candidate intake only; run normal-flag Pulp consumer matrices before any source or control change |

The Pulp rows are measurements of the named consumers and environment, not
universal claims about a primitive; the external-intake row is explicitly not a
Pulp measurement. FM8 was evaluated separately because its per-sample harmonic
reader has materially different trig density from FM6. Its failed quality result
does not authorize another drum: each candidate still requires an actual-voice
matrix, representative hit corpus, and at least a 10% gain while preserving the
existing audio and state-continuity gates.

The exact two-operator FM and double-FM6 whole-consumer executables were not
retained on `main` or an immutable public research snapshot, so those two
historical figures are not exactly rerunnable. The general benchmark above can
rescreen primitive and FM-like shapes, but a renewed decision must construct and
preserve a current actual-consumer experiment under the audio-harness contract.

## Alternatives and reopen rules

| Alternative | Result | Reconsider only when |
|---|---|---|
| Apple `simd::sinpi` | Correct but much smaller gain in the float-additive screen than degree 9 | A compiler/platform change reverses that actual-consumer result; the SSB paired/vector baseline remains unmeasured |
| Accelerate/vForce batch | Slower for a per-sample 64-partial batch | Batching spans a materially different consumer layout |
| [Swift Surge at `ac638794`](https://github.com/Jounce/Surge/blob/ac638794d4b02377e3628c1c8b1e07c2a15f1d52/Sources/Surge/Trigonometry/Trigonometric.swift) | Convenience façade over Accelerate/vForce, with array allocation and unit-stride batch APIs; no new approximation or C++ realtime advantage | A named Swift-owned offline/batch consumer needs its broader ergonomic API; benchmark direct Accelerate first |
| Oscillator recurrence | Fast, but short-run drift missed the quality objective | A bounded renormalization scheme clears long-hold and consumer gates |
| Scalar degree 9 additive | Slower than reference in prior 64-partial tests | New scalar target/compiler evidence exists |
| Degree-13 double FM6 | Median whole-voice gain only 6.8–7.1%, below the 10% gate | A materially new vectorized double design is proposed |
| Degree-13 double FM8 | Strong speed win, but recursive hit peak changed −38.63% in the broadened corpus | A materially different implementation or explicitly different timbral product contract declares a new quality gate before measurement |
| 2017 Chebyshev approximation | No current Pulp role after the selected degree-9 path won on both speed and quality objectives | A new objective or target demonstrates a whole-consumer advantage over the selected path |
| 2026 root-factored degree-11 Horner/Estrin | Promising external primitive result, but not measured under Pulp flags or consumers and its published peak error exceeds the precise profile budget | A local-overlay screen clears the existing error gate, then Horner wins an independent-lane consumer or Estrin wins a measured dependent FM/PM or drum consumer by at least 10% |
| RLIBM-ALL | Correct-rounding general-purpose scope exceeds realtime bounded-phase need | Correct rounding becomes a stated requirement, or an applicable double-pair design enters an actual-consumer screen |
| Integer SIMD or assembly | Not required to establish the portable/Apple SIMD win | A portable integer prototype first clears conversion and accuracy gates, then target assembly demonstrates enough additional consumer value to justify its maintenance cost |
| Compiler-wide fast-math | Rejected; changes unrelated floating-point contracts | Never as part of this facility; benchmark only as separately labeled research |

The reopen condition is consumer evidence, not a faster primitive in isolation.
Do not replace cached/setup trig, reference oracles, quadrature, GPU native trig,
or other components without their own performance and audio-quality gates.
