# DSP Throughput Benchmark

`pulp-dsp-throughput-benchmark` measures what the heavy `pulp::signal`
processors cost per audio frame, so a DSP change can carry a before/after
number instead of an impression. It is advisory evidence: nothing in it (or in
the CI lane that runs it) compares a number against a threshold, because timing
on a shared machine is noisy and a flaky perf gate is worse than none.

## What it measures

- **Processors** at 48 kHz for block sizes 32, 128 and 512: partitioned,
  non-uniform and zero-latency convolution, the FDN reverb, x4 oversampling
  (`fir_biquad`, `polyphase_iir`, `linear_phase_fir`) around a `tanh`, FIR
  filters, character delays, pitch/time, an 8-voice synth loop, compressor,
  meter, biquad/SVF/ladder filters, oscillators, gain, FFT and resampling.
  Each row reports:
  - **mean** ns/frame: median over repetitions of the whole-run time;
  - **p99** and **max** ns/frame of individually timed blocks. Realtime
    headroom is set by the worst block, not the average: a partitioned
    convolver's mean hides blocks that run an FFT and cost many times more.
- **Kernels** in ns/element at N = 64 and 512: the scalar loop shapes
  (`sum`, `sum_squares`, `max_abs`, `dot`, `ramp_mul`, per-output
  `correlate`) and every compiled `pulp::simd` backend (`scalar-backend`,
  `highway`, `accelerate` on Apple) for the same kernels plus `decimate2`.

Every case feeds a rolling window of noise and publishes its output through a
compiler memory barrier, so no loop can be hoisted out of the timing. A kernel
row under 0.005 ns/element is flagged as suspect in the JSON: that is a hoisted
loop, not a fast one.

## Running it

Build Release with the benchmark option on. The GPU stack is not needed:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BENCHMARK=ON -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF
tools/ci/governed-build.sh cmake --build build-bench \
  --target pulp-dsp-throughput-benchmark
./build-bench/test/pulp-dsp-throughput-benchmark \
  --commit "$(git rev-parse HEAD)" --json dsp-throughput.json
```

Options: `--filter SUBSTRING` runs matching rows only (`--filter conv.`,
`--filter kernel.dot`), `--seconds S` sets the audio per repetition (default
2), `--host NAME` labels the run, and `--smoke` exercises every case in about a
second (its numbers mean nothing). A run takes about 30 seconds.

Confirm the binary is optimized before trusting a number: the JSON records
`"optimized"` and `"ndebug"`, and both must be `true`. It also records the
host, CPU, OS, compiler, commit, the active `pulp::simd` backend and the
measured cost of one timer read.

## Comparing two runs

The JSON uses the `pulp-bench-sections/1` schema, which
`tools/scripts/bench_diff.py` renders as before/after tables with a
regressions list:

```bash
python3 tools/scripts/bench_diff.py --format markdown before.json after.json
```

Compare runs from the same host only, and prefer ratios within one run to
absolute numbers across runs: on a loaded machine single rows move by 10-15%.
A "no change" reading means something only when a row that must move (for
example the kernel rows of the code you changed) did move.

Per-host baselines for the SIMD work are kept with the project's planning
records, one file per host and date, each recorded with its commit and Release
proof.

## In CI

`.github/workflows/dsp-throughput-bench.yml` builds and runs the benchmark
weekly and on demand (`workflow_dispatch`) on an arm64 macOS runner and an
x86-64 Linux runner, uploads the JSON, the text log and a markdown table as
artifacts, and writes the table to the job summary. It never fails on a slow
result and is not a required check.

## Adding a row

Add a `bench(...)` call (processors) or `bench_kernel(...)` call (kernels) in
`test/test_dsp_throughput_benchmark.cpp` next to its neighbours, keep the
input rolling through `InputFeed` or `clobber()`, and use a stable,
dot-separated name (`family.variant`) so `bench_diff.py` lines it up with older
runs. Rename a row only when its meaning changes.
