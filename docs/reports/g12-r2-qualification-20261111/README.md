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
| `benchmark-executable` (not committed, see below) | `aea4cf9fcc0d5d4568ddba16f08fae01d64337dd13c93460f685844df6d87043` |
| `receipt.json` | `c89071d188b2069fd74927cc21771d21fe02ca9604947e1f242273437e0b82fb` |

## Benchmark executable

The 3.7 MB arm64 Mach-O the recorder copied next to the receipt is not kept in
the repository; its SHA-256 above and in `receipt.json` is the record of it.
`receipt.json` still names `benchmark-executable` as an artifact, so
`graph_performance_qualification.py validate` on this directory reports it
missing until the binary is restored. To regenerate it, check out the source
commit, configure the same build profile, build the fixture, and copy it in:

```sh
git checkout 3633377fdd47c086850a22ef8d87130cd17f4e6d
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF \
  -DPULP_BUILD_EXAMPLES=OFF -DPULP_GRAPH_DIRECT_COPY_BENCHMARK=ON
tools/ci/governed-build.sh cmake --build build --target pulp-graph-r2-measurement
cp build/test/pulp-graph-r2-measurement \
  docs/reports/g12-r2-qualification-20261111/benchmark-executable
shasum -a 256 docs/reports/g12-r2-qualification-20261111/benchmark-executable
```

A rebuilt binary matches the recorded digest only on the same toolchain
(`/usr/bin/c++`, version not recorded by the receipt) and SDK; a different
digest means the toolchain differs, not that the receipt is wrong. Re-recording
with the `record` command above produces a fresh receipt bound to the new
binary.

This receipt is bounded measurement evidence, not full G-12 qualification.
