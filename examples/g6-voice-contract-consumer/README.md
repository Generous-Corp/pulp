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
in Release. SDK headers are not modified. The planted-negative control changes
the stimulus so a pressure event addresses the live generation instead of the
stolen one, while retaining the same stale-event oracle:

```sh
tools/scripts/confirm_failure.sh \
  --file examples/g6-voice-contract-consumer/main.cpp \
  --break 'perl -0pi -e "s/auto stale = event\(Kind::Pressure, 1\);/auto stale = event(Kind::Pressure, 9);/"' \
  --build-dir build-g6-consumer --target pulp-g6-voice-contract --jobs 1 \
  --test ./build-g6-consumer/pulp-g6-voice-contract --keep-logs
```

Run the control from a clean committed consumer; retain baseline, broken and
restored receipts. A compile failure is inconclusive, not a confirmed negative.

Native POSIX CTest registers `cmake-g6-voice-contract-sdk-consumer`. It stages
the current build's SDK, compares the installed MPE header hashes, and builds
this exact consumer in a separate fixture. It runs the positive probe and the
same mutation/restoration loop against an indexed copy, leaving checkout files
untouched. Each phase's output and source/SDK provenance appear in the CTest log.
The test has no `slow` label so changes to its registration reach the normal
PR head affected-test lane. Registration alone is not execution evidence.
