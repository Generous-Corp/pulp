# Real-world inputs MVP

## Goal

Make it straightforward to connect a camera, sensor, controller, analysis
worker, network service, or a third-party application that shares such data to
a Pulp application or plugin without changing the Pulp SDK for each provider.
An integration author implements capture, normalization, permissions, and
product mapping in an application-owned adapter, then reuses Pulp's handoff,
validation, replay, and acceptance seams.

The MVP is complete when a new provider can be added without adding a provider
dependency, permission API, host SDK, landmark schema, or transport runtime to
Pulp core.

## What already exists (verify against code, not this page)

The plan builds on shipped seams. An implementation agent reads these first;
none of them is to be re-implemented.

| Seam | Where | Used for |
|---|---|---|
| Latest-value handoff | `pulp::runtime::TripleBuffer<T>` — `write(const T&)`, `write_with(fill)`, `read()`; one writer, one reader; `read()` returns the same value until the next write | native provider → audio callback |
| Ordered bounded events | `pulp::runtime::SpscQueue<T, Capacity>` — `try_push`, `try_pop`, `overflow_count()`, `telemetry()` | every observation must be consumed (gestures, triggers) |
| Coherent multi-word snapshot | `pulp::runtime::SeqLock<T>` — `write`, `read` | audio → UI snapshot of the gate state |
| Payload-free triggers | `pulp::runtime::ActivityChannel<N>` / `SharedActivityChannel<N>` | "lane N fired" indicators |
| UI → audio MIDI | `pulp::midi::MidiControlBus` — `send`, `drain` | a UI control that emits MIDI |
| MIDI in `process()` | `pulp::midi::MidiBuffer`, `MidiEvent::is_cc()/cc_number()/cc_value()/is_pitch_bend()`, `Processor::mpe_input()`, `Processor::ump_input()` | controllers, MPE, UMP |
| MIDI CC → saved parameter | `pulp::state::MidiParameterMap` — `pump()`, `handle_cc()` (writes with `set_normalized_rt`; the host records it as automation) | host-visible controller mapping |
| OSC | `pulp::osc::Receiver::listen(port, handler)`, `listen_with_options(port, ReceiverOptions{on_message, on_bundle, on_error, routes, bind_address})`, `stop()`; `pulp::osc::OscChannel` (a `MessageChannel` with `on_closed` / `on_error`); `pulp::osc::OscParameterMap` | companion process on the same or another machine |
| IPC | `pulp::events::InterprocessConnection` — length-prefixed frames, `connect`, `create_server`, `send_message`, virtual `connection_made()` / `connection_lost()` / `message_received()`, `set_max_message_bytes`, `set_frame_read_timeout`; `pulp::events::ConnectedChildProcess`, `ChildProcessManager` | companion process with lifecycle and disconnect detection |
| WebSocket | `pulp::runtime::WebSocketChannel::connect(tcp, host, path)` / `accept(tcp)`; `MessageChannel` callbacks | apps that publish over WebSocket |
| Browser → WAM | `wam_midi(status, d1, d2, offset)` and `wam_midi_sysex(ptr, len, offset)` exports (`core/format/src/wasm/wam_adapter.cpp`, `wam-runtime.mjs`) | page-side provider into the worklet |
| Ephemeral control value | `pulp::state::StateStore::get_modulated(id)`, `set_mod_offset`, `add_mod_offset`, `set_display_modulation(id, plain)` | base + ephemeral offset, shown by the UI |
| Processor lifecycle | `pulp::format::Processor::prepare(const PrepareContext&)` (`sample_rate`, `max_buffer_size`), `release()`, `process(out, in, midi_in, midi_out, const ProcessContext&)` (`num_samples`, `sample_rate`, `position_samples`, `reset_requested`), `create_view()` | where the consumer resets and reads |
| RT proof | `pulp::test::RtAllocationProbe` (`test/harness/rt_allocation_probe.{hpp,cpp}`), `ScopedRtProcessProbe` (`test/harness/scoped_rt_process_probe.hpp`; trap build also proves lock-freedom), `tools/scripts/confirm_failure.sh` | allocation/blocking receipts |
| Reference product | `examples/cv-control/` — `ControlSample`, `SignalMapping` (confidence gate, smoothing, sample-count expiry), `HandFrame`/`ImuFrame`/`ModelFrame` with their mappings and in-code `*Replay` classes, `decode_midi_control` (channel-16 pitch bend, 13-bit value + validity bit), `FaceToneProcessor` (TripleBuffer reader, optional `osc::Receiver`), `web/control_input.mjs` + `web/validate.mjs` (headless Chrome smoke run by the `web-plugins` workflow) | the executable recipe |
| Mobile adapters | `apple/Sources/PulpSwift/PulpMotionControlSource.swift` (`PulpMotionControlFrame{sequence, value, confidence, valid, timestampNanoseconds}`, `start(updateInterval:) -> Bool`, `stop()`, `PulpMotionControlMath.normalizeTilt`); `android/app/src/main/kotlin/com/pulp/input/MotionControlSource.kt` (`MotionControlFrame`, `start(): Boolean`, `stop()`, `normalizeTilt`) | one scalar tilt per platform, no permission or parameter writes |

Two things do **not** exist on `main` (control: `git grep -c TripleBuffer
origin/main -- core/runtime/include/pulp/runtime/triple_buffer.hpp` returns 6;
`git grep -c "ExternalInputFrame\|ExternalInputSample" origin/main -- core/`
returns nothing):

- a Pulp-owned multi-value observation contract (every current frame type is
  example-owned, and each example mapping is a hand-written fan-out into
  scalar `SignalMapping`s);
- a file-based replay format or a receipt writer (every replay is an in-code
  `std::span` over a Catch2 fixture; receipts in this repository are
  tool-emitted JSON documents with a `schema` id such as
  `pulp.sdk-provenance.v1`, none of them for input replay).

An open sibling PR (`codex/real-world-inputs-foundation-20261031`, Pulp PR
11350) adds a *scalar* `pulp::runtime::ExternalInputSample` /
`ExternalInputPolicy` / `map_external_input` header, a NOTICE section, and a
second plan page. It must be reconciled with this contract before either lands;
see "Decisions needed" and finding B1.

## Design rules

1. **One contract.** Every provider publishes a bounded, complete observation
   with identity, epoch, sequence, provider time, status, confidence, and
   values. Related values are one frame; consumers never assemble a pose from
   several publications.
