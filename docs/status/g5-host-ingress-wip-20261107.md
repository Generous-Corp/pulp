# G-5 host ingress WIP checkpoint — 2026-11-07

Status: **DOWNSTREAM ADAPTER INTEGRATED; PLUGIN VOICE DELIVERY REMAINS OPEN**

The lead-owned seam checkpoint `d3995763f6d5c7f8a45f7b970da177722e663c17`
is integrated locally as commit `51ef49ebbec9`. This worktree adds the public
consumer coverage and does not duplicate the seam.

The existing public `HostEventIngress` and `GraphAuthoredEventLane` contract
is complete and bounded, but this slice cannot be wired end to end without
changes to `core/host/include/pulp/host/signal_graph_runtime.hpp`. That header
declares the public `SignalGraph` lifecycle, `CompiledGraph` snapshot state,
and the private process helpers used by both canonical serial dispatch and the
legacy gather path.

## Exact handoff patch

The lead-owned header needs the following declarations before implementation
can proceed:

1. A public host-ingress lifecycle surface on `SignalGraph` that binds one
   prepared `HostEventIngress` to the active trigger epoch: begin/publish,
   checked block projection, reset, and bounded trace/counter inspection.
   Every method must refuse stale generation, late/future, unsupported node
   kinds, and epoch exhaustion before mutating output or queue state.
2. Trigger-controller-owned fixed storage for the ingress queue and its
   generation/epoch identity, charged with the existing 2 MiB trigger resource
   accounting. The queue must preserve absolute sample, producer ordinal and
   sequence, UMP/MIDI payload, and MPE zone/member identity.
3. `CompiledGraph` snapshot fields for the projected MIDI/UMP/MPE records and
   the serial-only ingress binding. The fields must be retained across a
   gap-free prepared swap and detached on reset/stale publication.
4. Private helpers called by `process_trigger_snapshot_` and
   `process_snapshot_impl` to run one checked projection before dispatch, with
   the same preflight/refusal ordering in both paths. Parallel, anticipation,
   fan-out, Plugin/MidiInput/MidiOutput, and legacy gather-node MIDI must remain
   refused or separate as specified by the G-5 packet.

The supplied seam now covers the implementation-owned files:

- `core/host/src/trigger_graph.cpp`: reset/stale/overflow/epoch plumbing and
  serial canonical projection;
- `core/host/src/signal_graph_process.cpp`: serial process preflight and
  Processor event binding;
- focused G-5 tests and the installed public SDK consumer, including partition
  sizes 1/32/64/127/256 and planted missing-edge, one-sample, reversed-tie,
  payload-mismatch, stale, late, and overflow negatives.

No scheduler or second adapter should be added. The existing block mailbox and
legacy gather-node MIDI path remain independent.

## Validation receipt

The governed Release target `pulp-test-group-trigger-graph` built successfully.
The public consumer target `pulp-test-g5-host-event-consumer` built and ran with
`g5-host-event-contract PASS checks=231 host_mpe_events=3 graph_events=9
graph_ingress_events=5 partitions=1,32,64,127,256 planted_negative=5`.
The focused CTest selection passed all 12 tests, including the G-5 contract
cases, trigger runtime consumer, and host-event consumer. A mutation changing
the planted reversed-tie expectation exited 1 with
`FAIL rule=reversed-tie-negative`, then the unmutated positive rerun passed.

Remaining gap: hosted Plugin/voice delivery of graph-owned host MIDI/MPE is
still intentionally refused by trigger-plan admission. This receipt proves
MIDI/UMP/MPE projection through the serial graph MIDI lane and existing
Processor sidecar plumbing only; it does not claim plugin voice parity.

The requested planning files were not present in this checkout or the adjacent
planning repository; the repository's G-5 readiness packet and host-ingress
audit were used as the available authority.

## MIDI mailbox repair — 2026-11-07

PR #11736's macOS gate reported two real failures at head
`7dca7b3ea192130d8c049cedcaf632fd200e73f7`: overflowed caller MIDI was reported
complete, and a gap-free swap retained only one stale event instead of the
bounded prefix. The ingress preflight in `signal_graph_mailbox.cpp` rejected
oversized buffers before `set_from_midi()` could publish its retained prefix
and incomplete marker. Removing that preflight restores caller-visible drops
while preserving the existing fixed-capacity copy and return value.

Governed Release target `pulp-test-host-signal-graph` rebuilt successfully.
The two failing tests now pass, as do all 10 tests matching `SignalGraph MIDI`.
