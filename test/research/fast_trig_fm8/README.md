# FM8 fast-trig evidence

The production benchmark is Release-only and advisory. It measures the complete
`Fm8DrumVoice`, not the polynomial in isolation: parallel, branching, and
eight-deep serial/feedback voices at 44.1/48/96 kHz and 32/64/128/512-sample
blocks. Each of 36 cells renders 16,384 frames with 15 trials, three
alternating-order passes, full-output checksums, and whole-hit error, peak, RMS,
DC, and coarse spectral-energy metrics.

The accepted Apple M3 Ultra / Apple Clang 21.0.0 run verified CMake Release and
target `-O3 -DNDEBUG`. Three clean processes measured 24.16–25.07% minimum,
33.00–33.30% median, and 38.69–40.83% p95 gains across cell medians. The focused
FM suite covers deterministic replay, block-partition invariance, active-hit
selector rejection, allocation freedom, finite output, and whole-hit
peak/RMS/DC/spectral bounds. Reference remains the default.

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
two binaries for at least seven pairs. They use only the pre-existing FM8 API,
so the base build proves whether the candidate's default-path dispatch changes
whole-voice throughput or output.

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

The accepted comparison used exact base
`d87bce52804eb99275d146fe0f0f962eee3b9997`, seven alternating pairs, and
identical checksum `1725.41387489`. Patched-reference throughput ranged from
−0.75% to +1.24%, with a +0.27% median. The probe first caught a reassociated
reference multiplication; preserving the original expression restored exact
output before these measurements were accepted.
