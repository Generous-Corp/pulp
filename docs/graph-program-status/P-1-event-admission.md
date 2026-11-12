# P1 — cross-block event admission consumer

Status: **PARTIAL / qualification pending**.

The fixture in `examples/p1-event-admission-consumer` is a public installed-SDK
consumer for the P1 admission gate. It exercises existing MIDI host ingress,
parameter events, trigger graph execution, and a test-owned voice-intent record
that carries the existing G-4 authored identity beside an existing MPE event.

It intentionally adds no runtime queue, voice-intent API, universal event bus,
second scheduler, or graph seam. Qualification requires a complete SDK from the
same source revision and successful execution across two block partitions. The
current installed SDK does not contain the newer graph event header, so the
receipt is **NOT_QUALIFIED**; runtime, realtime, provider-parity, and non-Darwin
lanes remain **NOT_CHECKED**.
