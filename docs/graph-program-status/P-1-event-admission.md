# P1 — cross-block event admission consumer

Status: **PARTIAL / qualification pending**.

The fixture in `examples/p1-event-admission-consumer` is a public installed-SDK
consumer for the P1 admission gate. It exercises existing MIDI host ingress
and trigger graph execution across blocks. It also contains standalone parameter-queue and test-owned voice-intent
adapter checks; those do not prove graph/mailbox projection and remain
NOT_CHECKED/PLANNED. Parameter-mailbox projection for a public Processor and
public TriggerContext current/future delivery are **NOT_CHECKED / UNAVAILABLE**;
see `docs/evidence/p1-projection-consumer-20261112/README.md:5-19`. The
trigger-graph trace is not a TriggerContext consumer receipt.

It intentionally adds no runtime queue, voice-intent API, universal event bus,
second scheduler, or graph seam. Qualification requires a complete SDK from the
same source revision and successful execution across two block partitions. The
current installed SDK does not contain the newer graph event header, so the
receipt is **NOT_QUALIFIED**; runtime, realtime, provider-parity, and non-Darwin
lanes remain **NOT_CHECKED**.
