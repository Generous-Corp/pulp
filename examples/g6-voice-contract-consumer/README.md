# Installed SDK allocator contract probe

This standalone consumer characterizes existing MIDI voice-allocation state.
It emits no audio and implements no graph voice API. The
[contract](../../docs/contracts/g6-voice-template-contract.md) distinguishes
existing behavior from missing G-6 bindings; the
[status](../../docs/graph-program-status/G-6.md) records which checks ran.

Set `PULP_SDK_PREFIX` to a complete installed SDK, then run from the Pulp root
on a host admitted for a narrow build:

```sh
cmake -S examples/g6-voice-contract-consumer -B build-g6-consumer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PULP_SDK_PREFIX"
CMAKE_BUILD_PARALLEL_LEVEL=1 tools/ci/governed-build.sh cmake --build build-g6-consumer \
  --target pulp-g6-voice-contract
./build-g6-consumer/pulp-g6-voice-contract
```

The process returns nonzero and names the failed rule; assertions remain active
in Release. The registered planted-negative control inverts the production
allocator's generation equality check in a private staged SDK copy. The
consumer and its stale-event oracle remain unchanged. Run it after the parent
Pulp build has completed on an admitted host:

```sh
ctest --test-dir build -V -R '^cmake-g6-voice-contract-sdk-consumer$'
```

Retain baseline, broken and restored receipts. A compile failure is
inconclusive, not a confirmed negative.

Native POSIX CTest registers `cmake-g6-voice-contract-sdk-consumer`. It stages
the current build's SDK, compares the installed MPE header hashes, and builds
this exact consumer in a separate fixture. It runs the positive probe and the
mutation/restoration loop against the indexed private SDK header, leaving the
consumer and checkout files untouched. Each phase's output and source/SDK
provenance appear in the CTest log and a dedicated transcript in the existing
`Testing/Temporary` CI artifact directory. The helper rejects dirty or mismatched
SDK provenance and compares installed headers and consumer sources to HEAD blobs.
The test has no `slow` label so changes to its registration reach the normal
PR head affected-test lane. Registration alone is not execution evidence.
