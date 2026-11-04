# G-5 event fixture preparation

**State: PREPARED, runtime HOLD.** The frozen trace oracle in
`examples/g5-event-consumer` checks two legal partitions of the reviewed
one-producer, two-sample event contract. It has one accepted control and planted
one-sample shift, reversed tie, and accepted-capacity failures. This is a
pre-runtime contract fixture only; no graph-produced event API or installed-SDK
consumer has been exercised. The full G-5 gate requires that consumer, output
audio, reset/refusal/budget controls, and realtime measurements.

The active G-10 routed worker owns `signal_graph_authoring.cpp`, prepared
topology and prepare paths needed by runtime integration. Implementation starts
after those exact source claims are released; the fixture can be reused then.
