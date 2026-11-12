# G-4 Processor, parameter, and event-port binding contract

Status: **design-only prerequisite; no runtime API or G-5 event transport is
introduced by this document**.

This contract defines the smallest public boundary needed for binding an
authored `Processor` to a reusable graph or voice slot. The repository already
has a generic Processor-backed subgraph path; this document specifies the
remaining contract and qualification boundary for stable authored binding. It
does not select new C++ names, add headers, or make a graph voice claim.

## Current boundary

The shipped subgraph template already accepts built-in Input, Output, and Gain
nodes **and** `NodeType::Plugin` Processor nodes. It validates Processor factory
metadata, constructs a `ProcessorNodeInstance` during preflight, binds named
catalog parameters, and rolls back failed expansion before graph mutation.
Exposed ports and all-or-nothing parameter restore are already implemented.
See [`signal_graph_subgraph.hpp`](../../core/host/include/pulp/host/signal_graph_subgraph.hpp),
lines 21–35, 52–62, 128–155, and
[`signal_graph_subgraph.cpp`](../../core/host/src/signal_graph_subgraph.cpp),
lines 20–45, 47–107, and 121–145. The existing positive Processor coverage is
in `test/test_signal_graph_subgraph.cpp`, lines 106–130.

The Processor class owns the descriptor, parameter registration, and host-thread
prepare/release lifecycle at
[`processor.hpp`](../../core/format/include/pulp/format/processor.hpp), lines
223–241. The remaining G-4 question is the stable authored binding contract
for voice slots, lifecycle replacement, and compatible port identity, rather
than whether generic Processor nodes exist.

Processor-backed graphs use the routed ProcessBlock path. A processing failure
clears output and fails closed; a setup/dispatch failure may fall through to
the legacy walk when no routed-only owner is present. Authored Processor nodes
are then explicitly fail-closed and are never reinterpreted by that legacy
walk. See [`signal_graph_process.cpp`](../../core/host/src/signal_graph_process.cpp),
lines 493–526, 534–559. These are implementation facts, not a substitute for
the public binding contract below.

## Required G-4 binding boundary

The eventual implementation must specify and test all of the following as one
transactional authoring contract:

1. **Processor declaration and identity.** A template declares a Processor
   kind/factory and a stable authored path. Expansion validates the declaration
   before mutating the graph, assigns instance identity independently from
   runtime node IDs, and rolls back every allocation on refusal.
2. **Audio binding and lifecycle.** A prepared instance receives the ordinary
   graph audio block through the existing execution plan. Prepare, release,
   replacement, and failed preparation have explicit ownership and silence
   behavior. The callback path performs no graph authoring, allocation, or
   blocking synchronization.
3. **Named parameter catalog.** A template maps stable parameter paths to the
   Processor's public parameter catalog. The mapping records parameter ID,
   plain-domain bounds, default, rate, and read-only status. Unknown, duplicate,
   non-finite, out-of-range, and read-only declarations are rejected before
   graph mutation.
4. **Parameter access and state.** Host edits use the same plain/normalized
   conversion rules as the existing Processor parameter surface. A failed
   multi-parameter restore leaves both the graph and Processor state unchanged;
   per-instance namespaces prevent one authored instance from changing another.
5. **Port declaration.** Audio and control ports have stable names, direction,
   kind, and channel constraints. Port compatibility, duplicate edges, cycles,
   and stale handles are checked before mutation with a stable boundary error.
6. **Event-port seam (G-4 only).** A Processor template may declare that it
   needs event input/output ports, but this contract deliberately leaves event
   timestamps, ordering, queue capacity, late/future handling, reset epochs,
   and cross-block transport unspecified. Those semantics belong to the
   separately reviewed G-5 event-delivery contract. G-4 must not introduce a
   second event queue or name a G-5 event type.

## Acceptance matrix for the implementation slice

The implementation is not complete until focused tests prove the existing
positive Processor expansion plus the remaining authored-binding cases: audio
rendering, named parameter read/write, per-instance state isolation, compatible
port connection, and voice-slot lifecycle replacement. Planted negatives must
prove that invalid Processor declarations, duplicate paths, unsupported
parameter metadata, incompatible ports, cycles, stale handles, failed
preparation, and partial restore cannot mutate the live graph. Existing generic
Processor tests should be retained as regression coverage rather than counted
as new G-4 implementation evidence.

An installed-SDK consumer may be added after the public headers exist. It must
use only published headers, exercise the positive path and at least one
mutation/refusal control, and report the exact SDK/header provenance. This
document itself supplies no execution or realtime evidence.

## Dependencies and stop conditions

This G-4 slice may proceed without importing an unmerged G-5 API. It must stop
at the event-port declaration seam until G-5 settles one event-delivery
contract spanning mailbox publication, routed scratch, legacy execution,
Processor binding, graph swaps, and stale/late/overflow policy. The current
G-5 audit records that dependency and its no-change boundary in
[`g5-host-ingress-audit-20261104.md`](../evidence/g5-host-ingress-audit-20261104.md),
lines 29–53.

Likewise, this contract does not implement graph polyphony, sustain, voice
allocation, persistence, or G-6 completion. Those remain prerequisites called
out by [`g6-voice-template-contract.md`](g6-voice-template-contract.md), lines
85–98.
