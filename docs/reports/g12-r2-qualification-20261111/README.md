# G-12/R2 bounded qualification slice (2026-11-11)

This is a fresh exact-head R2 receipt from `origin/main` at
`3633377fdd47c086850a22ef8d87130cd17f4e6d`. It refreshes the historical
2026-11-06 receipt without changing the measurement contract. The result is
**PARTIAL** evidence only: it does not qualify realtime callback deadlines,
WCET, cross-platform behavior, hosted plugins, fleet execution, or the G-12
flagship/final-review gates.

## Exact run

- Source commit: `3633377fdd47c086850a22ef8d87130cd17f4e6d`
- Fixture source: `test/test_graph_r2_measurement.cpp`
- Executable: `build/test/pulp-graph-r2-measurement`
- Executable SHA-256: `aea4cf9fcc0d5d4568ddba16f08fae01d64337dd13c93460f685844df6d87043`
- Build profile: Release, GPU `OFF`, examples `OFF`, direct-copy benchmark `ON`
- Receipt SHA-256: `c89071d188b2069fd74927cc21771d21fe02ca9604947e1f242273437e0b82fb`
- Command: `python3 tools/graph_performance_qualification.py record --source-root . --build-dir build --executable build/test/pulp-graph-r2-measurement --output-dir docs/reports/g12-r2-qualification-20261111`

## Measured scope and controls

The native executable emitted the complete 40-cell topology/scale/block matrix.
The receipt validator passed, and the focused Python suite passed 14 tests.
The independent controls passed with the positive receipt plus 12 planted
negative mutations rejected. Timing remains unpaced batch measurement; no
callback qualification is claimed.

## Artifacts

| artifact | SHA-256 |
| --- | --- |
| `raw-output.txt` | `ce43a2fa6a5c61709c61ee036792e7f522197c90949ec9960164c45b7a100370` |
| `raw-stderr.txt` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `source-fixture.cpp` | `f10161d7080616f206c179a40e982a75aa748de8b1ef088252863c637b307990` |
| `benchmark-executable` | `aea4cf9fcc0d5d4568ddba16f08fae01d64337dd13c93460f685844df6d87043` |
| `receipt.json` | `c89071d188b2069fd74927cc21771d21fe02ca9604947e1f242273437e0b82fb` |

This receipt is bounded measurement evidence, not full G-12 qualification.