2. **One handoff per shape.** Latest-value observations use an
   application-owned `TripleBuffer<T>`. Observations that must each be
   consumed use `SpscQueue`. Payload-free triggers use `ActivityChannel`.
   External processes use an existing OSC, IPC, WebSocket, MIDI, or WAM
   ingress whose receive callback is the single writer. Do not add another
   queue or bus.
3. **One mapping boundary.** Pulp validates an already-published observation
   and exposes an accepted value set; the application maps it to an ephemeral
   control value through `get_modulated` / `set_mod_offset` /
   `set_display_modulation`. Saved parameters and host automation remain the
   base state. Providers never write `StateStore` from their worker thread.
4. **Provider neutrality.** MediaPipe, OpenCV, ONNX Runtime, Core Motion,
   Android sensors, browser APIs, OSC/WebSocket client libraries, and host
   SDKs remain application-selected dependencies with application-owned
   licenses and permissions.
5. **Fail closed.** Invalid, non-finite, low-confidence, out-of-order, stale,
   lost, denied, paused, restarted, or wrong-source input has a deterministic
   fallback and a counted verdict.
6. **Evidence over demos.** A replay receipt and a runtime receipt prove the
   boundary; a visual demo is optional and never substitutes for them.
7. **Clocks are not shared.** The provider's clock orders the provider's own
   frames; audio sample counts measure staleness. Nothing in the contract
   assumes a provider knows the audio position.

## Review findings

Severity: **B** blocks an implementation agent, **M** causes rework if left,
**m** minor.

| # | Sev | Finding | Resolution in this plan |
|---|---|---|---|
| B1 | B | Two open PRs define two different core contracts: this plan's multi-value `ExternalInputFrame` and PR 11350's scalar `ExternalInputSample` with `timestamp_samples` + `max_age_samples`. Landing both leaves two mapping paths in `pulp::runtime`. | One contract, below. The scalar is `ExternalInputFrame<1>`. PR 11350 is either rebased onto this shape or withdrawn; it must not land first. |
| B2 | B | `std::span<const float> values` inside a frame handed through `TripleBuffer` is a data race: `TripleBuffer` copies the struct, so the span still points at the provider's storage, which the provider overwrites while the audio thread reads. The plan said "no span lifetime ambiguity" while showing a span. | `std::array<float, Capacity>` + `count`; `static_assert(std::is_trivially_copyable_v<...>)`. Spans appear only on the consumer side, over gate-owned storage. |
| B3 | B | The document is a forward-looking spec in the public `docs/` tree. The standing reviewer holds the PR on placement (internal planning belongs in the private `planning/` submodule; `docs/` documents shipped behaviour). | Not a content change. Listed under "Decisions needed". |
| M1 | M | `provider_time` has no unit, and "future-time refusal" assumes a clock shared between the provider and the audio thread. Core Motion reports mach time, Android reports `SensorEvent.timestamp` nanoseconds, OSC carries nothing unless the sender adds it, and PR 11350's `timestamp_samples` assumes the provider knows the audio sample position. | `provider_time_ns` is the provider's monotonic clock, used only for provider-side ordering and for replay diagnostics. Staleness is `age_samples` since the last accepted frame, advanced by the audio callback (what `SignalMapping::advance` already does). "Future" collapses into "not monotonic within an epoch" and is rejected as `rejected_order`. |
| M2 | M | The status enum mixed provider-declared states (`unavailable`, `denied`) with consumer-computed verdicts (`stale`, `malformed`). A provider cannot declare its own frame stale. | Two enums: `ExternalInputStatus` (provider) and `ExternalInputVerdict` (gate). |
| M3 | M | Restart and sequence reset were underspecified: a sequence that restarts at 1 is indistinguishable from a reordered packet. | `epoch` increments on every provider start; `sequence` is strictly increasing within an epoch; a new epoch is always accepted (and clears age); a lower sequence inside the same epoch is `rejected_order`. |
| M4 | M | Source arbitration was "the application owns it" with no mechanism. Two providers writing one `TripleBuffer` corrupt slot ownership permanently (its header says so). | `source_id` on every frame, `ExternalInputPolicy::source_id` selects; the gate counts `rejected_source`. The writer role moves between providers only through a happens-before (stop A, then start B). |
| M5 | M | No replay file format exists; every replay is an in-code array. "Versioned, line-oriented" was named but not defined. | JSON Lines format `pulp.external-input-replay.v1` and receipt `pulp.external-input-receipt.v1`, field-level below. |
| M6 | M | The transport table claimed an OSC "disconnect" proof. OSC over UDP has no disconnect; the only loss signal is the stale timeout. | OSC proves malformed (`ReceiverOptions::on_error`) + stale; IPC proves disconnect (`InterprocessConnection::connection_lost()`), `OscChannel`/`WebSocketChannel` prove it through `on_closed`. |
| M7 | M | The MIDI recipe did not say which of three existing MIDI paths it meant, and two of them are not ephemeral. | Table under "Transport recipes": `MidiParameterMap` writes the *saved* parameter (host sees automation); a reserved message decoded in `process()` (the `cv-control` channel-16 packet) or `MpeBuffer` feeds the *ephemeral* gate. |
| M8 | M | `cv-control` tests build only with `PULP_BUILD_EXAMPLES=ON`, which the required `macos` gate turns OFF. A contract test placed there never gates. | Core contract tests live in `test/` and are registered in a `test/cmake/*_tests.cmake` manifest without the `slow` label. |
| M9 | M | P4 put the third-party matrix in `NOTICE.md`. `NOTICE.md` attributes code Pulp redistributes; `docs/reference/licensing.md` says developer-supplied SDKs are "intentionally absent from `NOTICE.md`", and `DEPENDENCIES.md` policy says the same for the vendor-SDK carve-out. PR 11350's NOTICE section contradicts this. | The matrix is a table in `docs/reference/licensing.md`; `NOTICE.md` and `DEPENDENCIES.md` are untouched unless Pulp starts fetching or redistributing something. |
| M10 | M | The provider's lifecycle was tied to `prepare()`/`release()` by example. A host calls those on every sample-rate change; a camera permission prompt per `prepare()` is a product bug, and a provider that outlives the processor writes into a destroyed member `TripleBuffer`. | Provider lifecycle belongs to the application object. `prepare()` resets the gate; `release()` only expires it. The handoff is shared ownership (`std::shared_ptr<TripleBuffer<Frame>>`, the `SharedActivityChannel` precedent) so whichever side dies first is safe. `cv-control` keeps its in-`prepare()` OSC listener as a demo convenience and says so. |
| M11 | M | Multi-value frames cannot cross the WAM short-MIDI path: one 3-byte message carries 13 bits + 1 validity bit. | Browser scalar stays on the short packet. Browser multi-value uses the existing `wam_midi_sysex` path with a bounded application-defined payload, and is deferred past the MVP. |
| m1 | m | "Allocation-free audio consumption" had no named instrument. | `RtAllocationProbe` around `process()`; `ScopedRtProcessProbe` where the trap build is available; the test is confirmed with `confirm_failure.sh`. |
| m2 | m | The guide's Android snippet samples `TYPE_ROTATION_VECTOR` while the shipped adapter samples the accelerometer. The guide labels it "for example", so it is not wrong, only loosely matched. | Left as is; the guide is updated when the contract ships (T7). |
| m3 | m | `docs/status/docs-index.yaml` has no entry for this page (`tools/check-docs.sh` warns, does not fail). | Deferred until B3 is decided. |

