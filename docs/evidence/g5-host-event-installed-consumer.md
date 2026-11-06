# G-5 host event contract: installed-SDK consumer receipt

State: **PARTIAL**. This receipt proves that the bounded host MIDI/MPE/time-index
contract and graph-authored event lane can be consumed from a freshly installed
Release SDK by a project outside the Pulp checkout. It does not claim
SignalGraph wiring, adapter timestamp projection, plugin parity, or hard-realtime
qualification.

## Exact-head provenance

- Source and branch head: `407069a15b28830c02b869ec8b686b5dcd317acc`.
- `origin/main` at validation: `633825f7af3d4c8d0fee7e4ec139588282bdf0a1`.
- The branch merged that current base in `407069a15b28830c02b869ec8b686b5dcd317acc`.
- The source checkout was clean before receipt artifacts were generated. Platform
  was `darwin-arm64`, profile `dev`, and the configured build type was
  `CMAKE_BUILD_TYPE:STRING=Release`.
- The governed `./build/pulp build --all` completed successfully (3,729 targets).
- Installed prefix: `/Volumes/Workshop/Code/agent-worktrees/pulp-g5-host-event-installed-consumer-20261106-1458`.
  It contains 3,662 files and 601,611,243 bytes. Its sorted prefix manifest
  SHA-256 is
  `44a12b8e84a8aa5f9eeaeb07e6329973f46b4c817ac13274e33c7baea039f00b`.
- The installed `graph_event_contract.hpp` and source header both hash to
  `d0e530c8e75604b4b7ef06afca2503335e4bb8a307c82e87edc8077a47d66ca0`.

The external consumer was copied to
`/Volumes/Workshop/Code/agent-worktrees/pulp-g5-host-event-consumer-external-20261106-1458`.
Its two-file source manifest (CMake and main) has SHA-256
`81115b779dfcc77d8dfaa3267f4a28349cc56d4791ee4bb38e2d2de0c3a3a5b7`. The
consumer binary is 118,024 bytes with SHA-256
`6a404496b1d18ecf8bf0c45722e79e8be1f159fc6a1db6dae04fac8b5d5746d8`.
The durable command logs, source copies, and manifests are checked in under
[`g5-host-event-installed-consumer-raw/`](g5-host-event-installed-consumer-raw/).

## Commands and results

The SDK was built and installed before configuring the external consumer:

```sh
./build/pulp build --all
cmake --install build --prefix "$PREFIX" --config Release
cmake -S "$CONSUMER" -B "$CONSUMER/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DPulp_DIR="$PREFIX/lib/cmake/Pulp"
cmake --build "$CONSUMER/build" --parallel 1
ctest --test-dir "$CONSUMER/build" --output-on-failure
"$CONSUMER/build/pulp-g5-host-event-consumer"
```

Configure, single-worker build, CTest, and direct execution all returned zero.
CTest reported `1/1` passed. Direct execution printed:

```text
g5-host-event-contract PASS checks=79 host_mpe_events=3 graph_events=9 planted_negative=1
```

The receipt exercises absolute-sample projection, deterministic producer/tie
ordering, caller-owned MPE sidecar derivation, partition-invariant graph event
tracing, epoch checks, bounded accepted-event overflow, and the per-sample
attempt limit.

## Planted-negative controls

The private-header control checks that
`pulp/host/plugin_slot_vst3_internal.hpp` is absent from the installed prefix,
then attempts to compile an otherwise unchanged consumer. The compile fails
with exit `1` and the expected `fatal error: 'pulp/host/plugin_slot_vst3_internal.hpp'
file not found`.

The wrong-output control changes the expected projected event count from `3` to
`4`. It configures and builds against the same installed prefix, then exits `1`
with `g5-host-event-contract FAIL rule=host-project-count`.

The raw logs include the expected return codes and are hash-listed in
`g5-host-event-installed-consumer-raw/metadata.txt`. They are evidence artifacts,
not prose-only claims.

## Boundary retained

This receipt covers the reviewed fixed-capacity host ingress and graph-authored
lane contract from an installed SDK. The lane remains a contract primitive: it
is not wired into `SignalGraph` admission or Processor dispatch. Host scheduling,
plugin/MIDI adapter parity, graph routing, queued cross-block policy, and
allocation-free hard-realtime measurements remain open.
