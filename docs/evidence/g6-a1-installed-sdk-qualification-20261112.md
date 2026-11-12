# G6/A1 installed SDK qualification — 2026-11-12

This is a bounded evidence refresh for the existing G6 voice-bank runtime. It
adds no runtime or public API. The consumer was built from the installed SDK
staged by the Release build at source `f5eeb16bd24d7226a53cbda781f55cad6fbee72f`
(the subsequent `origin/main` move to `fca7fe9e6298` changed planning text only).

## Verified

- SDK provenance: `darwin-arm64`, `Release` artifacts, `source_git_dirty=false`.
  The provenance file SHA-256 is
  `f30882dec0e7ce14612a3927e4a819b2eb25b8d0b86f34960e958893f2a305da`.
- Scalar reference: `A1 scalar reference PASS positives=7 planted_negatives=10`.
- Installed public consumer: governed build and direct execution passed for
  partitions `1,32,64,127,256`, `samples=800`, with the expected sample values.
  The executable SHA-256 is
  `55d3e3423f0e1eb998aca3e4fe43ed3646be45a726435eeaeb1564b60387beec`.
- Mutation proof: `18 positive/planted/restored cases` passed. Sixteen planted
  negative cases returned exit 1 with their expected refusal markers; positive
  and restored cases returned exit 0.
- Full mutation receipt: [g6-a1-installed-sdk-receipt-20261112.json](g6-a1-installed-sdk-receipt-20261112.json), SHA-256
  `f66ed755720f53c8352825e77b04d6d8e2b8e4711f4d86b043afbbe788156446`.

The receipt's top-level `source_git_dirty=true` reflects the generated build,
SDK, and mutation directories present while the proof script collected its
status. Its nested SDK provenance independently records a clean source install.

## Commands

```text
python3 examples/g6-voice-bank-consumer/generate_a1_vectors.py --check
python3 examples/g6-voice-bank-consumer/verify_a1_reference.py
cmake -S examples/g6-voice-bank-consumer -B g6-consumer-build-20261112 -G Ninja \
  -DPulp_DIR=$PWD/g6-sdk-prefix/lib/cmake/Pulp \
  -DCMAKE_PREFIX_PATH=$PWD/g6-sdk-prefix -DCMAKE_BUILD_TYPE=Release
PULP_BUILD_JOBS=1 tools/ci/governed-build.sh cmake --build \
  g6-consumer-build-20261112 --target pulp-g6-voice-bank-consumer
python3 examples/g6-voice-bank-consumer/prove_mutations.py \
  --sdk-prefix $PWD/g6-sdk-prefix \
  --scratch $PWD/g6-mutation-scratch-20261112
```

## Boundary

This qualifies the existing macOS installed-consumer and mutation contract.
It does not establish save/restore, graph swaps, paced callback allocation or
lock bounds, DAW adapter behavior, or Linux/Windows parity. G6 remains
`PARTIAL/HOLD` until those separately admitted requirements have evidence.
