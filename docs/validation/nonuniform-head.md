# Non-uniform convolver head length

`NonUniformPartitionedConvolver` runs the first K·B taps of an IR in a
per-block head stage and the rest in a tail stage, which processes K blocks
at a time. When `load_ir()` is given no tail multiplier, K comes from the
pure table in `NonUniformPartitionedConvolver::choose_tail_multiplier()`.
This page is the evidence for that table and the command that regenerates
it.

## Where the table applies

The table applies only to the configuration it was measured on: single
precision, the Accelerate backend on Apple Silicon macOS, and the chip
families Apple M1 and Apple M5. `default_tail_multiplier()` reads
`machdep.cpu.brand_string` once and checks it with `table_measured_on()`.

Everywhere else, K stays 4 (`kDefaultTailMultiplier`) until it is measured:
- other chip families, such as M2, M3 and M4, which can rank the K arms
  differently for the same transforms;
- other platforms and FFT backends;
- double precision.

To extend the table to a new family, add that chip's CSV and re-fit.

## Reproduce

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_BENCHMARK=ON
tools/ci/governed-build.sh cmake --build build --target pulp-dsp-throughput-benchmark
./build/test/pulp-dsp-throughput-benchmark --nonuniform-head head.csv
```

Each row is
`ir,B,K,head,round,mean_ns_frame,p99_ns_call,max_ns_call,max_abs_vs_K1,ref_rms`.
The columns are:

- `mean_ns_frame`: the mean cost per frame over at least one second of audio
  (enough for four firings of the largest tail).
- `p99_ns_call` and `max_ns_call`: the 99th percentile and maximum of the
  per-callback times.
- `max_abs_vs_K1`: that K's largest output difference from K = 1 (the uniform
  engine) on identical input, beside the output RMS.

The matrix is IRs of 4096, 16384, 48000 and 96000 taps, blocks of 16 to 512
frames, and every power-of-two K whose head fits the IR. Every K is timed in
each of five rounds, in alternating order.

## Receipts

Built with Apple clang 21.0.0, Release (`-O3 -DNDEBUG`), macOS 27.0.1. Both
hosts also run CI work. The K arms in a cell are timed back to back, so the
cost ratio between them is the reliable figure. The absolute times are
indicative.

| File | Chip | Runs | Conditions |
|---|---|---|---|
| [m1-max.csv](nonuniform-head/m1-max.csv) | Apple M1 Max (MacBookPro18,4) | 3 | nice 19, background QoS, 1-minute load 4-5 |
| [m5-max.csv](nonuniform-head/m5-max.csv) | Apple M5 Max (Mac17,7) | 5 | nice 19, 1-minute load 7-9 |

Both files were measured on the compact-arena partitioned kernel, which the
head and tail stages are built from. The same benchmark binary ran on both
hosts.

### The fitting rule

Per chip and cell, a K's cost is the median over every round of its runs.
Its regret is that cost divided by the cheapest K's cost. The table takes the
K whose worst regret over the two chips is lowest; on a tie it takes the
smaller K.

An IR length between measured lengths uses the nearest one on a log scale,
so the column boundaries are the geometric means 8192, 28044 and 67882 taps.

The `nonuniform-head-table` test recomputes the table from these files with
`tools/scripts/nonuniform_head_check.py`. It fails if the header and the data
disagree.

### How the table performs

Regret over the 24 cells (in sample):

| Chip | Table: geomean, worst | Fixed K = 4: geomean, worst |
|---|---|---|
| M1 Max | 1.003, 1.04 | 2.06, 9.75 (96000 taps, 16 frames) |
| M5 Max | 1.005, 1.05 | 1.98, 8.56 (96000 taps, 16 frames) |

Out of sample, fitting on one chip and scoring the other:

| Held-out chip | Geomean | Worst |
|---|---|---|
| M1 Max | 1.010 | 1.07 (48000 taps, 64 frames) |
| M5 Max | 1.022 | 1.19 (48000 taps, 256 frames) |

Against the previous fixed K = 4 the table is 2.05x (M1 Max) and 1.97x (M5
Max) faster in geomean, and up to 9.7x and 8.6x in the best cell.

### Per-callback cost

A larger K makes the tail partitions bigger, which bunches FFT work into the
blocks where the tail fires, so the mean alone can hide callback spikes. The
tables below give each callback's p99 and maximum as a share of the 48 kHz
deadline (B / 48000 s). A tail that fires once every K blocks (K at most 64)
is at least 1 in 64 callbacks, so the p99 includes the firing blocks.

M5 Max, the worst cases. Each figure is the median over rounds, as table
vs K = 4; the last column is the worst single round:

| IR | B | Table K | p99 | Maximum | Worst-round maximum |
|---|---|---|---|---|---|
| 48000 | 16 | 64 | 4.1% vs 3.9% | 6.7% vs 7.7% | 9.3% vs 21.7% |
| 96000 | 16 | 64 | 7.3% vs 8.6% | 11.0% vs 13.6% | 19.6% vs 31.6% |
| 48000 | 32 | 32 | 2.0% vs 2.0% | 3.0% vs 2.5% | 4.8% vs 6.7% |
| 96000 | 32 | 64 | 4.4% vs 4.1% | 5.6% vs 5.0% | 8.2% vs 28.1% |

At the worst cases, the table's worst single round is below K = 4's in every
row. Its median p99 and median maximum are within a few tenths of a percent
of K = 4's, slightly higher in some rows, and the highest of them is 11.0% of
the deadline.

Over all 24 cells on M5 Max:
- In ordinary cells the table's p99 is higher. In the median cell it is 1.22x
  K = 4's p99, and at most 1.66x.
- Every cell where the table's p99 is higher stays at or below 4.4% of the
  deadline.
- Every cell where its maximum is higher (at most 1.54x) stays at or below
  5.6% of the deadline.
- The largest table maximum in any cell is 11.0% of the deadline, and 19.6%
  in the worst single round.

M1 Max ran at background QoS, so its maxima reflect preemption rather than
computation: K = 4's own median maximum is 385-1686% of the deadline at the
worst cases. Its p99 at those cells is far lower with the table, for example
36.6% vs 241.5% at 96000 taps and 16 frames.

### Output

The largest difference from the uniform engine over every cell and K is
4.3e-6 of the output RMS (4.31e-6 on both chips). That is float rounding,
since K only changes where the IR splits between the stages.
