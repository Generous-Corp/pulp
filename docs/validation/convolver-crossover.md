# Fixed-IR convolver engine crossover

`FixedIrConvolver` runs either the vectorized direct FIR (`FirFilterT`) or the
uniform partitioned FFT (`PartitionedConvolverT`), chosen once at prepare time
by the pure table in `FixedIrConvolver::direct_fir_limit()`. This page is the
evidence for that table and the command that regenerates it.

## Reproduce

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_BENCHMARK=ON
tools/ci/governed-build.sh cmake --build build --target pulp-dsp-throughput-benchmark
./build/test/pulp-dsp-throughput-benchmark --convolver-crossover crossover.csv
```

Each row is `taps,block,instances,arm,selected,mean_ns_frame,p50,p99,max`:
nanoseconds per frame for the whole group of instances, on identical input
and IR. `arm` is `direct` (forced FIR), `fft` (forced partitioned FFT) or
`fixed_ir` (the policy's choice); `selected` names the engine the policy
picks. `mean` is the median of five timed passes; `p50`/`p99`/`max` come from
a separate pass that times each callback.

The matrix: IRs of 8 to 4096 taps (17 lengths), blocks of 1 to 1024 frames
(11 sizes), and 1, 8, 16 and 32 independent instances. Per-frame cost does not
depend on sample rate; deadlines do.

## Receipts

Measured at the commit that combined `FixedIrConvolver` with the real-input
partitioned FFT, built with Apple clang 21.0.0, Release, macOS 27.0.1, AC
power. Each file holds, per row, the median over that host's repeated runs.
A run counted as quiet when the 1-minute load stayed below a quarter of the
core count before and after it; these hosts also run CI work, so most runs
were taken loaded and are kept because the engines in each cell are timed
back to back, which makes the direct/FFT ratio more reliable than either
absolute time.

| File | Chip | Runs | 1-minute load (quiet limit) |
|---|---|---|---|
| [m1-max.csv](convolver-crossover/m1-max.csv) | Apple M1 Max (MacBookPro18,4) | 3, all loaded | 10.9-30.3 (2.5) |
| [m5-max.csv](convolver-crossover/m5-max.csv) | Apple M5 Max (Mac17,7) | 3, all loaded | 25.7-55.2 (4.5) |
| [m5-ultra.csv](convolver-crossover/m5-ultra.csv) | Apple M5 Ultra (Mac17,15) | 6, one quiet | 5.1-11.7 (7.5) |

Each chip gives a crossover per block size: the largest measured IR length
up to which the direct engine is no slower than the FFT, in the median paired
ratio, at every instance count. The table takes the lowest of the three.

Regret is the policy's engine cost over the cheaper engine's in each of the
748 cells (taps x block x instances). Out of sample, fitting on two chips and
scoring the third:

| Held-out chip | Regret geomean | p95 | Worst cell | Cells over 1.1 |
|---|---|---|---|---|
| M1 Max | 1.002 | 1.000 | 1.22 (640 taps, 16 frames, 16 instances) | 7 |
| M5 Max | 1.003 | 1.000 | 1.26 (640 taps, 16 frames, 8 instances) | 11 |
| M5 Ultra | 1.001 | 1.000 | 1.14 (768 taps, 16 frames, 32 instances) | 2 |

An independent loaded M5 Max run, not used for fitting, scores 1.004
geomean, 1.000 p95, worst 1.31 (768 taps, 16 frames, 16 instances), 14 cells
over 1.1. Every worst cell sits just past the 16-frame limit, where the chips
disagree (576, 640 and 768 taps) and the table keeps the lowest; the engines
there are within about 1.3x of each other either way. Against always using
the FFT, the table is 2.38-2.40x faster in geomean and up to 18-22x in the
best cell.

The run-to-run spread of the paired ratio (largest over smallest across a
host's runs) had a median of 1.09-1.28 and a p95 of 1.35-1.62, so a single
loaded run can move a cell by tens of percent; the medians above are what
the table is fitted on.

## What the table encodes

- At 8-frame callbacks and below, the direct engine beat the uniform FFT at
  every measured length, so the limit there is the end of the measured range
  (4096 taps). Longer IRs keep the FFT engine until they are measured.
- From 16 frames up the real-input FFT wins early: 576 taps at 16 frames,
  128 at 32 and 64, and 64 from 128 frames up. The limits held at 1, 8, 16
  and 32 instances.
- Without the Accelerate backend the policy keeps the FFT engine, because the
  scalar direct kernel is an order of magnitude slower; other backends need
  their own measurements before the table applies to them.

## Reopen when

- the FIR or partitioned FFT kernels change materially;
- a new Apple Silicon generation or another SIMD backend is in scope;
- IRs beyond 4096 taps at small callbacks matter, which belongs with the
  non-uniform engine rather than this two-engine table.
