# Real-world inputs MVP

## Goal

Make it straightforward to connect a camera, sensor, controller, analysis
worker, or network service to a Pulp application or plugin without changing the
Pulp SDK for each provider. An integration author should implement capture,
normalization, permissions, and product mapping in an application-owned
adapter, then reuse Pulp's handoff, validation, replay, and acceptance seams.

The MVP is complete when a new provider can be added without adding a provider
dependency, permission API, host SDK, landmark schema, or transport runtime to
Pulp core.

## Design rules

1. **One contract.** Every provider publishes a bounded, complete observation
   with identity, sequence, provider time, validity, confidence, and values.
   Related values are one frame; consumers never assemble a pose from several
   publications.
2. **One handoff.** Native providers use an application-owned `TripleBuffer<T>`
   for latest-value observations. Ordered events use the existing SPSC/event
   paths. External processes use an existing OSC, IPC, MIDI, or WAM ingress.
   Do not add another queue or bus.
3. **One mapping boundary.** Pulp maps an already-published observation to an
   ephemeral control value. Saved parameters and host automation remain the
   base state. Providers never write StateStore from their worker thread.
4. **Provider neutrality.** MediaPipe, OpenCV, ONNX Runtime, Core Motion,
   Android sensors, browser APIs, OSC libraries, and host SDKs remain optional
   application dependencies with application-owned licenses and permissions.
5. **Fail closed.** Invalid, non-finite, low-confidence, future, stale, lost,
   denied, paused, or restarted input has a deterministic fallback and receipt.
6. **Evidence over demos.** A replay and runtime receipt proves the boundary;
   a visual demo is optional and never substitutes for it.

## Canonical model

Add one public, header-only model in `pulp::runtime`:

```cpp
struct ExternalInputFrame {
    std::uint64_t source_id;
    std::uint64_t sequence;
    std::uint64_t provider_time;
    ExternalInputStatus status; // valid, unavailable, denied, malformed, stale
    float confidence;
    std::span<const float> values; // bounded, application-owned storage
};
```

The frame must have a bounded representation suitable for `TripleBuffer`, with
no allocation or span lifetime ambiguity. The SDK owns status, validation,
sequence/restart semantics, age calculation, and mapping policy. The
application owns the meaning and calibration of the values. A small scalar
convenience type may remain for simple controls, but it must use the same
status, validation, and replay rules.

## Work packages

### P0 — Contract and handoff

- Finalize the bounded frame representation and status enum.
- Provide `ExternalInputPolicy`, validation, stale expiry, future-time refusal,
  source identity, and provider restart handling.
- Document the one-writer/one-reader `TripleBuffer` rule and ordered-event
  alternative.
- Add compile-time and runtime tests for allocation-free audio consumption.

**Exit:** a provider can publish a multi-field frame and a processor can read
it without a core SDK change or audio-thread allocation.

### P1 — Replay and failure harness

- Define a versioned, line-oriented replay format containing source identity,
  sequence, provider time, status, confidence, and bounded values.
- Add a deterministic replay utility usable from C++ tests and application CI.
- Cover valid frames, malformed/non-finite values, confidence gating, stale
  expiry, future timestamps, provider loss, pause/resume, sequence reset, and
  competing sources.
- Emit a machine-readable receipt with accepted, rejected, fallback, and
  dropped counts.

**Exit:** every provider adapter can prove behavior without hardware, model
weights, a browser, or a DAW.

### P2 — Transport and ingress recipes

Implement recipes and replay fixtures, not a new universal transport layer:

| Source | MVP path | Required proof |
|---|---|---|
| MIDI/MPE/UMP controller | Existing MIDI event and modulation paths | controller event reaches mapped ephemeral value |
| OSC or companion process | Existing OSC/IPC ingress | malformed packet, disconnect, and stale timeout |
| Browser/WAM | Existing WAM MIDI ingress | short packet rejection and valid packet replay |
| Native worker | `TripleBuffer<ExternalInputFrame>` | no allocation/blocking on audio read |

**Exit:** an application author has copyable adapter recipes and tests for the
three most common external boundaries.

### P3 — Reference provider adapters

Build two small, separate reference adapters outside Pulp core:

- **Motion:** Core Motion on iOS or Android rotation sensor, including
  permission denial, pause/resume, teardown, and one normalized frame.
- **Vision/model:** MediaPipe or OpenCV capture plus an optional ONNX Runtime
  worker, publishing a bounded face/hand/control frame.

Each adapter must use the canonical contract and replay fixtures. Dependencies,
model files, permissions, and licenses stay in the adapter repository or
application target.

**Exit:** one motion and one vision provider reach a Pulp processor in a named
application with runtime logs proving source identity, frame acceptance, stale
fallback, and clean shutdown.

### P4 — Integration documentation and capability surface

- Add a single guide with provider, handoff, lifecycle, mapping, replay, and
  acceptance checklists.
- Add a third-party integration matrix to `NOTICE.md`, `DEPENDENCIES.md`, and
  licensing docs distinguishing bundled, dynamically linked, and
  application-selected software.
- Add an agent task template containing exact files, tests, permissions, and
  receipts required for a new adapter.
- Register the canonical headers and capability descriptions once; do not add
  provider-specific capability rows to Pulp core.

**Exit:** an agent can implement a new adapter by following the checklist and
can identify every artifact needed for review.

## Agent handoff task

Give an implementation agent this sequence:

1. Read this plan and `docs/guides/real-world-inputs.md`.
2. Implement P0 and P1 on a fresh `origin/main` worktree.
3. Reuse `TripleBuffer`, existing SPSC/event facilities, and existing ingress;
   stop if a new queue, provider dependency, or core permission API appears
   necessary.
4. Add replay fixtures before adding a hardware/provider adapter.
5. Implement P2 recipes and tests.
6. Implement exactly one P3 adapter for the named product/platform.
7. Produce the acceptance receipt and update the integration matrix.

The agent must not add camera, sensor, MediaPipe, OpenCV, ONNX Runtime, OSC,
host, or permission dependencies to the Pulp SDK. Any proposed exception needs
a separately named product requirement and a contract review.

## Acceptance receipts

The MVP is ready when all of these exist:

- Public header compile receipt for the canonical contract.
- Replay receipt covering every failure state and deterministic fallback.
- Allocation/blocking probe proving the audio consumer path.
- MIDI, OSC/IPC, and WAM ingress receipts from existing paths.
- One mobile motion runtime receipt.
- One MediaPipe/OpenCV or model-worker runtime receipt.
- Clean teardown receipt for provider stop, permission denial, and host/plugin
  destruction.
- Updated notices and licensing inventory for every application-selected
  dependency.

## Explicit non-goals

The MVP does not add a universal sensor graph, camera service, inference
runtime, landmark schema, permission manager, host integration SDK, network bus,
or demo suite to Pulp core. It does not make a plugin assume camera access;
companion applications and existing host MIDI/automation remain valid paths.
