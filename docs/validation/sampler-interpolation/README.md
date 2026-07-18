# Sampler interpolation CPU evidence

**Status:** current verified Release capture for sampler source revision
`35f8741d8885d524730173044c96f24081c04076`. The full verifier and its negative
controls pass against the recorded source bundle and supplied benchmark binary.

This directory holds durable Release measurements for the interpolation
evaluator only. The measurements exclude sampler streaming, page-cache work,
envelopes, voice mixing, and host/plugin overhead; they are not a whole-sampler
CPU claim.

Reproduce the named Apple M5 Max artifact from a Release build:

```bash
cmake -S . -B build-sampler-bench -DCMAKE_BUILD_TYPE=Release \
  -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_TESTS=ON -DPULP_BENCHMARK=ON
cmake --build build-sampler-bench --target pulp-sampler-interpolation-benchmark -j 4
PULP_BENCHMARK_CAPTURED_UTC=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
PULP_BENCHMARK_SOURCE_REVISION=$(git rev-parse HEAD)
PULP_BENCHMARK_SOURCE_SHA256=$(python3 tools/scripts/verify_sampler_interpolation_benchmark.py --print-source-bundle-sha256)
PULP_BENCHMARK_BINARY_SHA256=$(shasum -a 256 build-sampler-bench/test/pulp-sampler-interpolation-benchmark | awk '{print $1}')
./build-sampler-bench/test/pulp-sampler-interpolation-benchmark \
  --machine-label "Apple M5 Max Mac17,7" \
  --machine-model "MacBook Pro Mac17,7, Apple M5 Max" \
  --os "macOS 26.5.2 build 25F84" --architecture arm64 \
  --compiler "Apple clang 21.0.0 (clang-2100.1.1.101)" \
  --source-base-revision "$PULP_BENCHMARK_SOURCE_REVISION" \
  --source-bundle-sha256 "$PULP_BENCHMARK_SOURCE_SHA256" \
  --benchmark-binary-sha256 "$PULP_BENCHMARK_BINARY_SHA256" \
  --generated-utc "$PULP_BENCHMARK_CAPTURED_UTC" \
  > docs/validation/sampler-interpolation/apple-m5-max-mac17-7.release.json
```

The source bundle hash covers the interpolation policy, sinc kernel, benchmark
render support, and benchmark driver. The binary hash binds the artifact to the
exact executable used for capture. The executable exits nonzero if a measured
P95 exceeds the ratcheted tier budgets. Each of three measurement epochs uses
31 batches that retain the minimum of five 8,192-frame repetitions; the report
keeps the epoch with the lowest P95. This is intentionally a quiescent best-case
evaluator-cost measurement: it filters transient and row-scale scheduler
contention without hiding a DSP regression, which affects every repetition and
epoch. It does not claim loaded-host tail latency. Capture on an otherwise idle
machine; a failure observed only under sustained host contention is an invalid
capture environment until reproduced quiescent. The per-tier budgets still
catch ordinary multi-fold DSP regressions. Verify the complete 108-row schema,
matrix, current source bundle, environment, and acceptance interpretation with:

```bash
python3 tools/scripts/verify_sampler_interpolation_benchmark.py \
  --benchmark-binary build-sampler-bench/test/pulp-sampler-interpolation-benchmark
python3 tools/scripts/verify_sampler_interpolation_benchmark.py --self-test \
  --benchmark-binary build-sampler-bench/test/pulp-sampler-interpolation-benchmark
```

The full verifier recomputes both the complete repo-owned compiler-input bundle
and the supplied executable SHA-256. A source-only verifier mode exists for
ordinary non-benchmark builds, but it is explicitly labeled and does not prove
the binary claim.
