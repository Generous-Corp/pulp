---
name: timeline
description: Pulp immutable timeline model and automation curves, typed command transactions, bounded journal and undo, durable assets, schema registry, and exact JSON persistence.
---

# Timeline document model

Use this skill when changing `core/timeline` document types or their construction
invariants.

## Contracts

- `Project`, `Sequence`, `Track`, and `Clip` are immutable snapshots. Validate
  once in their `create()` factories; reads must not repair, sort, or rebuild
  indexes.
- `Project` owns its sample-rate-independent `TempoMap` and `MeterMap`. Every
  path-copy rebuild, ID remap, journal comparison, and replay must preserve
  both maps.
- Every owned identity excludes zero and `UINT64_MAX`; the latter is the
  allocator's explicit exhausted sentinel. A project may store that sentinel
  after owning `UINT64_MAX - 1`; otherwise `next_item_id` is strictly larger
  than every owned ID. Allocation is monotonic and IDs are never reused.
- Tracks are sparse non-overlapping lanes. Their canonical clip order is
  `(anchor, start, ItemId)`. Timeline and ID indexes are persistent AVL trees;
  `replace_clip()` path-copies only search paths and shares untouched subtrees.
- A Track owns an ordered `DevicePlacement` chain. Placements contain only a
  durable `ItemId`; chain order is semantic, and clip edits retain the exact
  immutable chain storage. Runtime instances, graph nodes, plugin formats,
  paths, and platform metadata do not belong in Timeline. Durable device
  definition and configuration will be future document-owned state keyed by
  placement identity.
- `ClipTimeAnchor::Musical` follows tempo in ticks. `Absolute` uses
  `SamplePosition`, an integer sample count, and a normalized `RationalRate`,
  remaining fixed as tempo changes. Phase 1 rejects mixed anchors within one
  Track until a context-owned tempo/rate projection can compare them safely.
- `NoteContent` is a flat POD array sorted by `(start, ItemId)`. Note durations
  are positive, pitch is MIDI 0-127, and channel is 0-15.
- `AutomationCurve` is a position-ordered immutable point sequence. Point IDs
  and positions are unique within a curve; values are finite; curvature is in
  `[-1, 1]`. Continuous segments use a monotonic quadratic blend, while Hold
  segments retain the left value until the next point. `value_at()` is for
  control-thread or compile-time queries, never the audio-thread scheduler.
- `AutomationLane` is an immutable value that binds one curve to a
  format-neutral device-placement `ItemId` and opaque 32-bit parameter ID. Its
  standalone factory validates only the value identities. Once attached, Track
  owns lanes in canonical identity order, requires their target placements to
  exist in its device chain, and permits only one lane per placement/parameter
  pair. Lane and point IDs are Project identities owned by that Track; host
  delivery remains a separate contract.
- Keep automation responsibilities separated: curve data belongs in
  `automation_curve.*`, logical target binding belongs in `automation_lane.*`,
  RT cursor/coalescing belongs in `core/playback`, and graph delivery belongs in
  `core/host`.
- Hosted playback resolves each attached automation target through its
  `DevicePlacement` route and claims exact parameter ingress only for plugin
  nodes targeted by lanes. A plugin node cannot represent two automated
  placements or be claimed by competing bindings, and graph automation,
  audio-rate modulation, and live parameter injection cannot share that ingress
  while the claim is active. Delivery is generation-local through the binding's
  pinned `SignalGraph::ExecutionSnapshot`; route metadata is cached at prepare
  so later program adoption never queries a live plugin. Portable playback keeps
  ramp duration intact, but the current hosted `PluginSlot` boundary is
  point/step-only, so a ramp is lowered to an endpoint step within the current
  block. If exact-delivery cleanup or rollback fails, the binding is poisoned
  and fails closed until a fresh prepare establishes a new claim and snapshot.
  These are `core/host` ownership rules, not Timeline document invariants.
- `MediaRef` ranges are checked locally for overflow and against their asset at
  project construction.
- A media asset's SHA-256 `ContentHash` is its durable identity. Locators are
  optional late-resolution hints; representations have distinct hashes and
  unique roles. Missing local media is valid document state.
- Persistence uses canonical JSON envelopes with `data`, `type_name`, and
  integer `version`. All 64-bit IDs, positions, counts, durations, and rate
  components are canonical decimal JSON strings; never encode them as JSON
  numbers or floating point.
- Schema-v1 project persistence writes canonical `tempo_map` and `meter_map`
  arrays. BPM is stored by exact IEEE-754 bits; older v1 snapshots without map
  fields remain readable as 120 BPM and 4/4, then canonicalize on save.
- Track schema v2 introduced the required device-chain field; v3 adds required
  attached automation lanes. Adjacent downgrades succeed only when the field
  being removed is empty, so neither placement nor automation identity can be
  discarded. Placements, lanes, and lane targets remain separately versioned
  structural envelopes.
- Build a `SchemaRegistry` explicitly with `SchemaRegistryBuilder`; there is no
  global mutable registry. Registered content codecs are typed, `noexcept`, and
  own no hidden `ItemId`s in Phase 1. Migration callbacks must return and verify
  each complete intermediate envelope.
