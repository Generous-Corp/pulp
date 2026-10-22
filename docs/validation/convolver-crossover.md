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

The matrix: IRs of 8 to 4096 taps (17 lengths), blocks of 16 to 1024 frames
(7 sizes), and 1, 8, 16 and 32 independent instances. Per-frame cost does not
depend on sample rate; deadlines do.

## Receipts

Measured with Apple clang 21.0.0, Release, macOS 27.0.1, AC power, on hosts
that were also running other work (1-minute load noted). Engines are paired
within each run, so ratios are more reliable than absolute times.

| File | Chip | Load during run |
|---|---|---|
| [m1-max.csv](convolver-crossover/m1-max.csv) | Apple M1 Max (MacBookPro18,4) | 27-46 |
| [m5-max.csv](convolver-crossover/m5-max.csv) | Apple M5 Max (Mac17,7) | 8-24 |
| [m5-ultra.csv](convolver-crossover/m5-ultra.csv) | Apple M5 Ultra (Mac17,15) | 11-22 |

The table was fitted on separate M5 Max and M5 Ultra runs; these files are
an independent rerun, and the M1 Max contributed no fitting data.

| Chip | Selected engine / cheaper engine (mean, p95, max) | Speedup over always-FFT (geomean, max) |
|---|---|---|
| M1 Max | 1.002, 1.000, 1.17 | 2.17x, 33.0x |
| M5 Max | 1.002, 1.000, 1.12 | 2.16x, 43.0x |
| M5 Ultra | 1.006, 1.000, 1.49 | 2.28x, 66.9x |

The worst cells are within the run-to-run spread of the two separately timed
arms on loaded hosts (about +-5% on M1/M5 Max, +-30% on M5 Ultra).

## What the table encodes

- At 16-frame callbacks and below, the direct engine beat the uniform FFT at
  every measured length, so the limit there is the end of the measured range
  (4096 taps). Longer IRs keep the FFT engine until they are measured.
- Above 16 frames the crossover is not monotonic in block size: about 448
  taps at 32 frames, 256 at 64, 512 at 128, 384 at 256 and 256 at 512 and
  above. It held at 1, 8, 16 and 32 instances.
- Without the Accelerate backend the policy keeps the FFT engine, because the
  scalar direct kernel is an order of magnitude slower; other backends need
  their own measurements before the table applies to them.

## Reopen when

- the FIR or partitioned FFT kernels change materially;
- a new Apple Silicon generation or another SIMD backend is in scope;
- IRs beyond 4096 taps at small callbacks matter, which belongs with the
  non-uniform engine rather than this two-engine table.
