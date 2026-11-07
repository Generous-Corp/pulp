# G-5 host event projection receipt — 2026-11-06

**Disposition: bounded ingress adapter implemented; G-5 remains PARTIAL.**

At source head `87a6adc4c45167bf444afa16d891186deb8dbd99`,
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
ctest --test-dir build -R '^(G5|Typed trigger)' --output-on-failure
11/11 tests passed
```

The positive control checks absolute-to-block offsets, UMP retention, MPE
sidecar derivation, and destination ordering. The planted negatives check
late-event refusal and capacity refusal with destination immutability. The
existing G-5 contract cases continue to pass, including epoch, overflow,
attempt-limit, reset, and partition controls.

The next full-semantics slice still requires a graph-owned host ingress
adapter that binds epoch/reset ownership and block publication to the graph
controller, plus a public installed-SDK consumer that renders the same event
trace through both executor paths. Scheduler, hard-realtime, and plugin-parity
claims remain unqualified.
