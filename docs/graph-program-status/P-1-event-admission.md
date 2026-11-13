# P1 — cross-block event admission consumer

Status: **PARTIAL / qualification pending**.

The fixture in `examples/p1-event-admission-consumer` is a public installed-SDK
consumer for the P1 admission gate. Each of the four typed families runs
through a public seam across at least two blocks and two partition schedules:
MIDI host ingress; triggers through the graph-authored event lane
(`CustomNodeType::process_events`, `SignalGraph::connect_graph_event`,
`GraphEventWriter::emit_at`, `GraphEventReader::events`); parameters through
`SignalGraph::inject_parameter_events` into a `PluginSlot` node; and voice
intent through the installed `VoiceBankBinding`, including stale-generation
refusal by `dispatch_deferred`. Public `TriggerContext` was refused as a seam
and is not used; the existing trigger-graph trace is kept as trigger-graph
evidence. Parameter delivery to a plain `add_processor_node()` `Processor` is
not relied on.

A combined control drives all four families through one test-owned epoch and
absolute sample origin on the same block boundaries. A control-side adapter
projects `HostEventIngress` MIDI into `SignalGraph::inject_midi` and converts
absolute parameter records to block-relative offsets; the graph-authored lane
producer and the `PluginSlot` node share that `SignalGraph`, and the voice bank
renders on the same blocks. The canonical (family, absolute sample, order key)
trace is identical across five schedules, and an in-binary sequence mutation
changes only the touched family's digest while a record diff names only the
touched records. See
`docs/evidence/p1-event-admission-consumer-20261112.md:93-364` and the control
disposition in
`docs/evidence/p1-event-admission-consumer-20261112/receipt.json:8-156`.

It intentionally adds no runtime queue, voice-intent API, universal event bus,
second scheduler, or graph seam. The fixture builds and passes from a complete
Release source install whose headers used here match `origin/main`
`d486ff70c6b02c59acdeac504af747724d1fb906` (one comment-only difference,
recorded in the receipt). Every new control has a planted input mutation that
fails with that control's rule name. The receipt stays **NOT_QUALIFIED**: UMP
and MPE sidecars through `inject_midi` in this fixture, mailbox overflow
telemetry, reset applied to all four families at once, callback allocation
freedom as a measured property, and voice intent as a graph node are
**NOT_CHECKED**; routed/reference executor parity is closed (the combined
control matches on the reference walk and the routed executor across all five
schedules; parallel routing is refused once the graph-event lane is bound and
stays NOT_CHECKED); runtime, realtime,
provider-parity, and non-Darwin lanes remain **NOT_CHECKED**.
