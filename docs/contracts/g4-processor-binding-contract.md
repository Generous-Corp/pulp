# G-4 Processor, parameter, and event-port binding contract

Status: **design-only prerequisite; no runtime API or G-5 event transport is
introduced by this document**.

This contract defines the smallest public boundary needed before a G-4
subgraph may contain an authored `Processor`. It is an acceptance contract for
a later implementation. It does not select C++ names, add headers, or make a
graph voice claim.

## Current boundary

The shipped G-4 template expands only built-in Input, Output, and Gain nodes.
Its parameter declarations are named paths with finite bounds and currently
target distinct Gain nodes. Exposed ports are validated before graph mutation;
state restore is all-or-nothing for declared parameters. See
[`signal_graph_subgraph.hpp`](../../core/host/include/pulp/host/signal_graph_subgraph.hpp),
the declarations at lines 15–17, 40–54, and 90–116.

`Processor` already owns a descriptor, parameter registration, and host-thread
prepare lifecycle, but the public class does not define a graph-node factory or
an authored graph binding. See
[`processor.hpp`](../../core/format/include/pulp/format/processor.hpp), lines
199–208.

The graph has internal automation and event scratch, while an authored
Processor is currently accepted only by the routed ProcessBlock path. A routed
failure fails closed rather than falling through to the legacy walk. These are
implementation facts, not a public binding contract; see
[`signal_graph_prepare.cpp`](../../core/host/src/signal_graph_prepare.cpp),
lines 977–1005, and
[`signal_graph_process.cpp`](../../core/host/src/signal_graph_process.cpp),
lines 474–481.

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

The implementation is not complete until focused tests prove positive
Processor expansion, audio rendering, named parameter read/write, per-instance
state isolation, and compatible port connection. Planted negatives must prove
that invalid Processor declarations, duplicate paths, unsupported parameter
metadata, incompatible ports, cycles, stale handles, failed preparation, and
partial restore cannot mutate the live graph.

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
