# Paired fast-trig SSB research snapshot

This non-production branch preserves the exact Phase 5 experiment so a future
compiler, CPU, or library candidate can be compared without reconstructing the
harness from prose. It does not authorize the public aliases or degree-13 pair
on Pulp main.

- Base: `410944913a9416bff2aa3d408f129ef292a038cf`
- Host: Apple M3 Ultra (28 cores, 96 GB), macOS 26.6.2 (25G83)
- Compiler: Apple Clang 21.0.0 (`clang-2100.1.1.101`), arm64
- Build: CMake Release; cache and all three target flags verified as
  `-O3 -DNDEBUG`
- Upstream expression: `publik-void/sin-cos-approximations` pinned at
  `d65178e684c7626b0fe7df6f261dbadc54403bce`, under the public permission
  already cited in `NOTICE.md`

Build and run:

```sh
cmake -S . -B build-fast-trig-pair -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BENCHMARK=ON -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF
cmake --build build-fast-trig-pair --target \
  pulp-fast-trig-pair-benchmark pulp-test-fast-math \
  pulp-test-signal-frequency-shifter-ssb -j8
./build-fast-trig-pair/test/pulp-test-fast-math
./build-fast-trig-pair/test/pulp-test-signal-frequency-shifter-ssb
for run in 1 2 3; do
  ./build-fast-trig-pair/test/pulp-fast-trig-pair-benchmark \
    > "pair-${run}.jsonl"
done
```

The benchmark alternates reference/candidate order, constructs both inputs
before timing, recreates component state for every timed render, and reads every
output channel into weighted checksums after timing. `median` is the usual
middle value. The historical across-cell p95 uses the empirical quantile at
`floor(0.95 * (N - 1))` in the sorted zero-based array; the benchmark's per-lane
p95 uses index `floor(0.95 * N)` and is the maximum of its 15 trials.

The exact three final JSONL processes and both selector-probe output sets are in
`evidence.md`. `reference_probe.cpp` rebuilds the default-path comparison; build
it once against this branch and once against the base above, then alternate the
two binaries. The first probe set demonstrates why the per-sample runtime
selector was rejected; the second demonstrates reference neutrality after the
compile-time-profile redesign.

Reopen production only after the canonical plan's two-primary-target and audio
gates pass. Platform paired/vector math remains an unmeasured baseline here.
Chebyshev remains closed absent a consumer win under its documented reopen
condition; an integer-assembly candidate first needs a portable prototype;
RLIBM or platform `sinpi` needs an applicable double-pair design.
