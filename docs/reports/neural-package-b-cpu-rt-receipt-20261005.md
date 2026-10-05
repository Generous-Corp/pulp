# Package B CPU neural benchmark and RT receipt

Date: 2026-10-05  
Protected base: `origin/main` at `ff83c0e9fd147dc143af7228eac0e3298e84b35f`  
Worktree: `/Users/danielraffel/Code/pulp-neural-package-b-20261005`  
Scope: benchmark/test receipt only; no manifest or runtime source changes.

> **Historical audit snapshot.** This receipt records the package-B benchmark
> at the protected base above; it is not a current-head status receipt. For
> current coordination and acceptance state, see the [neural audio program
> status](../status/neural-audio-program-status-20261004.md) and the [neural
> real-time competitive proof plan](neural-competitive-proof-plan-20261003.md).

## Reproduction

The governed build was run with:

```text
pulp build --target pulp-test-streaming-model-benchmark
```

The focused test was discovered and run by CTest:

```text
ctest --test-dir build -N -R 'streaming CPU matrix is callback-safe and allocation-free'
ctest --test-dir build --output-on-failure -R 'streaming CPU matrix is callback-safe and allocation-free'
```

CTest discovered test 490 and passed 1/1. The direct Catch2 case also passed 102 assertions in 1 case. The opt-in detailed receipt was generated with:

```text
PULP_STREAMING_BENCHMARK_BLOCKS=64 \
PULP_STREAMING_BENCHMARK_REPEATS=3 \
PULP_STREAMING_BENCHMARK_JSON=$PWD/build/neural-benchmark-receipt.json \
build/test/pulp-test-streaming-model-benchmark \
  'streaming CPU matrix is callback-safe and allocation-free'
```

The generated JSON is `pulp.neural.streaming-benchmark.v1`, has 9 cells (3 host metadata rows × 3 model shapes), 64 blocks/repeat, and 3 repeats. The executable SHA-256 was `a96fabd91bb5fc09ae7671e160d241274ac898a948af0b7ae702f06f79d1675c`; the detailed JSON SHA-256 was `05fad43a9d1b09df3aa9c00920b0f111e4c1c13af4555b83756edb0f2c74ccec`.

## Measured matrix

| host sample-rate metadata | frames | model shape | allocations | deadline misses | max abs error | model checksum | steady p50 model/oracle (ns) |
| ---: | ---: | :--- | ---: | ---: | ---: | ---: | ---: |
| 44100 | 32 | 1×3 | 0 | 0 | 0 | 621.750589 | 83 / 42 |
| 44100 | 32 | 2×5 | 0 | 0 | 0 | 1683.329697 | 83 / 83 |
| 44100 | 32 | 4×7 | 0 | 0 | 0 | 4243.886098 | 125 / 125 |
| 48000 | 64 | 1×3 | 0 | 0 | 0 | 1251.945294 | 125 / 84 |
| 48000 | 64 | 2×5 | 0 | 0 | 0 | 3343.863660 | 125 / 125 |
| 48000 | 64 | 4×7 | 0 | 0 | 0 | 8485.873511 | 250 / 250 |
| 96000 | 128 | 1×3 | 0 | 0 | 0 | 2537.181765 | 250 / 208 |
| 96000 | 128 | 2×5 | 0 | 0 | 0 | 6639.528348 | 291 / 291 |
| 96000 | 128 | 4×7 | 0 | 0 | 0 | 16911.378128 | 500 / 500 |

The CPU model matches the independent CPU oracle in every cell (`max_abs_error=0` and equal checksums). `host_sample_rate_metadata` is configuration metadata only: this test does not claim execution against a physical host clock or device sample-rate conversion.

## RT controls and limits

The callback allocation counter stayed at zero in all cells. On this UNIX host, the test target enables the pthread interposition trap (`lock_probe=pthread_interposition_trap`). The receipt still emits `locks=null`, `blocking_calls=null`, and `counter_status=unavailable_no_explicit_event_counters`: no portable explicit lock/blocking event counters are available, and the trap is not a proof of absence across non-UNIX hosts. The benchmark verdict remains `feasibility_only`; paced delivery, independent cold starts, named serialized model provenance, complete host/toolchain provenance, and planted campaign controls remain open gates.
