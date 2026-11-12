# P1 event admission consumer

Status: **PARTIAL / qualification pending**.

The source fixture is an installed-SDK consumer at
`examples/p1-event-admission-consumer`. It composes existing MIDI host ingress,
parameter queue, trigger graph, G-4 voice identity, and MPE allocator seams.
The voice-intent record is test-owned and exists only to prove that the current
identity and allocator contracts can be carried together; no runtime API or
queue is added.

The fixture requires a complete SDK install and a Release build. Until that
consumer is run from an exact installed SDK, the gate remains `NOT_QUALIFIED`.
The runtime, provider parity, realtime, and non-Darwin lanes remain
`NOT_CHECKED`.