Answers to the review questions, in one line each:

1. *Multi-value without a universal schema:* yes, once B2/M2 are applied — a
   frame is `count` floats whose meaning the application owns; nothing in Pulp
   names a landmark, axis, or tensor.
2. *Reuse vs duplication:* reuse is correct in intent; B2, M6, M7 were the
   places where the plan would have forced a duplicate or a misuse.
3. *Lifecycle completeness:* specified below (state table), including
   arbitration and teardown order, which were missing.
4. *Replay and receipts sufficient without hardware:* yes once M5 is applied;
   the receipt is produced by the replay utility, not by core at runtime.
5. *Third-party classification:* corrected table below; nothing is
   core-supported.
6. *Agent can follow without inventing APIs:* yes after B1/B2/M1–M11; the
   plan now names every type it expects an agent to write and every existing
   API it expects an agent to call.
7. *Placement:* see "Placement".
8. *Smallest MVP:* T1–T4 + T6 (see "Smallest useful MVP").

## Canonical contract

One header-only, dependency-free, allocation-free file:
`core/runtime/include/pulp/runtime/external_input.hpp`, namespace
`pulp::runtime`, exported through the existing `pulp::runtime` target. Nothing
below allocates, locks, logs, or touches `StateStore`.

```cpp
namespace pulp::runtime {

/// Provider-declared state of one observation. The provider is the only
/// writer of this field; the gate never changes it.
enum class ExternalInputStatus : std::uint8_t {
    valid = 0,    // values[0..count) are meaningful
    invalid,      // provider ran but has nothing to report (tracking lost)
    unavailable,  // provider stopped, hardware absent, or not started
    denied,       // permission refused by the platform or user
    paused,       // application lifecycle paused the provider
};

/// One complete observation. Trivially copyable so it can cross
/// TripleBuffer / SpscQueue by value and be replayed byte-for-byte.
template <std::size_t Capacity>
struct ExternalInputFrame {
    static_assert(Capacity >= 1 && Capacity <= 64, "bounded by design");

    std::uint32_t source_id = 0;          // application-assigned provider identity; 0 = unset
    std::uint32_t epoch = 0;              // increments on every provider (re)start; 0 = never started
    std::uint64_t sequence = 0;           // strictly increasing within an epoch; 0 = never published
    std::uint64_t provider_time_ns = 0;   // provider's own monotonic clock (ordering + diagnostics only)
    ExternalInputStatus status = ExternalInputStatus::unavailable;
    float confidence = 0.0f;              // [0, 1]; providers without a score publish 1 when valid
    std::uint8_t count = 0;               // values in use, <= Capacity
    std::array<float, Capacity> values{};

    static constexpr std::size_t capacity() noexcept { return Capacity; }
};
static_assert(std::is_trivially_copyable_v<ExternalInputFrame<1>>);
static_assert(std::is_trivially_copyable_v<ExternalInputFrame<64>>);

/// Scalar control: the shape the shipped mobile adapters and the
/// cv-control short MIDI packet already produce.
using ExternalInputScalar = ExternalInputFrame<1>;

/// Consumer policy. Owned by the audio-side object; set from prepare()
/// (sample-rate dependent) or by the application before the provider starts.
struct ExternalInputPolicy {
    std::uint32_t source_id = 0;         // accepted source; 0 accepts any (single-provider apps)
    float minimum_confidence = 0.0f;     // frames below this are rejected_confidence
    std::uint64_t max_age_samples = 0;   // audio samples since last accepted frame; 0 = never expires
    std::uint8_t expected_count = 0;     // 0 = any count in [1, Capacity]; otherwise exact
};

/// Gate verdict for one submit() call. Every value is a receipt counter.
enum class ExternalInputVerdict : std::uint8_t {
    accepted = 0,
    unchanged,            // same (epoch, sequence) as the last accepted frame; not an error
    rejected_status,      // status != valid
    rejected_source,      // policy.source_id set and frame.source_id differs
    rejected_order,       // same epoch, sequence <= last accepted, or provider_time_ns went backwards
    rejected_malformed,   // count == 0, count > Capacity, count != expected_count, or any non-finite value/confidence
    rejected_confidence,  // finite confidence outside [0,1] or below minimum_confidence
};

struct ExternalInputCounters {
    std::uint64_t accepted = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t rejected_status = 0;
    std::uint64_t rejected_source = 0;
    std::uint64_t rejected_order = 0;
    std::uint64_t rejected_malformed = 0;
    std::uint64_t rejected_confidence = 0;
    std::uint64_t expired = 0;        // advance() crossed max_age_samples
    std::uint64_t epochs = 0;         // distinct epochs accepted
};

/// Audio-side validator and holder of the current accepted value set.
/// Single-threaded: the reader of the handoff owns it. No allocation, no
/// locking, no logging. Smoothing, scaling, and parameter mapping stay in
/// the application (see examples/cv-control SignalMapping).
template <std::size_t Capacity>
class ExternalInputGate {
public:
    using Frame = ExternalInputFrame<Capacity>;

    /// Validate one published frame against policy. A rejected or unchanged
    /// frame leaves the accepted state untouched. A rejected frame does not
    /// reset age: a provider that only sends garbage still expires.
    ExternalInputVerdict submit(const Frame& frame, const ExternalInputPolicy& policy) noexcept;

    /// Advance the audio clock. Returns true exactly on the call that expires
    /// the accepted value set (age crosses policy.max_age_samples).
    bool advance(std::uint64_t samples, const ExternalInputPolicy& policy) noexcept;

    /// Drop the accepted value set now (prepare(), release(), reset_requested).
    void expire() noexcept;

    bool valid() const noexcept;
    std::span<const float> values() const noexcept;        // empty when !valid()
    float value(std::size_t index, float fallback) const noexcept;
    std::uint32_t epoch() const noexcept;
    std::uint64_t sequence() const noexcept;
    std::uint64_t age_samples() const noexcept;
    ExternalInputStatus last_status() const noexcept;      // status of the last submitted frame, for display
    const ExternalInputCounters& counters() const noexcept;
    void reset_counters() noexcept;
};

} // namespace pulp::runtime
```

