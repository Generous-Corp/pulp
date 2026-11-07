# G-5 host event projection receipt — 2026-11-06

**Disposition: bounded ingress adapter implemented; G-5 remains PARTIAL.**

At source fix head `d17bdfe425ad`, based on live `origin/main`
`2f3d36dae75c3dca283c58e47f7731356f088e0d`,
`project_host_event_block` maps a projected `HostEventRecord` span from its
epoch-local absolute sample domain into preallocated MIDI, UMP, and optional
MPE sidecars. It validates the block window and all destination capacities
before mutation, preserves insertion order for equal samples, sorts each
sidecar by block-relative offset, and refuses an out-of-window record instead
of retiming it. UMP and MPE records remain attached to the same publication.

The resulting sidecars are accepted by the existing
`SignalGraph::inject_midi(node, midi, mpe)` mailbox. That mailbox already
bridges the payload into both routed executor paths and the legacy walk, and
the routed Processor binding passes the MPE sidecar through `mpe_input()`.
This change adds no scheduler, queueing, graph-authored callback admission,
or host-time projection.

Focused Release validation:

```text
./build/pulp build --target pulp-test-group-trigger-graph
build/test/pulp-test-group-trigger-graph "[g5]"
All tests passed (322 assertions in 11 test cases)
```

The positive control checks absolute-to-block offsets, UMP retention, MPE
sidecar derivation, and destination ordering. The planted negatives check
late-event refusal, destination-capacity refusal, and a full 128-note
channel-expression fan-out with destination immutability. `derive_mpe` flushes
the tracker pending note-off queue at block start before consuming new records.
The existing G-5 contract cases continue to pass, including epoch, overflow,
attempt-limit, reset, and partition controls. The CTest wrapper was not used
for this receipt because it hung after printing only its project header; the
filtered test executable is the durable focused receipt.

The next full-semantics slice still requires a graph-owned host ingress
adapter that binds epoch/reset ownership and block publication to the graph
controller, plus a public installed-SDK consumer that renders the same event
trace through both executor paths. Scheduler, hard-realtime, and plugin-parity
claims remain unqualified.
