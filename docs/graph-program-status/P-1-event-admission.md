# P1 — cross-block event admission consumer

Status: **PARTIAL / qualification pending**.

The fixture in `examples/p1-event-admission-consumer` is a public installed-SDK
consumer for the P1 admission gate. Each of the four typed families now runs
through a public seam across at least two blocks and two partition schedules:
MIDI host ingress; triggers through the graph-authored event lane
(`CustomNodeType::process_events`, `SignalGraph::connect_graph_event`,
`GraphEventWriter::emit_at`, `GraphEventReader::events`); parameters through
`SignalGraph::inject_parameter_events` into a `PluginSlot` node; and voice
intent through the installed `VoiceBankBinding`, including stale-generation
refusal by `dispatch_deferred`. Public `TriggerContext` was refused as a seam
and is not used; the existing trigger-graph trace is kept as trigger-graph
evidence. Parameter delivery to a plain `add_processor_node()` `Processor` is
not relied on. See
`docs/evidence/p1-event-admission-consumer-20261112.md:92-182` and the control
disposition in
`docs/evidence/p1-event-admission-consumer-20261112/receipt.json:8-66`.

It intentionally adds no runtime queue, voice-intent API, universal event bus,
second scheduler, or graph seam. The fixture builds and passes from a complete
Release source install whose host headers used here are byte-identical to
`origin/main` `1042181bec4b33ea3180ced14972a8ab01fd63bf`. Every new control has
a planted input mutation that fails with that control's rule name. The receipt
stays **NOT_QUALIFIED**: one shared epoch across all four families,
`inject_midi` projection inside this fixture, mailbox overflow telemetry, the
explainable sequence mutation, callback allocation freedom, more than two
partition schedules, and routed/reference executor parity are **NOT_CHECKED**;
runtime, realtime, provider-parity, and non-Darwin lanes remain
**NOT_CHECKED**.