Rules the implementation must keep, each with a test (T1):

- `submit` evaluates in this order: `unchanged` → `rejected_source` →
  `rejected_status` → `rejected_malformed` → `rejected_confidence` →
  `rejected_order` → `accepted`. A non-`valid` status is still "seen":
  `last_status()` reports it so a UI can show *denied* vs *lost*, and it
  **does** expire the accepted values immediately (a provider saying
  "unavailable" is an explicit loss notification).
- A frame with `epoch > epoch()` is a restart: it is accepted regardless of
  `sequence`, age resets, `epochs` increments. A frame with `epoch <
  epoch()` is `rejected_order`.
- `advance()` is called once per audio block with `ProcessContext::num_samples`
  when no new frame was accepted in that block (exactly what
  `SignalMapping::advance` does today); the accepted values survive
  `advance()` until age reaches `max_age_samples`.
- `expire()` on `prepare()`, `release()`, and when
  `ProcessContext::reset_requested` is true.
- A `TripleBuffer` reader calls `submit(buffer->read(), policy)` every block;
  the `unchanged` verdict makes that idempotent, so no caller keeps a
  "consumed sequence" variable (which `FaceToneProcessor` does by hand today).

Ownership and threading:

| Object | Owner | Thread |
|---|---|---|
| `ExternalInputFrame` storage inside the handoff | shared: `std::shared_ptr<TripleBuffer<Frame>>` (or `SpscQueue`) created by the application, held by provider and consumer | writer: provider thread or ingress callback thread; reader: audio |
| `ExternalInputPolicy` | consumer (processor) | written in `prepare()` or before the provider starts; read on audio |
| `ExternalInputGate` | consumer (processor) | audio only |
| `ExternalInputCounters` snapshot for UI | consumer publishes through `SeqLock<ExternalInputCounters>` or `TripleBuffer` if the UI shows them | audio writes, UI reads |
| Provider object, permissions, device, model, inference | application | provider thread; start/stop from the application's lifecycle thread |

## Handoff selection

| Observation shape | Use | Overflow / loss evidence |
|---|---|---|
| Latest value wins (pose, tilt, model output, slider) | `TripleBuffer<ExternalInputFrame<N>>` | none by construction; staleness via `advance()` |
| Every observation matters (tap, gesture onset, button) | `SpscQueue<ExternalInputFrame<N>, Capacity>`; audio drains with `try_pop` until empty, submitting each | `overflow_count()` → runtime receipt `published_dropped` |
| Payload-free trigger | `ActivityChannel<Lanes>` | `consume_count()` |
| The observation *is* MIDI | `MidiBuffer` in `process()` (host path) or `MidiControlBus` (UI path) | `MidiBuffer` dropped-event counters under the realtime capacity limit; `MidiControlBus::send` returns false when full |
| Provider in another process | OSC (`Receiver`), IPC (`InterprocessConnection`), WebSocket (`WebSocketChannel`); the receive callback is the single `TripleBuffer` writer | OSC: `on_error` + stale; IPC/WS: `connection_lost()` / `on_closed` → publish one `unavailable` frame |
| Provider in a browser page, consumer in a WAM worklet | scalar: reserved short MIDI message (`decode_midi_control`); multi-value: `wam_midi_sysex` (deferred) | decoder returns `nullopt` → not submitted |

## Lifecycle, permissions, loss, arbitration, teardown

Every row is a replay fixture (T2) and, for live adapters, a runtime receipt
line (T6).

| Event | Provider publishes | Gate result | Application fallback |
|---|---|---|---|
| Start, permission granted | `epoch+1`, `sequence=1`, `status=valid` | `accepted`, `epochs+1` | normal mapping |
| Start, permission denied | one frame `status=denied` | `rejected_status`, `last_status()==denied`, values expired | base parameter; UI may show "denied" |
| Hardware/provider absent | one frame `status=unavailable` | as above with `unavailable` | base parameter |
| Tracking lost but provider alive | `status=invalid` per update | `rejected_status`, values expired | base parameter |
| Low confidence | `status=valid`, low `confidence` | `rejected_confidence`; previous values kept until age expires | last good value, then base |
| Malformed (NaN/Inf, bad count) | whatever arrived | `rejected_malformed`; previous values kept | last good value, then base |
| Provider stalls silently | nothing | `advance()` crosses `max_age_samples` → `expired` | base parameter |
| Pause (app background, audio session interrupted) | one frame `status=paused`, then stop publishing | `rejected_status`, values expired | base parameter |
| Resume | `epoch+1`, `sequence=1`, `valid` | accepted as a restart | normal |
| Restart / crash recovery | `epoch+1` | accepted; lower `sequence` in the *old* epoch is `rejected_order` | normal |
| Reordered / duplicate packet (UDP) | same epoch, lower or equal sequence | `rejected_order` / `unchanged` | unaffected |
| Second provider selected | application stops A (A publishes `unavailable`), sets `policy.source_id = B`, starts B | frames still arriving from A are `rejected_source` | base until B's first frame |
| Host sample-rate change | nothing (provider keeps running) | `prepare()` recomputes `max_age_samples`, calls `expire()` | base until next frame |
| Host reset (`reset_requested`) | nothing | `expire()` | base |
| Teardown | `stop()` → provider's last callback returns → provider publishes one `unavailable` frame → provider releases its `shared_ptr` | `release()` calls `expire()` | n/a; the shared handoff outlives whichever side dies first |

Permissions are platform-owned. The only Pulp-visible artefacts of a
permission are the `denied` status and the runtime receipt's
`permission` field. Pulp never prompts, never stores a grant, and never reads
a platform permission API.

## Replay format and receipts

### Replay fixture — `pulp.external-input-replay.v1`

UTF-8 JSON Lines. Line 1 is the header; every later line is one record. The
reader rejects an unknown `schema`, an unknown record `t`, or a `values`
array longer than `capacity`. Non-finite values are written as the strings
`"nan"`, `"inf"`, `"-inf"` because JSON cannot carry them; the reader maps
them to the IEEE values so malformed-input fixtures are first-class.

