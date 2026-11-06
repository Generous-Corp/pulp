# G-12/R2 bounded qualification slice (2026-11-06)

This artifact records the first runnable R2 wrapper slice from the repaired PR head. It is **PARTIAL** evidence only; it does not qualify realtime callback deadlines, WCET, cross-platform behavior, hosted plugins, fleet execution, or the G-12 flagship/final-review gates.

## Exact run

- Source commit: `7a63d4d99dc04cc651a3c47bdc974263b93c13e6`
- Fixture source: `test/test_graph_r2_measurement.cpp` (SHA-256 `5958bf2a806695ac8e0493662232cdec19be67acab90e62ac8081b6050ab454c`; copied as `source-fixture.cpp`)
- Executable: `build/test/pulp-graph-r2-measurement`
- Executable SHA-256: `6ba732c383cd83ac693ffd371350a39950ec5fdd1a45293ce8a671e038e7ecf7`
- Platform: `Darwin 27.2.0 arm64`
- Compiler: `unknown unknown` (`/usr/bin/c++`)
- Build profile: Release, GPU `OFF`, examples `OFF`, direct-copy benchmark `ON`
- Command: `python3 tools/graph_performance_qualification.py record --source-root . --build-dir build --executable build/test/pulp-graph-r2-measurement --output-dir docs/reports/g12-r2-qualification-20261106`
- Receipt SHA-256: `2a5b5e9f6ad7a3f1c866d07dbd72b3889d55448e772ddc617d44605bebf3fcb3`

## Measured scope

The native executable emitted the complete 40-cell matrix: passthrough scale 1, chain scales 1/4/16/64, and fan-out scales 4/16/64, each at blocks 16/32/64/128/512. Every cell reported `oracle=pass`, `process_allocations=0`, and timed allocation evidence of `timed_allocations=0` across 31 samples over 100,000 timed blocks after 500 warmup blocks. The validator parses and binds the checked raw stdout, requires all activation/timing fields and run constants, and applies a deterministic float32 output oracle with a bounded fan-in ULP contract.

This remains PARTIAL evidence: it does not claim callback deadlines, WCET, cross-platform RT safety, hosted plugins, fleet execution, or final G-12 flagship coverage.

## Controls

- Positive receipt validation passed (`validator.txt`).
- Resigned mutations of expected output, untimed allocation, timed allocation, timing order, and raw-output binding are planted negatives in the focused controls.
- Restoring the original receipt passed again (`controls.txt`).

Each mutation recomputes `receipt_sha256`, so a stale digest cannot explain a rejection.

## Artifacts

| artifact | SHA-256 |
| --- | --- |
| `raw-output.txt` | `ef3f1bebc33442ebdaaa66e3f9e479828a21d26e4f5941d6ebdd55f03afb0d66` |
| `raw-stderr.txt` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `source-fixture.cpp` | `5958bf2a806695ac8e0493662232cdec19be67acab90e62ac8081b6050ab454c` |
| `receipt.json` | `2a5b5e9f6ad7a3f1c866d07dbd72b3889d55448e772ddc617d44605bebf3fcb3` |

This receipt is a bounded measurement harness result, not a full G-12 qualification claim.
