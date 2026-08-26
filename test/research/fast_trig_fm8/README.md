# FM8 fast-trig research evidence

**Research-only outcome: `deferred_quality_no_go`.** The degree-13 candidate
cleared the whole-voice performance gate, but a broadened 0.75-second hit corpus
failed the predeclared peak-preservation bound. Algorithm 9 with wave base 12 at
48 kHz produced a candidate/reference peak ratio of `0.613740` (−38.63%) while
its RMS ratio remained `0.969703`. Do not ship this FM8 selector or treat the
performance result as authorization for another drum. Reopen only with a
materially different implementation or timbral contract and a newly declared
quality gate.

The production benchmark is Release-only and advisory. It measures the complete
`Fm8DrumVoice`, not the polynomial in isolation: parallel, branching, and
eight-deep serial/feedback voices at 44.1/48/96 kHz and 32/64/128/512-sample
blocks. Each of 36 cells times 16,384 frames with 15 trials and three
alternating-order passes. A separate 0.75-second render supplies full-hit error,
peak, RMS, DC, and coarse spectral-energy metrics at every sample rate.

The Apple M3 Ultra / Apple Clang 21.0.0 performance screen verified CMake Release and
target `-O3 -DNDEBUG`. Three clean processes measured 24.16–25.07% minimum,
33.00–33.30% median, and 38.69–40.83% p95 gains across cell medians. The focused
FM suite covers deterministic replay, block-partition invariance, active-hit
selector rejection, allocation freedom, finite output, and whole-hit
peak/RMS/DC/spectral bounds. The original three-scenario corpus passed; Ultra
review found that it was too narrow, and the broadened algorithm/wave/sample-rate
corpus produced the decisive failure above. Reference remains the only
production profile.

A separately compiled `-arch x86_64 -O3 -DNDEBUG` binary also completed all 36
cells under Rosetta with a 26.32% minimum and 28.60% median gain. Treat that as
x86 compile/runtime evidence only; it is not a native-Intel performance claim.

Build and rerun:

```sh
cmake -S . -B build-fast-trig -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BENCHMARK=ON -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF
cmake --build build-fast-trig --target \
  pulp-fast-trig-fm8-benchmark pulp-test-drum-fm pulp-test-fast-math -j8
./build-fast-trig/test/pulp-test-fast-math
./build-fast-trig/test/pulp-test-drum-fm
for run in 1 2 3; do
  ./build-fast-trig/test/pulp-fast-trig-fm8-benchmark > "fm8-${run}.jsonl"
done
```

The historical study retained one scheduler-contaminated process rather than
hiding it. An overlapping shared-host build caused p95 spikes and one 7.12%
cell; that same cell measured 28.46% and 28.11% in adjacent clean processes.
A clean fourth process replaced it and measured a 25.00% minimum. Preserve and
label comparable contamination; never silently discard an unfavorable cell.

## Reference-neutrality probe

Compile `reference_probe.cpp` once against the exact experiment base and once
against the candidate checkout using the same compiler and flags. Alternate the
two binaries for at least seven pairs. They use only the pre-existing FM8 API
and emit an FNV-1a hash over every float sample, so the base build proves
whether the candidate's default-path dispatch changes whole-voice throughput or
output bits.

```sh
c++ -std=c++20 -O3 -DNDEBUG \
  -I /path/to/checkout/core/signal/include \
  -I /path/to/checkout/core/timebase/include \
  -I /path/to/checkout/core/runtime/include \
  test/research/fast_trig_fm8/reference_probe.cpp -o /tmp/fm8-reference-probe
/tmp/fm8-reference-probe
```

Record the exact base/candidate SHAs, compiler, flags, alternating order,
per-pair outputs, and summary with the final research disposition.

The final comparison used exact base
`d87bce52804eb99275d146fe0f0f962eee3b9997`, seven alternating pairs, and
identical weighted checksum `1725.413874887721` and sample hash
`6353b73abdd8d840` in every lane and pair. Patched-reference throughput ranged
from −0.36% to +0.60%, with a +0.09% median. The probe first caught a reassociated
reference multiplication; preserving the original expression restored exact
output before these measurements were recorded.
