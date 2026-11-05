# G-5 event fixture preparation

**State: PARTIAL bounded runtime; full event-plane HOLD.** The frozen trace oracle in
`examples/g5-event-consumer` checks two legal partitions of the reviewed
one-producer, two-sample event contract. It has one accepted control and planted
one-sample shift, reversed tie, and accepted-capacity failures. This is a
contract fixture for the future event plane. The merged bounded typed runtime
has now been exercised by the exact-head [installed-SDK consumer receipt](../evidence/g5-trigger-runtime-installed-consumer.md),
including a planted wrong-output control. The full G-5 gate still requires
general graph event transport, queue/timestamp semantics, output audio,
reset/refusal/budget controls, and realtime measurements.

The bounded runtime is intentionally limited to the admitted framework trigger
pair and surrounding audio nodes. It does not add arbitrary event producers,
host timestamp transport, or a second scheduler; the frozen event fixture can
be reused when those semantics are explicitly settled.
