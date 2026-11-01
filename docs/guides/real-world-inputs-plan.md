# Real-world inputs: implementation plan

## Goal

Make application-owned camera, sensor, controller, analysis, and network inputs
safe and useful in Pulp without adding providers, permissions, host SDKs, or
demo-specific schemas to Pulp core. A provider publishes one complete
observation; an audio consumer reads the latest observation, applies an
explicit ephemeral mapping, and falls back deterministically when the input is
invalid or stale.

## Stack rank

1. **Foundational contract (this change).** Ship a tiny header-only observation
   and mapping contract that is allocation-free and usable with the existing
   `TripleBuffer<T>`. Validate finite values, confidence, timestamps, and stale
   expiry in one reusable function. Add focused tests for the failure cases.
2. **Replay and acceptance harness.** Add an application/test utility that feeds
   recorded observations through the same mapping path and reports malformed,
   stale, sequence-reset, and provider-loss cases. This proves behavior without
   requiring a camera, model runtime, or DAW.
3. **Transport recipes.** Document narrowly scoped OSC/IPC, MIDI/MPE, and WAM
   ingress adapters that translate into the contract. Keep transports optional
   and outside core; add one live acceptance only when a named product needs it.
4. **Platform adapters.** Add small iOS/Android/browser provider adapters only
   for a named app, with permission and lifecycle receipts. Do not create a
   universal sensor subsystem.
5. **Host integrations.** Treat OBS, Resolve, Max, TouchDesigner, Zoom, and Meet
   as separately verified product boundaries. No host SDK or demo integration is
   part of the foundation.

## Current slice and acceptance

The runtime now provides `ExternalInputSample`, `ExternalInputPolicy`, and
`map_external_input`. The API deliberately leaves capture, inference, transport,
parameter ownership, and arbitration to the application. Acceptance is a focused
unit test covering valid input, confidence gating, stale expiry, non-finite
rejection, and timestamps from the future. The next slice is the replay harness;
it should consume this API rather than introduce another mapping contract.
