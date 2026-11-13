# P1 event admission consumer

Status: **PARTIAL / qualification pending**.

The source fixture is an installed-SDK consumer at
`examples/p1-event-admission-consumer`. It verifies MIDI host ingress and
trigger-graph execution across two blocks. Parameter ordering/overflow and the
voice-intent identity adapter are standalone contract checks; they do not prove
parameter-mailbox or graph voice projection. No runtime API or queue is added.

The fixture requires a complete SDK install and a Release build. Until that
consumer is run from an exact installed SDK, the gate remains `NOT_QUALIFIED`.
The runtime, provider parity, realtime, and non-Darwin lanes remain
`NOT_CHECKED`.

## Exact live-origin recheck — 2026-11-12

This lane was rechecked from a fresh worktree at exact `origin/main`
`e3a6c36e7e3d8efb836659f1bc5ae4dd8a419a5c` (Pulp `1.119.0`). The only
available installed release SDK is `/Users/danielraffel/.pulp/sdk/1.54.0`,
with `sdk-provenance.json` source SHA
`b9c14f9d8332cbb0cbcbec75740331b8ddade4c6` and
`lib/cmake/Pulp/PulpConfig.cmake` SHA-256
`253e3552b00a5845205c7cbf9d23a7f3cc0aca51573df51fe0ea9eb6fe1de794`.
That prefix does not ship `pulp/host/graph_event_contract.hpp`,
`trigger_graph.hpp`, or `voice_identity.hpp`.

The bounded public consumer configure succeeded against that prefix, but the
governed single-worker build failed before compilation with:

```text
fatal error: 'pulp/host/graph_event_contract.hpp' file not found
```

The durable command logs are [live-origin-configure.log](p1-event-admission-consumer-20261112/live-origin-configure.log)
(`5a21785141418bf25c086bbd5a5a13d0870e9e98a05eaf846f7e4db4f426995c`) and
[live-origin-build.log](p1-event-admission-consumer-20261112/live-origin-build.log)
(`0fe0762ad7c4dcc2323e7b052abf1f4bc4b7578f10133c80eb359e1a4e5088e5`).
No CTest or executable run was attempted after the compile refusal.

The source-level seam audit independently remains fail-closed: generic
`inject_parameter_events` publication does not add a routed Processor input
unless the private exact-parameter claim is present
(`core/host/src/signal_graph_prepare.cpp:1122-1127`), and the exact claim is
owned by the private topology-edit path
(`core/host/src/signal_graph_prepared_topology_edit.cpp:2094-2108`).
`TriggerContext` construction is private to the trigger state
(`core/host/include/pulp/host/trigger_graph.hpp:46-58`), while graph voice
projection has no public mailbox or dispatch seam. Therefore the four-family,
two-block P1 gate remains **NOT_QUALIFIED / HOLD**; no runtime/API seam,
universal queue, or voice-intent API is introduced.
