# G-12 baseline evidence (2026-11-05)

This is an evidence-only baseline from `origin/main` at source commit
`cbb55cdd011fa48848e0a4df403faf38907e1055` in the classified worktree
`g12-baseline-evidence-20261105`. No tracked source files were changed.

## Host and build

- Host: Mac15,14, arm64; macOS 27.2 (26B123); 28 logical CPUs; 96 GiB RAM.
- Compiler: Apple clang 21.0.0.21000401 (`arm64-apple-darwin27.2.0`).
- CMake cache: `CMAKE_BUILD_TYPE=Release`, `PULP_ENABLE_GPU=OFF`,
  `PULP_BUILD_EXAMPLES=OFF`, `PULP_GRAPH_DIRECT_COPY_BENCHMARK=ON`.
- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  -DPULP_GRAPH_DIRECT_COPY_BENCHMARK=ON -DPULP_ENABLE_GPU=OFF
  -DPULP_BUILD_EXAMPLES=OFF`.
- Target builds used governed `pulp build --target ...`.

Executable and source hashes:

| item | SHA-256 |
| --- | --- |
| `build/test/pulp-graph-r2-measurement` | `0426a43d7eb8317ebc3e27c9dfb94177fb701a7f9c577dd3392f8bb01203b133` |
| `test/test_graph_r2_measurement.cpp` | `23d7238ac46485b1d8d19d0379699084879ea92f213c5580243cb99d4704039e` |
| `test/harness/rt_allocation_probe.cpp` | `112034d91dfaa2ab84681c173b373e4543e7f4e690809802ce1e213542f6e17f` |
| `build/test/pulp-test-signal-graph-x9-modular-instrument` | `3bd469ee91aa3864e2aafbc3e73552276d90a368ea6855360591a6d5c744d09e` |
| `test/test_signal_graph_x9_modular_instrument.cpp` | `5cf2bf8ddf1f254ee9cb1bc1afbb4ae3745f0f8f6b7a905c0417dfbfbecf3963` |

## R2 native benchmark

`pulp-graph-r2-measurement` ran to completion in 110.35 s (user 99.52 s,
system 0.93 s). It emitted 40 activation rows and 40 timing rows covering
`passthrough`, `chain`, and `fan-out`; scales `[1, 4, 16, 64]`; and block sizes
`[16, 32, 64, 128, 512]` with the fixture's passthrough/fan-out exclusions.
All 40 timing rows reported `oracle=pass` and `process_allocations=0`.
Warmup was 500 blocks, timing was 100,000 blocks x 31 samples per row.

Raw stdout is in `r2-native-output.txt` (SHA-256
`6c37c77f8a8bdc34611f40ad2af0511b26647c686b1cc1c6a60fbe49fd93b375`); timing
and stderr are in `r2-time-and-stderr.txt` (SHA-256
`ca98b09ea39b12b8c2e4688ee8e6152db815f9f3b3c4bbc249695d5262ec549f`).

The newer `tools/graph_performance_qualification.py` receipt wrapper is not
present at this source head. Therefore wrapper-only fields are **N/A**:
canonical receipt digest, full source/executable binding in the receipt,
per-row timed allocation samples, complete-matrix validator, and planted
positive/negative controls. Native one-scope allocation checks are reported
above and do not substitute for those fields.

## X9 supporting probe

The Release X9 executable ran directly: 5 test cases, 1,470 assertions, all
passed. Generated CTest discovery then ran the same five cases (including the
X3 reusable-component case) with 5/5 passed. A CTest name query for the
executable itself still returns `No tests were found!!!`; the generated case
names are the runtime authority. Raw receipts:

- `x9-direct-output.txt` (SHA-256 `0a82c304dce29d7393e58986553b60a934cbc86326327874b08bd3aa4c523e54`)
- `x9-generated-ctest.txt` (SHA-256 `7c9590792027cffc6ea3222ac638980a6509756032f75112b45e699e2484fb68`)
- `x9-hosted-receipt-ctest.txt` (SHA-256 `136422d4ca4634f008b4657f86f4ee6e61142ed7b1da54432916f15c3c62dabd`)

The hosted-format availability receipt passed, with CLAP, VST3, and AU all
`available: false`; LV2 is `not-applicable`. No hosted lifecycle claim is made.

## RT supporting probes

The grouped Release RT-safety executable ran 21 test cases and 59 assertions,
all passed. The CTest probe `Prepared realtime pitch/time processor is
allocation-free while processing` passed 1/1. Raw receipts are
`rt-direct-output.txt`, `rt-list-tests.txt`, `rt-ctest-output.txt`, and their
hashes can be regenerated with `shasum -a 256 docs/reports/g12-baseline-20261105/*`.

RTSan is **N/A on this host/configuration**: Apple clang 21.0.0 rejects
`-fsanitize=realtime` for `arm64-apple-darwin27.2.0` (`rtsan-compiler-probe.txt`).
No Linux/LLVM-20 hosted RTSan run was attempted, so no cross-platform RTSan
claim is made.

## G-12 dimension status

| dimension | status | evidence boundary |
| --- | --- | --- |
| R2 graph runtime matrix/oracle | **PARTIAL** | 40 native rows, oracle pass, no allocations; wrapper receipt and negative controls absent at this head |
| R2 timing | **PARTIAL** | Native p50/p99/max/jitter rows; no calibrated cross-host baseline or receipt digest |
| RT allocation probe | **PARTIAL** | One probe scope per row, 40/40 zero; no timed allocation samples |
| X9 live graph fixture | **PASS (local)** | 5/5 CTest cases, 1,470 assertions |
| X9 hosted CLAP/VST3/AU | **N/A** | Artifacts absent; availability receipt records false for all three |
| Local RT-safety checks | **PASS (local)** | 21 cases, 59 assertions; pitch/time CTest case 1/1 |
| Hosted RTSan | **N/A** | Unsupported by Apple clang arm64; no Linux run |
| Browser/native parity, fleet, CI, cross-platform | **N/A** | Not exercised by this lane |
