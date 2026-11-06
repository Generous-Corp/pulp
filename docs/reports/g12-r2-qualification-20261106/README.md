# G-12/R2 bounded qualification slice (2026-11-06)

This artifact records the first runnable R2 wrapper slice from current `origin/main`. It is **PARTIAL** evidence only; it does not qualify realtime callback deadlines, WCET, cross-platform behavior, hosted plugins, fleet execution, or the G-12 flagship/final-review gates.

## Exact run

- Source commit: `142d724d659c1fbd1c7d53c80142c7d64bd68276`
- Executable: `build/test/pulp-graph-r2-measurement`
- Executable SHA-256: `2c7638f810c362fa76624d702d3affaff575864724a2f22e1d972fd0612b7f86`
- Platform: `Darwin 27.2.0 arm64`
- Compiler: `unknown unknown` (`/usr/bin/c++`)
- Build profile: Release, GPU `OFF`, examples `OFF`, direct-copy benchmark `ON`
- Command: `python3 tools/graph_performance_qualification.py record --source-root . --build-dir build --executable build/test/pulp-graph-r2-measurement --output-dir docs/reports/g12-r2-qualification-20261106`
- Receipt SHA-256: `7380b0bb2c102f438a5b711faa2174ce7ffda0c5098dbce03765ea9d5dac5176`

## Measured scope

The existing native executable emitted the complete 40-cell matrix: passthrough scale 1, chain scales 1/4/16/64, and fan-out scales 4/16/64, each at blocks 16/32/64/128/512. Every cell reported `oracle=pass` and `process_allocations=0`; timing rows retained the executable's 31 samples over 100,000 timed blocks after 500 warmup blocks. The wrapper binds each cell's activation/timing fields and the deterministic first-sample output oracle.

The output oracle allows the fixture's bounded fan-in tolerance (1e-4), while the receipt preserves the exact reported output bits.

## Controls

- Positive receipt validation passed (`validator.txt`).
- A resigned mutation of one expected output cell failed validation.
- A separately resigned mutation of one reported allocation count failed validation.
- Restoring the original receipt passed again (`controls.txt`).

The negatives are semantic controls: each mutation recomputes `receipt_sha256`, so a stale digest cannot explain the failure.

## Artifacts

| artifact | SHA-256 |
| --- | --- |
| `raw-output.txt` | `26ee9e712b6ff4d6550bba2e18310d575015654eff61f3e4397d9c0bf0f73828` |
| `raw-stderr.txt` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `receipt.json` | `7380b0bb2c102f438a5b711faa2174ce7ffda0c5098dbce03765ea9d5dac5176` |

This receipt is a bounded measurement harness result, not a full G-12 qualification claim.