```json
{"schema":"pulp.external-input-replay.v1","capacity":3,"sample_rate":48000,"block_samples":128,"policy":{"source_id":1,"minimum_confidence":0.6,"max_age_samples":12000,"expected_count":3},"description":"imu tilt with pause/resume"}
{"t":"frame","source_id":1,"epoch":1,"sequence":1,"provider_time_ns":1000000,"status":"valid","confidence":1.0,"values":[0.1,0.2,0.3]}
{"t":"expect","verdict":"accepted","valid":true,"values":[0.1,0.2,0.3],"tolerance":1e-6}
{"t":"advance","blocks":94}
{"t":"expect","valid":false,"expired":1}
{"t":"frame","source_id":1,"epoch":1,"sequence":2,"provider_time_ns":2000000,"status":"valid","confidence":1.0,"values":["nan",0.2,0.3]}
{"t":"expect","verdict":"rejected_malformed","valid":false}
{"t":"frame","source_id":1,"epoch":2,"sequence":1,"provider_time_ns":500,"status":"valid","confidence":1.0,"values":[0.5,0.5,0.5]}
{"t":"expect","verdict":"accepted","epoch":2,"counters":{"epochs":2}}
```

Record fields:

| `t` | Required | Optional |
|---|---|---|
| `frame` | `source_id`, `epoch`, `sequence`, `status` (`valid`/`invalid`/`unavailable`/`denied`/`paused`), `confidence`, `values` | `provider_time_ns` (default 0) |
| `advance` | `blocks` (integer, each `block_samples` long) or `samples` | — |
| `expect` | at least one of `verdict`, `valid`, `values`, `epoch`, `sequence`, `age_samples`, `expired`, `counters` (partial object) | `tolerance` (default `1e-6`) |
| `reset` | — | — (calls `expire()`, models `prepare()`/`reset_requested`) |

The same fixture drives: the core gate directly (T2), `cv-control`'s
`SignalMapping` through the gate (T3), and any application mapping that
exposes `submit`/`advance`.

### Replay receipt — `pulp.external-input-receipt.v1`

Written by the replay utility after a run, one JSON document.

```json
{
  "schema": "pulp.external-input-receipt.v1",
  "fixture": "test/fixtures/external-input/imu-pause-resume.jsonl",
  "fixture_sha256": "…",
  "capacity": 3,
  "policy": {"source_id": 1, "minimum_confidence": 0.6, "max_age_samples": 12000, "expected_count": 3},
  "records": {"frames": 3, "advances": 1, "expects": 4, "resets": 0},
  "counters": {"accepted": 2, "unchanged": 0, "rejected_status": 0, "rejected_source": 0, "rejected_order": 0, "rejected_malformed": 1, "rejected_confidence": 0, "expired": 1, "epochs": 2},
  "final": {"valid": true, "values": [0.5, 0.5, 0.5], "epoch": 2, "sequence": 1, "age_samples": 0, "last_status": "valid"},
  "expectations": {"checked": 4, "failed": 0},
  "failures": [],
  "result": "pass"
}
```

`failures[]` entries are `{"line": n, "field": "...", "expected": …,
"actual": …}`. `result` is `pass` only when `failed == 0` **and** every
record parsed. A fixture with zero `expect` records yields `result:
"inconclusive"`, never `pass` — a receipt that could not fail is not a
receipt. A negative-control fixture (every expectation deliberately wrong)
ships beside the positive ones and must produce `result: "fail"` (the
"pair every negative finding with a control" rule).

### Runtime receipt — `pulp.external-input-runtime-receipt.v1`

Written by an **application** (not by Pulp core) when a live provider stops.
Core contributes only `ExternalInputCounters`; the application serializes.

```json
{
  "schema": "pulp.external-input-runtime-receipt.v1",
  "product": "cv-control-ios-sample",
  "platform": "iOS 19.1 / iPhone16,1",
  "provider": "CMMotionManager deviceMotion",
  "source_id": 1,
  "permission": "granted",
  "epochs": [{"epoch": 1, "started_ns": 0, "stopped_ns": 30100000000, "frames_published": 1806, "published_dropped": 0}],
  "counters": {"accepted": 1806, "unchanged": 0, "rejected_status": 2, "rejected_source": 0, "rejected_order": 0, "rejected_malformed": 0, "rejected_confidence": 0, "expired": 1, "epochs": 1},
  "teardown": {"provider_stopped_before_consumer_destroyed": true, "last_frame_status": "unavailable", "audio_allocations_observed": 0},
  "pause_resume_events": 1
}
```

`permission` is one of `granted`, `denied`, `not_required`, `not_requested`.
`permission` and `teardown` are the two fields a reviewer reads first; a
receipt without them is incomplete.

## Placement

| Layer | Contents | Where | Why |
|---|---|---|---|
| **Pulp core** | `ExternalInputStatus`, `ExternalInputFrame<N>`, `ExternalInputScalar`, `ExternalInputPolicy`, `ExternalInputVerdict`, `ExternalInputCounters`, `ExternalInputGate<N>` | `core/runtime/include/pulp/runtime/external_input.hpp` (header-only); tests in `test/test_external_input.cpp` | the one thing every provider and every product shares; zero dependencies; must be gated by the required `macos` check |
| **Pulp core, non-RT** | replay reader + receipt writer (`ExternalInputReplay`, `run_external_input_replay(path, Gate&, policy) -> ExternalInputReplayReceipt`, `write_receipt(path, receipt)`) using the vendored `choc::json` | `core/runtime/include/pulp/runtime/external_input_replay.hpp` + `core/runtime/src/external_input_replay.cpp` | an application's CI must run the same fixtures Pulp runs, which `test/support/` cannot provide to an installed SDK. ~200 lines, never touched by the audio thread. If the reviewer prefers it out of core, the fallback is `test/support/external_input_replay.{hpp,cpp}` plus a copy in the recipe, at the cost of drift. |
| **Shared application support (recipes, copied not linked)** | smoothing/scaling mapping (`SignalMapping`), OSC/IPC/WebSocket/MIDI/WAM ingress adapters that build a frame, the browser `control_input.mjs`, `PulpMotionControlSource` (Swift), `com.pulp.input.MotionControlSource` (Kotlin) | `examples/cv-control/`, `apple/Sources/PulpSwift/`, `android/app/src/main/kotlin/com/pulp/input/` | application-owned by definition; versioned with the repo so recipes cannot cite an API that does not exist at the same commit. Packaging them as a `pulp kit` (`kind: source`) is a follow-up once two products copy them. |
| **Product** | provider SDK linkage (MediaPipe/OpenCV/ONNX/Core ML/LiteRT), model files, permission strings and prompts, calibration, source-selection UI, companion processes, host-specific bridges, runtime receipt writer | the application or a separate repository | license, permission, and lifecycle ownership |

