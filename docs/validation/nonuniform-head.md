# Non-uniform convolver head length

`NonUniformPartitionedConvolver` runs the first K·B taps of an IR in a
per-block head stage and the rest in a tail stage, which processes K blocks
at a time. When `load_ir()` is given no tail multiplier, K comes from the
pure table in `NonUniformPartitionedConvolver::choose_tail_multiplier()`.
This page is the evidence for that table and the command that regenerates
it.

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
| [m1-max.csv](nonuniform-head/m1-max.csv) | Apple M1 Max (MacBookPro18,4) | 3 | nice 19, background QoS, 1-minute load 9-34 |
| [m5-max.csv](nonuniform-head/m5-max.csv) | Apple M5 Max (Mac17,7) | 5 | nice 19, 1-minute load 4-8 |

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
| M1 Max | 1.007, 1.08 | 2.37, 11.99 (96000 taps, 32 frames) |
| M5 Max | 1.007, 1.09 | 2.03, 9.46 (96000 taps, 16 frames) |

Out of sample, fitting on one chip and scoring the other:

| Held-out chip | Geomean | Worst |
|---|---|---|
| M1 Max | 1.039 | 1.39 (96000 taps, 64 frames) |
| M5 Max | 1.024 | 1.15 (4096 taps, 128 frames) |

Against the previous fixed K = 4 the table is 2.35x (M1 Max) and 2.02x (M5
Max) faster in geomean, and up to 12.0x and 9.5x in the best cell.

### Per-callback cost

A larger K fires a larger tail transform less often, so the table trades a
lower mean for larger occasional callbacks. The ratios below are the table's
per-callback figure over the same figure at K = 4.

| Chip | Per-callback p99, median ratio (max) | Per-callback maximum, median ratio (max) |
|---|---|---|
| M5 Max | 1.27 (1.58) | 1.01 (1.47) |
| M1 Max | 0.64 (1.49) | 0.59 (1.05) |

On M5 Max the worst cell is 96000 taps at 16 frames. There the table's p99 is
17.2% of a 96 kHz callback deadline (K = 4: 20.8%), and its maximum is 23.2%
(K = 4: 39.8%).

On the loaded, background-QoS M1 Max, the same cell's p99 is 98.6% of that
deadline with the table and 963% with K = 4.

### Output

The largest difference from the uniform engine over every cell and K is
4.3e-6 of the output RMS. That is float rounding, since K only changes where
the IR splits between the stages.
