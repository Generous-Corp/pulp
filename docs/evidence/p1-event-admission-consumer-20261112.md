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