Dependency ordering: core contract → replay/receipt → recipes migrate to the
contract → mobile adapters emit the contract → licensing/doc table → optional
vision adapter and host demos.

## Third-party classification

Nothing in this table is core-supported. "Optional application-owned" means a
recipe may show it and a product may link it; Pulp fetches, links, and
redistributes none of it.

| Technology | Class | Boundary into Pulp | Notes |
|---|---|---|---|
| MediaPipe (Apache-2.0), OpenCV (Apache-2.0), OpenSeeFace | optional application-owned provider | native worker → `TripleBuffer<Frame<N>>`; browser → short MIDI / sysex | model/task bundles carry their own terms; audit separately |
| ONNX Runtime (MIT), LiteRT, Core ML | optional application-owned inference | same as above | the worker publishes only the application-owned outputs |
| Core Motion, Android `SensorManager` | optional application-owned platform sensor | `PulpMotionControlSource` / `MotionControlSource` recipe → application handoff | permissions and pause/resume are platform-owned |
| Browser DeviceMotion, getUserMedia, WebHID, Web Serial, Web Bluetooth | optional application-owned (page) | `control_input.mjs` → WAM MIDI ingress | secure origin + user gesture required |
| OSC senders (TouchOSC, TouchDesigner, Max, Pd, openFrameworks, vvvv) | separately verified sharing apps | `pulp::osc::Receiver` (core, already shipped) | no client library needed on the Pulp side |
| MIDI/MPE controllers | core-supported ingress (already shipped) | `MidiBuffer`, `MpeBuffer`, `MidiParameterMap` | nothing to add |
| WAM / browser page | core-supported ingress (already shipped) | `wam_midi`, `wam_midi_sysex` | scalar now; multi-value deferred |
| OBS Studio (GPL-2.0) | separately verified host | companion process over OSC/WebSocket/UDP, or a virtual device | OBS-side code is a GPL project in its own repository; it may *consume* Pulp's MIT code; Pulp never includes OBS code |
| DaVinci Resolve / Fairlight | separately verified host | VST3/AU parameter (shipped); optional companion bridge for scripting/control | no Blackmagic SDK in Pulp |
| Max/MSP, TouchDesigner, openFrameworks, vvvv, Pure Data | separately verified sharing apps and hosts | OSC/MIDI/IPC named signals | no Pulp node graph, no landmark schema |
| Zoom, Google Meet | separately verified product requirement | companion app, virtual microphone/camera, or a verified SDK | never assume a call host loads a plugin, accepts OSC, or grants camera |

Doc placement for this table when it ships: a new "Application-selected
provider and host integrations" subsection in `docs/reference/licensing.md`
under "Optional Vendor SDK Integrations", mirroring the "Developer-Supplied
Vendor SDKs" convention (absent from `NOTICE.md`, no `DEPENDENCIES.md` row).

## External host boundaries

Hosts and capability-sharing applications are compatibility forcing
functions, not a new Pulp provider or transport API. They consume existing
plugin, OSC, MIDI, IPC, WebSocket, or external-process boundaries through
optional recipes. Pulp core never links a host SDK or copies host code. A host
adapter becomes implementation work only when a named application requires a
live acceptance receipt, and every host demo names the boundary it exercises.

### Boundary taxonomy

| Boundary | Pulp provides | Companion / host owns | Proof |
|---|---|---|---|
| **In-host plugin parameter** (VST3/AU/CLAP/LV2/AAX automation, host MIDI) | the format adapters, `MidiParameterMap`, `get_modulated`, display modulation | the host's automation, scheduling, and UI | existing adapter tests + a `pluginval`/`auval` run; host automation reaches the parameter |
| **Companion process stream** (a helper the product ships, or a sharing app) | `InterprocessConnection`, `ConnectedChildProcess`, `osc::Receiver`, `WebSocketChannel`; the receive callback publishes one frame | camera/device permission, inference, process lifecycle, the wire schema | IPC disconnect → `unavailable` frame; replay fixture recorded from a real session |
| **Virtual audio/video device** (virtual mic/cam/loopback) | nothing new: the device appears as an ordinary input bus or `AudioInputProvider` | the driver, its installer, and its signing | audio arrives on the bus; no control frame is involved unless the companion also publishes one |
| **Network protocol bridge** (OSC, obs-websocket-style JSON over WebSocket, MIDI over network, NDI-like discovery) | `osc::Receiver`, `WebSocketChannel`, `pulp::events::ServiceBrowser` / `ServicePublisher` for discovery; frame construction is application code | the protocol's authentication, versioning, and reconnect policy | malformed → `on_error`; closed → `on_closed` → `unavailable`; stale timeout |
| **Browser page** | WAM adapter, `wam_midi` / `wam_midi_sysex`, the web runtime | permissions, secure origin, user gesture, page lifecycle | headless Chrome smoke (`web/validate.mjs`) + decoder rejection test |

### Recipe template (one per host, filled in by the agent)

```
Host / app:            <name and version tested>
Boundary:              <one row of the taxonomy>
Direction:             <host → Pulp | Pulp → host | both>
Transport:             <existing Pulp API: osc::Receiver / InterprocessConnection / WebSocketChannel / MIDI / WAM>
Wire schema:           <application-owned; example message; what maps to source_id/epoch/sequence/status/confidence/values>
Permissions:           <who prompts, where the grant lives, how denial reaches the frame>
Lifecycle:             <start/stop/pause/resume/reconnect; who publishes `unavailable`>
Policy:                <source_id, minimum_confidence, max_age_samples, expected_count>
License boundary:      <host license; what code lives where; nothing of the host in Pulp>
Replay fixture:        <path; recorded from a real session or synthesized; includes loss + malformed>
Receipts:              <replay receipt path; runtime receipt path if live>
Not proven:            <what the demo deliberately does not claim>
```

### Applications that share capabilities

A third-party app that publishes tracking or analysis (face/hand data over
OSC from TouchDesigner, a Max patch sending `/pose/*`, a Python worker on a
WebSocket, an NDI-style metadata stream) maps onto the same contract with no
new Pulp code:

1. The receive callback (`osc::Receiver` handler, `message_received`,
   `on_message`) is the single writer of one `TripleBuffer<Frame<N>>`.
