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
- Double FM6/drum synthesis, scalar additive evaluation, setup-time trig, and
  test oracles remain on reference math because their consumer evidence did
  not justify a replacement.

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

## Periodic reassessment

Rerun after a material compiler, standard-library, SDK, CPU, or shader change,
or when a new candidate library offers a plausible consumer advantage. Record
the date, Pulp source SHA, dirty state, CPU/device, OS, compiler, Release flags,
benchmark schema, three independent summaries, quality result, and resulting
`adopt`, `covered`, `deferred_no_win`, or `not_applicable` verdict. Append a new
dated finding; do not rewrite the historical row to make an old decision look
current.

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

## Alternatives and reopen rules

| Alternative | Result | Reconsider only when |
|---|---|---|
| Apple `simd::sinpi` | Correct but much smaller modeled gain than degree 9 | A compiler/platform change reverses the actual-consumer result |
| Accelerate/vForce batch | Slower for a per-sample 64-partial batch | Batching spans a materially different consumer layout |
| Oscillator recurrence | Fast, but short-run drift missed the quality objective | A bounded renormalization scheme clears long-hold and consumer gates |
| Scalar degree 9 additive | Slower than reference in prior 64-partial tests | New scalar target/compiler evidence exists |
| Degree-13 double FM6 | Median whole-voice gain only 6.8–7.1%, below the 10% gate | A materially new vectorized double design is proposed |
| 2017 Chebyshev approximation | No speed/quality role after the selected degree-9 path won | A new objective or target demonstrates a consumer advantage |
| RLIBM-ALL | Correct-rounding general-purpose scope exceeds realtime bounded-phase need | Correct rounding becomes a stated consumer requirement |
| Integer SIMD or assembly | Not required to establish the portable/Apple SIMD win | Portable intrinsics are exhausted and conversion, accuracy, and maintenance costs are measured |
| Compiler-wide fast-math | Rejected; changes unrelated floating-point contracts | Never as part of this facility; benchmark only as separately labeled research |

The reopen condition is consumer evidence, not a faster primitive in isolation.
Do not replace cached/setup trig, reference oracles, quadrature, GPU native trig,
or other components without their own performance and audio-quality gates.
