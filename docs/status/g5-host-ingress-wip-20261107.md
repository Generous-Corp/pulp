# G-5 host ingress WIP checkpoint — 2026-11-07

Status: **BLOCKED ON LEAD-OWNED SIGNAL GRAPH SEAM**

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

Once that exact seam is supplied, the implementation-owned files are:

- `core/host/src/trigger_graph.cpp`: reset/stale/overflow/epoch plumbing and
  serial canonical projection;
- `core/host/src/signal_graph_process.cpp`: serial process preflight and
  Processor event binding;
- focused G-5 tests and the installed public SDK consumer, including partition
  sizes 1/32/64/127/256 and planted missing-edge, one-sample, reversed-tie,
  payload-mismatch, stale, late, and overflow negatives.

No scheduler or second adapter should be added. The existing block mailbox and
legacy gather-node MIDI path remain independent.

The requested planning files were not present in this checkout or the adjacent
planning repository; the repository's G-5 readiness packet and host-ingress
audit were used as the available authority.