2. The application assigns `source_id` per connection and bumps `epoch` on
   every (re)connect; `sequence` is the app's own counter unless the wire
   carries one.
3. The wire's own confidence/validity, if any, becomes `confidence` and
   `status`; a message that does not parse is not published (counted by the
   adapter, reported in the runtime receipt).
4. `on_closed` / `connection_lost()` publishes one `unavailable` frame; UDP
   sources rely on `max_age_samples`.
5. The recorded session is checked in as a replay fixture, so the
   integration is tested forever without the app.

### License and permission guardrails

- Never link, vendor, or copy GPL/LGPL host code (OBS) into Pulp or into an
  MIT-licensed recipe. A GPL host plugin is its own repository and may use
  Pulp under MIT.
- Never add a host or vendor SDK (Blackmagic, Zoom, NDI, Spout, Syphon) as a
  Pulp dependency; a product that needs one follows the developer-supplied
  SDK carve-out in `docs/reference/licensing.md`.
- Never store, request, or infer a platform permission in Pulp; the frame
  carries `denied` and the runtime receipt carries `permission`.
- Network listeners bind loopback by default (`ReceiverOptions::bind_address`,
  `create_server` without a host); a recipe that binds all interfaces says why.
- A sharing app's data may be personal (faces, hands, voice). A recipe stores
  only the normalized values the mapping needs, never raw frames, and the
  replay fixture is synthesized or consented.

### Host demos worth doing first

1. **OSC sharing app → native `cv-control`** (TouchDesigner or Max patch
   publishing `/pulp/control`): zero new Pulp code, proves the network
   protocol bridge and the recorded-fixture discipline. One afternoon.
2. **Fairlight VST3 parameter**: proves the in-host plugin boundary with the
   shipped adapter and host automation; no SDK. The receipt is the host's
   automation lane driving `get_modulated`.
3. **OBS companion over WebSocket/OSC**: proves the companion-process stream
   without touching GPL code; the companion is a separate repository.
4. **Zoom/Meet via virtual device**: deferred until a product names it; needs
   a driver or a verified SDK and a separate permission review.

## Transport recipes (tests live in `test/`, not only in examples)

| Source | Existing path | Ephemeral or saved | Required proof (T4) |
|---|---|---|---|
| MIDI CC → parameter | `MidiParameterMap::handle_cc` | **saved** (host records automation) | existing `MidiParameterMap` tests; the recipe only names it |
| MIDI reserved message → frame | decode in `process()` (`decode_midi_control` shape) | ephemeral | bad status / data byte > 0x7F → not published; valid → `accepted` |
| MPE per-note | `Processor::mpe_input()` → `ExternalInputFrame<3>` (bend, pressure, timbre) per voice if the product wants it ephemeral | ephemeral | optional; `mpe` skill |
| OSC | `Receiver::listen_with_options` with `on_error` + handler publishing a frame | ephemeral | malformed packet counted via `on_error`; wrong arg types not published; silence → `expired` |
| IPC | `InterprocessConnection` subclass: `message_received` publishes, `connection_lost` publishes `unavailable` | ephemeral | disconnect → `last_status()==unavailable` within one block; oversized frame rejected by `set_max_message_bytes` |
| WebSocket sharing app | `WebSocketChannel::connect`, `on_message` publishes, `on_closed` publishes `unavailable` | ephemeral | same as IPC |
| Browser → WAM | `control_input.mjs` → `wam_midi` → `decode_midi_control` | ephemeral | existing `test_cv_control.cpp` short-packet case + `web/validate.mjs` |
| Native worker | `std::shared_ptr<TripleBuffer<Frame<N>>>` | ephemeral | `RtAllocationProbe` around `process()` reports 0 |

## Work packages, ranked (each is one PR with its acceptance test)

| # | PR | Files | Acceptance | Depends on |
|---|---|---|---|---|
| T1 | **Core contract + gate** | `core/runtime/include/pulp/runtime/external_input.hpp`; `test/test_external_input.cpp`; register `pulp-test-external-input` in `test/cmake/core_runtime_canvas_signal_tests.cmake` (the `pulp::runtime` owner; `pulp-test-sync` sits in `format_reload_tests.cmake` for historical reasons — do not add there), `LIBRARIES pulp::runtime`, `SOURCES test_external_input.cpp harness/rt_allocation_probe.cpp`, **no `slow` label** | one `TEST_CASE` per verdict and per lifecycle row; `static_assert` trivially copyable for `<1>` and `<64>`; `RtAllocationProbe` reports 0 allocations across 10 000 `submit`/`advance` calls; `TripleBuffer` round trip with `unchanged` dedupe; `confirm_failure.sh --file …/external_input.hpp --break "perl -0pi -e 's/rejected_order/accepted/'" --target pulp-test-external-input` returns CONFIRMED. Reconcile or close PR 11350 in the same change. | — |
| T2 | **Replay reader + receipt writer + fixtures** | `core/runtime/include/pulp/runtime/external_input_replay.hpp`, `core/runtime/src/external_input_replay.cpp`; `test/fixtures/external-input/*.jsonl` (one per lifecycle row + `negative-control.jsonl`); `test/test_external_input_replay.cpp` in the same manifest | every fixture → `result: "pass"`; negative control → `result: "fail"`; zero-expect fixture → `"inconclusive"`; `"nan"` token round-trips; unknown schema rejected; receipt JSON validated field-by-field by the test (schema id, counters sum to frames, `fixture_sha256` matches) | T1 |
| T3 | **`cv-control` consumes the contract** | `examples/cv-control/cv_control.hpp` (`SignalMapping` keeps smoothing, drops its own validation; `*Replay` classes replaced by the JSONL fixtures), `cv_control_processor.hpp` (gate replaces `consumed_sequence_`, `shared_ptr<TripleBuffer>`), `README.md`; web unchanged | `pulp-cv-control-test` green; `web/validate.mjs` green in the `web-plugins` workflow; the example's fixtures are the T2 fixtures (no second copy) | T2 |
| T4 | **Ingress recipe tests in `test/`** | `test/test_external_input_ingress.cpp` (OSC malformed via `on_error`, OSC wrong-type args, IPC `connection_lost` → `unavailable`, reserved-MIDI decode rejection); same manifest; `LIBRARIES pulp::runtime pulp::osc pulp::events pulp::midi` | each case asserts the gate verdict and `last_status()`; the IPC case uses the existing `pulp-connected-child-process-fixture` pattern from `pulp-test-ipc` | T1 |
| T5 | **Licensing table + guide update** | `docs/reference/licensing.md` (new subsection), `docs/guides/real-world-inputs.md` (point the recipe at `ExternalInputGate`, fix the Android sensor type), `docs/status/docs-index.yaml` as needed | `tools/check-docs.sh` exit 0; `docs_noise_lint.py --mode report` exit 0 | T1 |
| T6 | **Mobile adapters emit the contract + runtime receipt** | `apple/Sources/PulpSwift/PulpMotionControlSource.swift` (frame gains `sourceId`, `epoch`, `status`), the iOS sample app writes `pulp.external-input-runtime-receipt.v1` on stop; `android/app/.../MotionControlSource.kt` same; unit tests in `apple/Tests/PulpSwiftTests/` and `android/app/src/test/` | Swift + Kotlin tests cover denied, pause/resume (epoch bump), stop → `unavailable`; the Android `android-run-fixtures` lane uploads one runtime receipt with `permission` and `teardown` populated | T1 |
| T7 | **OSC sharing-app host demo** | `examples/cv-control/recipes/osc-sharing-app.md` (filled template) + one recorded fixture | fixture passes T2; the recipe names what is not proven | T3 |
| T8 | **Browser multi-value over sysex** | `control_input.mjs`, decoder in `cv_control.hpp`, `web/validate.mjs` | bounded payload; oversized/short rejected; headless smoke green | T3 |
| T9 | **Vision/model reference adapter** (separate target or repository) | OpenCV/MediaPipe capture + optional ONNX worker publishing `Frame<N>`; its own licensing file | runtime receipt with `permission`, `teardown`, `expired ≥ 1` (cover unplugged camera); replay fixture recorded from the worker | T2, T5 |
| T10 | **Fairlight VST3 host demo** | recipe + fixture | host automation reaches `get_modulated`; receipt names the host version | T3 |

