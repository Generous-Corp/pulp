# P1 — cross-block event admission consumer

Status: **PARTIAL / qualification pending**.

The fixture in `examples/p1-event-admission-consumer` is a public installed-SDK
consumer for the P1 admission gate. It exercises existing MIDI host ingress
and trigger graph execution across blocks. It also contains standalone parameter-queue and test-owned voice-intent
adapter checks; those do not prove graph/mailbox projection and remain
NOT_CHECKED/PLANNED. Parameter-mailbox projection for a public Processor and
public TriggerContext current/future delivery are **NOT_CHECKED / UNAVAILABLE**;
see `docs/evidence/p1-event-admission-consumer-20261112.md:5-14` and the
control disposition in
`docs/evidence/p1-event-admission-consumer-20261112/receipt.json:21-44`. The
trigger-graph trace is not a TriggerContext consumer receipt.

It intentionally adds no runtime queue, voice-intent API, universal event bus,
second scheduler, or graph seam. The fixture now builds and passes from a
complete Release source install whose graph headers are byte-identical to
`origin/main` `5b904b8712667b87d2ecd43f9315c574fc37ec68`, across partitions 4
and 8 (`docs/evidence/p1-event-admission-consumer-20261112.md`, "Installed-SDK
run — 2026-11-13"). That closes the installed-SDK lane for the MIDI and
trigger-graph controls only. The receipt stays **NOT_QUALIFIED** because the
parameter-mailbox and public TriggerContext rows are unavailable through the
public API; runtime, realtime, provider-parity, and non-Darwin lanes remain
**NOT_CHECKED**.