- Unknown or future extension content is retained as exact validated envelope
  bytes. Saving may splice those bytes unchanged and reports
  `has_opaque_objects`; ID remapping must fail closed for any opaque subtree.
- Versioned persistence fixtures live under `test/fixtures/timeline/vN/` and
  remain permanent compatibility inputs. Exercise unknown envelopes from those
  files instead of rebuilding equivalent JSON inside a test so whitespace,
  escapes, and member order cover the exact-byte re-save contract.
- Decode through `DecodeLimits`. Keep input size, depth, value/member/array and
  domain object limits enforced before growth. Duplicate object keys, malformed
  UTF-8/surrogates, noncanonical wide integers, and non-normalized rates fail.
- `serialize_project()` and `deserialize_project()` do not implement a ZIP or
  package container. Asset locators describe possible package-relative bytes,
  but container I/O belongs to a later slice.
- Project and subtree remapping are two-pass: allocate all owned IDs first, then
  rebuild the snapshot and fix references. `MediaRef::asset_id` is external to
  Clip/Track/Sequence remaps and is translated by `ExternalIdFixup`; failure is
  atomic and does not advance the caller's allocator. Preflight the complete
  owned subtree for duplicate IDs before allocating; this includes parent IDs,
  cross-track collisions, clips, note events, automation lanes, and automation
  points. Lane and point IDs remap as owned identities, target placement IDs
  remap as internal references, and opaque parameter IDs remain unchanged.
- Fallible public APIs return `pulp::runtime::Result`; do not throw.

## Editing contracts

- `InsertClip`, `RemoveClip`, `InsertAutomationLane`, `RemoveAutomationLane`,
  `MoveClip`, `SetNoteVelocity`, `SetClipPlaybackProperties`, `SetTempoMap`, and
  `SetMeterMap` are the bounded mutation vocabulary. Automation commands attach
  or tombstone complete Track-owned lanes; map commands carry exact
  expected/replacement document values and participate in the same transaction,
  journal, undo, and replay machinery. `reduce_transaction()` is pure: it
  returns a new snapshot, exact canonical dirty set, and reverse-ordered inverse
  commands.
- `DocumentSession` is the sole authoritative writer. Multiple control-thread
  callers serialize through it; readers atomically pin immutable snapshots.
  Every transaction declares its expected revision. Stale revisions and typed
  value/owner preconditions reject the whole transaction without publication.
- Command and transaction IDs are writer-scoped monotonic idempotency keys.
  `UndoGroupId` is the separate, explicit gesture-coalescing identity; different
  writers never coalesce.
- `WriterToken` is a move-only, session-bound capability. Its ID allocators are
  thread-safe and saturate permanently at exhaustion; never copy or synthesize
  writer-local ID streams.
- Gesture phases form one serialized session state machine: `Begin` opens a
  writer/group, only matching `Update`/`End` may follow, and other writers plus
  undo/redo receive `GestureState` until the group closes.
- Project identity lookup is a persistent AVL directory. Deletion tombstones
  IDs forever; inverse insertion may reactivate the exact identity and parent
  chain. Never scan the whole project or reuse an ID to implement undo.
- The in-memory command journal is bounded and fail-closed. A full journal
  rejects before publication; it never ring-evicts committed entries.
  `checkpoint()` truncates only a caller-confirmed durable prefix.
- Journal mutation and tombstone restoration are session-internal. Public
  `reduce_transaction()` never revives tombstones, and replay rejects a
  checkpoint snapshot/revision mismatch or cross-entry writer-ID reuse.
- Undo and redo submit fresh ordinary transactions. They append to the journal;
  they do not delete or rewrite history.

## Scope boundary

This subsystem does not own a durable `JournalSink`, package/container I/O,
publication, playback or automation delivery, launch
slots, takes, nesting, device implementations, routing, audio, format adapters,
or UI. Add those in their owning modules instead of widening the command and
persistence core opportunistically.
Plugin delay compensation and latency-to-output queries are likewise
`core/host` responsibilities. Timeline may retain durable device placement
identity, but it must not query live plugin latency or duplicate the prepared
graph's generation-local timing metadata.

## Validation

Build and run `pulp-test-timeline-model`, the automation curve and lane suites,
the commands, transactions, journal, and undo suites, plus
`pulp-test-timeline-schema-registry` and `pulp-test-timeline-persistence` in
Release and UBSan configurations.
Keep the 10k-clip edit test proving bounded node creation, subtree sharing, and
reclamation; a vector rebuild is not an acceptable persistent-index substitute.
Keep `pulp-test-timeline-replay-golden` green: it applies real journaled gain,
fade, and note edits, replays from the checkpoint, and compares the resulting
audio/MIDI byte stream with both the committed snapshot and pinned fixture.
Also verify installed-header consumption, `-fno-exceptions -fno-rtti`, and that
timeline translation units do not include or link `pulp::format`, `pulp::host`,
or `pulp::view`.