## Smallest useful MVP

T1 + T2 + T3 + T4 + T6. After these, an agent can add a provider by writing
one adapter that publishes `ExternalInputFrame<N>` into a shared
`TripleBuffer`, prove it with a JSONL fixture and receipt, and ship a runtime
receipt from the device — without any core change. T5 lands with the first
of them that changes public behaviour.

Explicitly deferred: T8 (multi-value browser packet), T9 (vision adapter; needs
a named product for the model-file licensing), T7/T10 host demos (one
afternoon each, after a product asks), a `pulp kit` for the recipes, a CLI
wrapper for the replay utility (`pulp input replay …`), and any UI widget.

## Stop conditions (read before writing code)

- About to write a lock-free queue, ring, or "latest value" holder → **stop**:
  `TripleBuffer`, `SpscQueue`, `SeqLock`, `ActivityChannel` already exist.
- About to add a transport, socket, pipe, or message framing → **stop**:
  `osc::Receiver`, `OscChannel`, `InterprocessConnection`, `WebSocketChannel`,
  `NamedPipe`, `MessageChannel` already exist; WAM ingress is `wam_midi` /
  `wam_midi_sysex`.
- About to name a landmark, axis, joint, tensor layout, or units in core →
  **stop**: values are `count` floats; meaning lives in the application.
- About to add a provider SDK, model runtime, camera, sensor, or permission
  call under `core/` → **stop**: product or recipe only.
- About to read a provider timestamp on the audio thread to decide staleness →
  **stop**: use `advance()` with `num_samples`.
- About to write to `StateStore` from a provider callback → **stop**: publish
  a frame; the audio thread maps.
- About to write a second mapping/validation function beside
  `ExternalInputGate` (as PR 11350's `map_external_input` would be) → **stop**:
  extend the gate's tests instead.
- About to put a contract test under `examples/` → **stop**: it will not gate.
- About to add a NOTICE/DEPENDENCIES row for something Pulp does not fetch,
  link, or redistribute → **stop**: licensing.md table.
- About to claim "no hardware needed" without a replay receipt, or "works in
  host X" without a filled recipe template → **stop**.

## Agent handoff checklist

1. Fresh worktree from `origin/main` (never `/tmp`); `git submodule update
   --init planning` if the planning copy of this document is the live one.
2. Read, in order: this document; `docs/guides/real-world-inputs.md`;
   `core/runtime/include/pulp/runtime/triple_buffer.hpp`, `spsc_queue.hpp`,
   `activity_channel.hpp`; `examples/cv-control/cv_control.hpp` and
   `cv_control_processor.hpp`; `test/test_sync_primitives.cpp`;
   `test/harness/rt_allocation_probe.hpp`; skills `streams`, `mpe`,
   `web-plugins`, `view-bridge`, `code-comments`, `test-audit`.
3. Check PR 11350's state; coordinate (rebase onto T1 or close) before
   pushing T1.
4. Implement T1 exactly as the signatures above; any deviation is recorded in
   the PR description with the reason.
5. Write the failing test first; run `tools/scripts/confirm_failure.sh` for at
   least the `rejected_order` and `expired` rules.
6. Register the suite in `test/cmake/core_runtime_canvas_signal_tests.cmake`;
   run `ctest --test-dir build -R external-input` and `pulp test`.
7. Run `tools/scripts/gates.sh origin/main`, `tools/check-docs.sh`,
   `python3 tools/scripts/docs_noise_lint.py --mode report`, and the
   diff-coverage check before `shipyard pr --base main --no-arm`.
8. Each PR description lists: files, tests, fixtures, receipts produced, and
   the "not proven" line.
9. Do not implement T7–T10 without a named product; do not touch `NOTICE.md`
   or `DEPENDENCIES.md`.

## Decisions needed

1. **Placement of this document**: keep `docs/plans/` (new public category,
   currently on HOLD) or move to `planning/real-world-inputs-mvp.md` and
   document only shipped behaviour under `docs/`. The content is identical
   either way.
2. **PR 11350**: fold into T1 (rebase to `ExternalInputFrame<1>` + gate,
   drop the NOTICE section and the second plan page) or close it.
3. **Replay utility placement**: `core/runtime` (installed SDK can run app
   fixtures) versus `test/support` (smaller core, recipes copy it).
4. **First named product** for T9 (vision adapter) and for the first host
   demo (T7 OSC sharing app is recommended; it needs no new code).

## Explicit non-goals

The MVP does not add a universal sensor graph, camera service, inference
runtime, landmark schema, permission manager, host integration SDK, network
bus, UI widget, CLI command, or demo suite to Pulp core. It does not make a
plugin assume camera access; companion applications and existing host
MIDI/automation remain valid paths.
