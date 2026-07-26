---
name: timeline
description: Build, edit, validate, explain, render, import, or integrate Pulp timeline projects through the CLI, MCP tools, or C++ SDK. Use for sequencers, arrangements, clips or notes, tempo and meter maps, automation, takes and comps, freeze, capture-to-timeline workflows, durable journals, project persistence, DAWproject import, Standard MIDI File import and export, and agent-driven timeline operations.
---

# Timeline document model

## Choose the surface

- Use MCP for agent-driven project inspection, command application, validation,
  explanation, and render. Its five operations are `pulp_timeline_project_open`,
  `pulp_timeline_command_apply`, `pulp_timeline_validate`,
  `pulp_timeline_explain`, and `pulp_timeline_render`.
- Use `pulp seq` and `pulp render` for shell scripts, CI, and human-operated
  headless workflows. Prefer `seq apply` with typed command envelopes over
  inventing one-off mutation flags.
- Use the C++ SDK when embedding an editor, transport, compiler, renderer,
  recorder, or durable session. Keep document mutation in `DocumentSession`,
  playback derivation in `PlaybackProgramCompiler`, realtime rendering behind
  immutable programs and transport snapshots, and capture publication as
  ordinary timeline commands.
- Link the optional `Pulp::dawproject-import` SDK target only when ingesting
  DAWproject XML, and `Pulp::smf-interop` only when reading or writing Standard
  MIDI Files; keep the dependency-minimal model on `Pulp::timeline`.
- Use the generated schema surfaces to discover command/document shapes; do
  not hand-copy schema vocabularies into a client.

Start with `project_open` or `seq validate` when the source is unfamiliar.
Apply edits as one expected-revision transaction, validate the result, use
`explain` to inspect playback lowering/PDC, then render only when an audio
artifact is needed. Never modify canonical project JSON text directly.

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
- `ItemLocation` ownership is exactly `(kind, parent_id)`, where `parent_id` is
  the immediate owner. `sequence_id`, `track_id`, and `clip_id` are retained
  ancestor-navigation caches and must not become additional ownership keys.
  Add new item kinds by extending `immediate_parent_id()` rather than adding a
  new owner-specific field. Legacy identity records without `parent_id` derive
  it from their validated navigation fields during decode.
- Tracks are sparse non-overlapping lanes. Their canonical clip order is
  `(anchor, start, ItemId)`. Timeline and ID indexes are persistent AVL trees;
  `replace_clip()` path-copies only search paths and shares untouched subtrees.
- Initial `Track::create`, `Project::create`, and identity restoration must
  validate and sort complete input sets, then bulk-build balanced persistent
  AVL indexes with exactly one node per final entry. Do not feed bulk state
  through the per-edit insertion path: its transient path copies turn initial
  construction into allocator-heavy O(n log n) work. Ordinary edits still use
  path-copy insertion/replacement so prior snapshots share untouched subtrees.
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
- A `Sequence` owns two annotation lists: `SequenceMarker` (a named point) and
  `SequenceRegion` (a named span). They are **sequence**-owned, not
  project-owned: a `Project` holds many sequences, so a project-level list could
  not say which timeline it annotates. Both are canonical-ticks values, carry an
  optional packed `0xRRGGBBAA` colour (a float colour type would live outside
  this module's dependency floor, and exact bytes are what a document model
  needs), share one identity space (a marker may not reuse a region's `ItemId` in
  the same sequence), and are stored sorted — markers by `(position, id)`, regions by
  `(position, duration, id)`. **Regions may overlap and nest by design**: named
  sections contain sub-sections, so disjointness is deliberately not an
  invariant. What `Sequence::create` does enforce is a positive region length, a
  non-negative position, and containment inside the sequence's musical duration
  when it declares one — an absolute-only sequence bounds nothing above.
- **`Sequence` is built through `create()`, not aggregate init.** Existing
  partial overloads stay source-compatible; the full-fidelity path takes
  `SequenceInput`, so new owned collections extend a named input rather than an
  unbounded chain of positional overloads. `Sequence` remains pimpl'd behind
  `shared_ptr<const Data>`.
- **The `pulp.timeline.sequence` and `pulp.timeline.project` schemas are both
  versioned; the encoder must not hard-code either version.**
  `sequence_schema_policy` and `project_schema_policy` (mirroring
  `track_schema_policy`) own the type name, current version, oldest readable
  version, and the predicates — `requires_annotations(version)`,
  `requires_scenes(version)`, and `supports_session_start(version)` — that
  decode and preflight both consult. A
  literal version in `write_sequence`, `walk_sequence`, `walk_project`, or
  `structural_registry_validation` is how these drift apart — route every one
  through the policy. `pulp.timeline.project` being versioned at all is easy to
  miss: it sat at v1 long enough that several call sites reached for the
  fixed-version `data_for()` helper instead of `data_for_versions()`.
- **A required member and an optional one need different migration shapes.**
  Sequence markers/regions are required arrays, so v1→v2 splices `[]` in at the
  canonical position and v2→v1 refuses when either is non-empty. The project's
  `session_start` is optional, so its migration pair only moves the version
  number — nothing is inserted — and the downgrade refuses only when the member
  is actually present. Copy the shape that matches the field, not the nearest
  migration.
- **`Project::Data` is brace-initialized positionally in `append_asset` and
  `remove_asset`.** Appending a field to the struct there fails *open*: those
  sites keep compiling and silently leave the new field default, dropping
  document state on any asset edit. Those two sites now use designated
  initializers; keep them that way, and put any new field where a stale
  positional init cannot type-check.
- Markers and regions are command-addressable: `InsertMarker` / `RemoveMarker` /
  `InsertRegion` / `RemoveRegion` reduce through
  `transaction_marker_internal`, which plans an `ItemKind::Marker` or
  `ItemKind::Region` identity parented by the **sequence** (not a track), so
  `DirtyItem::owner_track` is legitimately zero for these commands. They emit
  inverse commands, so undo, redo, and journal replay restore the annotation and
  its tombstone ownership exactly.
- Scenes are sequence-owned in authored order, and each scene owns its ordered
  slots. A non-empty slot must name a clip in that sequence; a Jump follow
  action must name an existing slot, while other kinds carry no target.
  `InsertScene` / `RemoveScene` and `InsertSlot` / `RemoveSlot` reduce through
  `transaction_scene_internal` with exact inverse and tombstone ownership.
- Automation lanes are command-addressable: `InsertAutomationLane` /
  `RemoveAutomationLane` reduce through the shared transaction pipeline
  (`transaction_reduction_support` + `transaction_automation_internal`),
  validating the target sequence/track/placement, allocating lane and point
  identities as owned Project identities, and emitting inverse commands so undo,
  redo, and journal replay restore the lane and its tombstone ownership exactly.
  Command equivalence compares lane payloads bit-exactly (float bits), so a
  re-authored point differing only in a signed zero is not treated as equal.
- Keep automation responsibilities separated: curve data belongs in
  `automation_curve.*`, logical target binding belongs in `automation_lane.*`,
  RT cursor/coalescing belongs in `core/playback`, and graph delivery belongs in
  `core/host`.
- A `Take` is one recorded region referencing a sealed media asset, anchored to
  absolute sample time (its timeline length is the media frame count). A
  `TakeLane` is an immutable, identity-ordered set of takes owned by a Track,
  and a Track carries a `record_armed` document-intent flag the capture engine
  reads but never mutates here. A take's parent is its lane — the second
  lane-owned exception alongside `AutomationPoint`, so `immediate_parent_id()`
  returns the supplied `lane_id` for `ItemKind::Take` and it is excluded from
  coordinate-based parent recompute; a lane's parent is its Track. Take
  identities must be disjoint from every other track-owned id, and a take whose
  `MediaRef` asset is missing or out of range is rejected at `Project::create`,
  exactly like a clip `MediaRef`.
- Take lanes and record-arm are command-addressable: `InsertTakeLane` /
  `RemoveTakeLane` / `SetRecordArm` reduce through the shared pipeline
  (`transaction_reduction_support` + `transaction_take_internal`, dispatched like
  automation), allocating lane+take identities as owned Project identities and
  emitting inverse commands so undo/redo/journal-replay restore lane and
  tombstone ownership exactly. `InsertTakeLane` re-validates each take's asset
  reference against the project (the recorder emits `CreateAsset` first, so the
  asset already exists when the take command reduces); `SetRecordArm` is an
  optimistic expected/replacement gate on a non-identity flag. `InsertTake` /
  `RemoveTake` edit one lane-owned take and preserve its tombstone parent.
  `SetActiveTakeLane` optimistically selects one existing lane as the active
  playlist/comp; zero selects the arrangement. Removing an active lane is
  rejected, so clearing selection and removing the lane must be one explicit
  transaction. `SetTakeComp` carries exact expected/replacement segment lists.
  Each segment names a take and an in-bounds, normalized absolute sample range;
  lane construction canonicalizes by timeline start and rejects missing takes,
  mixed/wrong rates, empty or overlapping ranges. Removing a take selected by
  the comp fails closed. Playback may flatten this source data into a derived
  cache, but the cache is never document truth.
- A `TrackFreeze` is an optional immutable selection of one sealed media
  artifact plus its absolute placement/rate and a `ContentHash` fingerprint of
  the exact render plan. It does not remove or mutate authored clips, takes,
  automation, or device placements. Publish with one transaction ordered as
  `CreateAsset` then `SetTrackFreeze`; the latter is an exact
  expected/replacement gate dispatched through
  `transaction_track_state_internal`. Undo clears the selection before
  removing the artifact, asset removal fails while selected, and replay selects
  the sealed artifact without re-rendering. Construction and mutation validate
  that the asset exists, the media range is in bounds, and the artifact rate
  matches. ID remap fixes the external artifact reference through
  `ExternalIdFixup`; the render-plan hash is content metadata, not an owned
  identity.
- `Project::Data` and `Track::Data` mutations rebuild by copy-and-modify
  (`auto next = *data_; next.field = ...; make_shared<const Data>(move(next))`),
  never positional brace-init — adding a field must not silently shift an
  unrelated mutation site. `create()` factories build from input with designated
  initializers.
- `MediaRef` ranges are checked locally for overflow and against their asset at
  project construction.
- A media asset's SHA-256 `ContentHash` is its durable identity. Locators are
  optional late-resolution hints; representations have distinct hashes and
  unique roles. Missing local media is valid document state.
- `AudioLoopInfo` is optional typed metadata on a sealed media asset: musical
  length and meter, one-shot intent, MIDI root note, half-open frame markers,
  manual or analyzer-suggested loop points, and canonical tags. Derive tempo
  from musical length, asset frame count, and sample rate; do not persist a
  duplicate BPM value that can disagree. This is descriptive authoring state,
  not clip repetition, sample traversal, or a comp-selection model.
- Persistence uses canonical JSON envelopes with `data`, `type_name`, and
  integer `version`. All 64-bit IDs, positions, counts, durations, and rate
  components are canonical decimal JSON strings; never encode them as JSON
  numbers or floating point.
- Schema-v1 project persistence writes canonical `tempo_map` and `meter_map`
  arrays. BPM is stored by exact IEEE-754 bits; older v1 snapshots without map
  fields remain readable as 120 BPM and 4/4, then canonicalize on save.
- Track schema v2 introduced the required device-chain field; v3 adds required
  attached automation lanes; v4 adds the required `take_lanes` array and
  `record_armed` flag; v5 adds the required `active_take_lane_id` (`"0"` means
  arrangement). Adjacent downgrades succeed only when the field being removed
  is empty/default (v5→v4 requires zero selection; v4→v3 requires empty takes
  and `record_armed` false), so
  neither placement, automation, nor take identity can be discarded. Placements,
  lanes, lane targets, take lanes, and takes remain separately versioned
  structural envelopes. Take-lane schema v2 adds required `comp_segments`; its
  v1 upgrade adds an empty comp, while v2→v1 succeeds only for an empty comp.
  Track schema v6 adds optional `freeze`; v5→v6 is version-only because absence
  means unfrozen, while v6→v5 succeeds only when `freeze` is absent. Never
  silently discard a selected artifact during downgrade.
- Asset schema v2 adds optional `loop_info`. Its v2 to v1 downgrade succeeds
  only when that field is absent, and a v1 envelope that illegally contains
  the field is rejected rather than silently normalized.
- `schema_release.hpp` records the exact structural type/version sets first
  shipped in `v0.736.0` (Track v1), `v0.744.0` (Track v2), and `v0.748.0`
  (Track v3). `serialize_project_for_release()` applies the registry's
  downgrade callbacks parent-first, then rewrites reachable child envelopes.
  It fails closed when a removed feature is populated, an encountered
  extension has no explicit target, or the target map names a type/version the
  supplied registry cannot provide. Do not infer release compatibility from
  the current schema version or preserve an opaque extension in an older
  release export.
- Release export also projects the project identity table. Identity kinds
  unknown to the target release fail when active; inactive tombstones of those
  kinds are removed only after confirming their IDs remain below
  `next_item_id`, which preserves the no-reuse boundary an older reader will
  carry forward.
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
- Use `peek_project_summary()` for project browsers and admission checks that
  need identity, name, root, or structural counts without constructing the
  immutable document. Pass the same `SchemaRegistry` intended for load so
  registered content is distinguished from opaque content. The peek still
  scans the complete structural envelope and enforces every `DecodeLimits`
  quota, including take lanes and takes, but does not resolve item or media
  references.
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

### Widening `ClipContent` is guarded, and the two guards are not interchangeable

`ClipContent` decides what a clip *is*, so nearly every consumer dispatches on
it — and the default failure of a new alternative is silence, not an error. A
consumer written as an `if`/`if constexpr` chain keeps compiling and treats the
new kind as absent: a clip that renders nothing, an export manifest that reports
no loss while dropping data, a remap that carries stale ItemIds into a copy. No
test catches that, because nothing wrote a test for a kind that did not exist.

So the variant carries two guards, and which one a site gets is a judgement, not
a style preference:

- **Visit through `ClipContentCases`** when the site is genuinely dispatching —
  the encoder, content equality, the journal's retained-size accounting, the
  interchange census, the id-remap walk, and the playback compiler's content
  classifier. The overload set has no generic fallback, so widening the variant
  fails overload resolution right there.
- **`static_assert(kClipContentAlternativeCount == N)`** when the site *cannot*
  be a visit but is only correct for today's alternatives — the decoder, which
  dispatches on envelope type names rather than on the variant, and the two
  asset referential-integrity scans, which reach assets through `MediaRef`
  alone. Each message names the decision that site owes.

A site that reads one alternative and is correct for every other one (a note
lookup, a `MediaRef` range check at construction) needs neither; do not add
noise there. And note that `-fno-exceptions` makes a bare `std::get` on a
mismatched alternative call `std::terminate` rather than throw, so a fallthrough
`std::get` is a process abort, not a caught error — that is why the encoder is a
visit and not a chain ending in `std::get<OpaqueContent>`.

## Editing contracts

- `InsertClip`, `RemoveClip`, `InsertAutomationLane`, `RemoveAutomationLane`,
  `MoveClip`, `SetNoteVelocity`, `SetClipPlaybackProperties`, `SetTempoMap`,
  `SetMeterMap`, `CreateAsset`, `RemoveAsset`, `InsertTakeLane`,
  `RemoveTakeLane`, `InsertTake`, `RemoveTake`, `SetRecordArm`,
  `SetActiveTakeLane`, `SetTakeComp`, `SetTrackFreeze`, `InsertMarker`,
  `RemoveMarker`, `InsertRegion`, `RemoveRegion`, and `SetChordScaleLane` are the
  bounded mutation
  vocabulary. Automation commands attach or tombstone complete Track-owned
  lanes; map commands carry exact expected/replacement document values and
  participate in the same transaction, journal, undo, and replay machinery.
  `reduce_transaction()` is pure: it returns a new snapshot, exact canonical
  dirty set, and reverse-ordered inverse commands.
- `CreateAsset`/`RemoveAsset` are the asset-table mirror pair (shaped like
  `InsertClip`/`RemoveClip`). `CreateAsset` carries the whole `MediaAsset` by
  value — the `ContentHash` is the sealed durable identity. The reducer plans
  the `ItemKind::Asset` identity and appends the asset by reference to that hash
  through `Project::append_asset`; **replay never re-captures or re-hashes media
  bytes**, so the same checkpoint plus journal reproduce a byte-identical asset
  table (the sealed-content contract). An invalid/empty `ContentHash` is
  rejected before the asset can enter the document. `RemoveAsset` fails closed
  while any clip's `MediaRef` still points at the asset, tombstones the identity
  like a clip removal, and its inverse re-creates the sealed asset — so undo/redo
  stays whole. Asset validation and identity-mutation application live in shared
  `model.cpp` helpers so construction, sequence replacement, and asset mutation
  enforce one sealed-identity and one identity-transition path.
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
- A session may attach a `JournalSink`. It publishes a transaction only after
  `append_batch()` reports the complete batch durable, and truncates a
  checkpoint only after the sink installs its reconstructed snapshot. A sink
  error is ambiguous, so it permanently poisons new durable writes for that
  session; already-cached idempotent results retain their normal semantics.
  Both callbacks run under the session writer lock and must not call
  lock-taking APIs on the originating `DocumentSession`.
- `FileJournal` is the native crash-consistent sink. Its frames contain
  canonical snapshots, a revision, and checksums; append success follows the
  platform durability fence. Recovery truncates only a torn final frame and
  fails closed on corruption earlier in the file. Checkpointing the current
  revision uses a synced temporary sibling plus atomic rename, while
  checkpointing an older prefix leaves newer durable frames intact. Restored
  sessions attach only after an exact, read-only canonical-bytes/revision
  validation; restoration never rewrites or truncates the recovered journal.
  Symlink paths canonicalize to one lock identity; multiply linked journal
  files are rejected because atomic checkpoint replacement cannot preserve
  hard-link identity.
- Journal mutation and tombstone restoration are session-internal. Public
  `reduce_transaction()` never revives tombstones, and replay rejects a
  checkpoint snapshot/revision mismatch or cross-entry writer-ID reuse.
- Undo and redo submit fresh ordinary transactions. They append to the journal;
  they do not delete or rewrite history.

### Sequence-owned context and the compile-context subscription contract

A `Sequence` owns a `ChordScaleLane` — an ordered set of `ChordScaleEvent`s
(position, chord root + quality, scale root + mode) that *other* items read while
compiling. That cross-entity read is what the compile-context subscription
contract exists for, and the contract is what keeps the compiler's dirty set
exact:

- `CompileContextKind` (`compile_context.hpp`) is the vocabulary; a renderer
  declares the kinds it reads as a `CompileContextSubscriptions` set.
- The **read** side is `CompileContextView`, constructed with that declaration.
  An undeclared kind reads as null. Do not add an accessor that bypasses the
  declaration — the whole point is that a hook cannot depend on context the
  compiler does not know to invalidate it for.
- The **invalidate** side lives in `core/playback` (see the playback skill):
  `ContextSubscriberIndex` is the kind → reader-track reverse index, and
  `resolve_dirty_tracks()` turns a `DirtySet` into an exact `DirtyTrackSet`.
- A context edit emits **two** things: a `DirtyContext{sequence, kind}` (which
  names what changed) and a companion `DirtyItem` flagged `DirtyFlags::Context`
  with no owning track (so an item-scanning consumer still sees the transaction
  changed something rather than silently seeing an empty dirty set). A
  trackless item that is *not* Context-flagged means a structural sequence edit.

Adding a context kind is a data change here plus a reverse-index case in the
compiler. It is never a reason to widen an invalidation.

### Adding a field to `pulp.timeline.sequence` touches two silent mirrors

Beyond the documented recipe (model → registry → version + both migrations →
encode/decode → regenerate codegen → web source closure → tests), two files
mirror the registry with no reference pointing at them from it, and both fail
with an error that does not name them:

- `structural_registry_validation.cpp` carries an **exact** expected field list,
  order, kind, required-ness, and version range per structural type. Miss it and
  `serialize_project()` returns a bare `InvalidSchema` before writing a byte.
- `schema_json_preflight.cpp`'s `walk_*` functions re-validate the envelope
  independently of the decoder, pinning `[oldest_readable, current]` versions per
  type. Miss it and every v2 document fails to load even though the decoder
  handles it. New optional/versioned array fields also need their own
  `governed_array` count so a hostile document cannot allocate unbounded.

The paired version policy header (`sequence_schema_policy.hpp`, alongside
`track_schema_policy.hpp` and `asset_schema_policy.hpp`) is what keeps those
three call sites agreeing about which version introduced which field. Add one
rather than spelling version numbers inline.

### `GrooveTemplate` composes two transforms that behave differently

A groove is a `timebase` swing warp plus a repeating offset table. The warp is
continuous and monotone (its contract and its round-trip bound live in the
timebase skill); the table is a per-step displacement and **can** reorder
material where adjacent steps lean opposite ways. Offsets are bounded to less
than a step, which limits that but does not forbid it — so assert monotonicity
of the swing half only, never of the composition.

The table is indexed by the **authored** position, not the swung one, so
changing the swing setting never re-assigns notes to different steps. A test for
that has to pick a position swing carries across a step boundary; most positions
land in the same step under either order and a test using one of those passes
whichever way the code is written.

### Downgrade refusals: refuse on *authored* data, not just audible data

The recipe says a downgrade must refuse rather than lie. The narrower trap is
deciding what "would lose data" means. `migrate_sequence_v2_to_v1` refuses on a
non-empty chord lane, which is the whole of that type. A struct-shaped field
(the groove) has several members that a downgrade would drop, and only some of
them change what the document sounds like — a strength setting is inert while
the feel it attenuates is absent, and a name is never audible at all. Refuse on
**every** departure from the default value anyway. They are still data the user
authored, and a downgrade that silently discards them is the same failure as one
that silently retimes the music, just quieter.

Check the members field by field rather than comparing the object's bytes
against the canonical default: a hand-written document may carry them in any
order and still say exactly the same thing. Write one refusal test per member —
a single "a populated groove refuses" case passes even when the predicate only
looks at one field, which is how a weakened refusal gets through review.

### `Sequence` grows by overload, and each new owned field needs a version predicate

`Sequence` is pimpl'd behind `shared_ptr<const Data>` and built through
`create()` overloads, so a new owned field arrives as another overload whose
older siblings chain forward by constructing the field's default. No existing
call site changes and there is no positional brace-init hazard — do **not**
convert `Sequence` to a `SequenceInput` struct to add one.

What does need care is `sequence_schema_policy.hpp`: each owned field gets its
own `<field>_introduced_version` plus a `requires_<field>(version)` predicate,
because the decoder and the preflight both have to know which document versions
carry it and which must *not*. Both sides are checked — a field present in a
version that predates it is as much a rejection as one missing from a version
that requires it — so reusing another field's predicate silently accepts
malformed documents on one of the two paths.

### `Command` `retained_size()` must account for heap the alternative owns

`retained_size()` has a chain of `if constexpr` arms and a `sizeof(T)` fallback.
A new alternative that owns a vector or a string falls into the fallback and
reports only its inline size, so the journal's memory accounting silently
under-counts and the bound it enforces stops meaning what it says. This does not
fail to compile and no test notices unless one is written for it.

### Adding a `Command` alternative fails closed in one place and aborts in another

`Command` has **no** exhaustive-visitor guard of the `ClipContentCases` kind.
Two consumers behave differently:

- `command.cpp`'s `equivalent()` is an `if constexpr` chain ending in a generic
  `else` that reads `.track_id`/`.clip_id`. A new alternative without those
  fields is a **compile error** — it fails closed, which is what you want.
- `detail::reduce_transaction()` in `transaction.cpp` is an `if/else if` chain
  ending in `std::get<SetClipPlaybackProperties>`. Under `-fno-exceptions` that
  is `std::terminate`, not a caught error. Add the reduce branch **before** the
  final `else`; a compile-clean build proves nothing here.

Also extend the two coverage guards that pin the vocabulary: the type-name array
in `test_timeline_schema_registry.cpp` (`static_assert`ed against
`variant_size_v<Command>`) and the encoded-envelope batch in
`test_timeline_command_persistence.cpp` (which asserts one decoded command per
alternative, in variant order).

## Schema codegen & drift gate

The `SchemaRegistry` is the single generative source for the timeline's agent
surfaces (JS facade, TypeScript definitions, MCP tool definitions, CLI verbs):
they are generated from it, never hand-maintained, so they cannot drift.
`emit_schema_manifest()` (`schema_codegen.cpp`) projects the built-in registry
into one canonical JSON-Schema document — a lossless view of every type's
domain, current version, fields, required set, and migration edges — round-
tripped through `canonicalize_json` so the same registry always yields
byte-identical output regardless of registration order.

The committed artifact is `core/timeline/schema/timeline_schema.json`; the
`pulp-timeline-schema-emit` binary regenerates it. **After any change to
`register_builtin_timeline_schemas` (adding/removing a type, a field, a version,
or a migration edge), regenerate the artifact or the drift gate fails:**

```
python3 tools/scripts/schema_drift_check.py --update   # regenerate
python3 tools/scripts/schema_drift_check.py            # verify in sync
```

`schema_drift_check.py` is the standalone gate logic (regenerate → byte-diff →
nonzero on drift), wired as the `timeline-schema-drift` ctest; the
`timeline-schema-drift-selftest` ctest confirms it catches a stale artifact.
Wiring it as a standalone GitHub-workflow required check is separate and owned by
the CI layer.

Placement convention (repo-wide): a **subsystem-local generator binary** lives
under `core/<subsystem>/tools/` (here, `schema_emit_main.cpp`), while a
**repo-wide gate script** lives under `tools/scripts/` with the other checks
(`schema_drift_check.py`, alongside `timeline_engine_dependency_floor_check.py`).
Don't invent a per-subsystem `tools/` dir for a gate script.

`schema_drift_check.py` is generic — it takes `--artifact` and `--emit-cmd` and
byte-compares. A new generated artifact anywhere in the repo reuses it as-is
rather than growing its own gate; `core/interchange` registers three drift ctests
against it (vocabulary header, capability tables, docs page) from one emitter.

A module added under `core/` that should have an enforced dependency floor
registers itself in `MODULE_FLOORS` in
`tools/scripts/timeline_engine_dependency_floor_check.py`. The check's selftest
iterates `MODULE_FLOORS` generically, so a new entry gets include-scan and
link-scan coverage plus selftest proof without touching the selftest. Adding an
entry is not the same as widening `timeline`'s own floor — a module that sits
*above* timeline gets its own row and must not appear in timeline's set.

### Derived surfaces are projections of the manifest, not the registry

Every downstream agent surface is a **pure function of the committed
`timeline_schema.json`**, not a second reader of the registry. Each is guarded by
the same shared `schema_drift_check.py` (its own artifact, its own ctest), so the
chain is `registry → manifest → surface`: the JSON gate guards the first edge, a
per-surface gate guards the second. A surface generator never links the timeline
library — it consumes the JSON.

The **TypeScript-type surface** is the first such projection:
`core/timeline/tools/schema_ts_emit.py` reads the manifest and emits
`core/timeline/schema/timeline_types.d.ts` — one `export interface` per schema
type, plus a `TimelineSchemaTypeName` union and a `TimelineSchemaTypeMap`. Kinds
map by `x-pulp-kind` (`Boolean`→`boolean`; `U32`/`I64String`/`U64String`→
`number | string`, the union covering both the string wire form of the 64-bit
kinds and a numeric runtime value; `String`→`string`; `Object`→
`Record<string, unknown>`; `Array`→`readonly unknown[]`), and a field `$ref`
overrides its kind with the referenced interface. **After regenerating
`timeline_schema.json`, regenerate the `.d.ts` too or its gate fails:**

```
python3 core/timeline/tools/schema_ts_emit.py --out core/timeline/schema/timeline_types.d.ts
```

The `timeline-schema-ts-drift` ctest byte-checks the committed `.d.ts` against a
fresh emission; `timeline-schema-ts-selftest`
(`core/timeline/tools/test_schema_ts_emit.py`) proves determinism, complete
projection, kind mapping, and that the gate catches a mutated artifact. Note the
generator is Python (a pure JSON projection needs no build), so it sits beside
the C++ emitter under `core/timeline/tools/` but reuses the shared gate rather
than a bespoke drift script.

The **CLI-verb surface** is the same shape:
`core/timeline/tools/schema_cli_emit.py` reads the manifest and emits
`core/timeline/schema/timeline_cli_verbs.json` — one verb per schema type,
each with its domain, version, and a flag per field. The flag value type maps by
`x-pulp-kind` (`Boolean`→`bool`; `U32`/`U64String`→`uint`; `I64String`→`int`;
`String`→`string`; `Object`/`Array`→`json`), and a field `$ref` becomes a `json`
flag that records the referenced schema type. Verb tokens drop the `pulp.` prefix,
join the hierarchy with `:`, and kebab-case each segment
(`pulp.timeline.automation_target.device_parameter` →
`timeline:automation-target:device-parameter`). This artifact is the
manifest-derived *definition* of the verbs; wiring them into the `pulp` CLI binary
is a separate downstream integration. **After regenerating the manifest,
regenerate this too or its gate fails:**

```
python3 core/timeline/tools/schema_cli_emit.py --out core/timeline/schema/timeline_cli_verbs.json
```

The `timeline-schema-cli-drift` ctest byte-checks it; `timeline-schema-cli-selftest`
(`core/timeline/tools/test_schema_cli_emit.py`) proves determinism, complete
projection, value-type mapping, and confirm-the-failure.

The **JS-facade surface** is the runtime-JS counterpart to the `.d.ts`:
`core/timeline/tools/schema_js_emit.py` reads the manifest and emits
`core/timeline/schema/timeline_facade.js` — a frozen ES module exporting
`timelineSchema` (a descriptor per type: domain, version, and fields), a
`timelineSchemaTypeNames` list, and `timelineSchemaManifestVersion`. The JS
engine imports it directly (no JSON parse). Field `jsType` reports the actual
runtime type — unlike the `.d.ts`, which widens the numeric kinds to
`number | string`, the facade reports `U32`→`number` and the 64-bit string kinds
(`I64String`/`U64String`)→`string` (carried as strings to keep precision);
`Object`→`object`, `Array`→`array`, and a `$ref` field records the referenced
type. **After regenerating the manifest, regenerate this too or its gate fails:**

```
python3 core/timeline/tools/schema_js_emit.py --out core/timeline/schema/timeline_facade.js
```

The `timeline-schema-js-drift` ctest byte-checks it; `timeline-schema-js-selftest`
(`core/timeline/tools/test_schema_js_emit.py`) proves determinism, complete
projection, jsType mapping, confirm-the-failure, and — when `node` is present —
that the emitted module parses, imports, and is deeply frozen (skipped, not
failed, without `node`).

The **MCP tool-definition surface** is another manifest projection:
`core/timeline/tools/schema_mcp_emit.py` emits the fixed five engine operations
(project open, command apply, validate, explain, and render) into
`core/timeline/schema/timeline_mcp_tools.json`. The operation set is an API
decision rather than a copy of the schema CLI-verb table. Its type vocabularies
remain manifest-derived: project open lists every Document type, command apply
constrains its envelope to the Command types, and validate lists the Diagnostic
types. An empty Command domain emits the object-valued reject-all schema
`{"type":"string","not":{}}` for `type_name`, rejecting every command name until
the registry defines one. JSON Schema forbids an empty `enum`, and the released
MCP Tool wire contract requires property schemas to be objects rather than
boolean schemas; omitting the enum would accidentally accept an unbounded
string. The live MCP server consumes this generated artifact for both
advertisement and exact-five operation dispatch.

```
python3 core/timeline/tools/schema_mcp_emit.py \
    --out core/timeline/schema/timeline_mcp_tools.json
```

The `timeline-mcp-drift` ctest byte-checks the artifact;
`timeline-mcp-selftest` (`core/timeline/tools/test_schema_mcp_emit.py`) proves
determinism, exact operation membership, complete domain projection,
fail-closed empty command behavior, and confirm-the-failure.

### Headless operations and CLI

`pulp::tool-timeline` is the shared headless implementation for agent-facing
project operations. It loads either canonical inline JSON or a project path,
uses the built-in registry for persistence and command decoding, submits edits
through `DocumentSession`, compiles the root sequence through
`PlaybackProgramCompiler`, and renders arrangement audio through
`ArrangementAudioRenderer`.

External asset locators are URI hints, not filesystem paths by definition.
The headless render lane accepts bare local paths and canonical `file://` URIs,
percent-decodes local file URIs (including `file://localhost/...`), and skips
unsupported or non-local schemes before constructing a filesystem path. It
still verifies the resolved bytes against the asset's `ContentHash`.
`PackageRelative` locators remain a separate contained-path contract beneath
the canonical project directory; do not route them through the external-URI
resolver.

The installed CLI keeps this operational layer thin:

```
pulp seq schema
pulp seq validate <project.json>
pulp seq explain <project.json> [--sample-rate <hz>]
pulp seq apply <project.json> <commands.json> [--out <project.json>]
pulp render <project.json> --out <file.wav> [--sample-rate <hz>]
```

Do not add hand-authored mutation verbs to `cmd_seq.cpp`; `apply` consumes the
registry-derived typed command envelopes. `render` emits Float32 WAV and does
not silently instantiate hosted devices or invent plugin delay compensation.
The headless explain result reports unknown PDC offsets as JSON `null`.

The live MCP server embeds `timeline_mcp_tools.json` at configure time and
dispatches exactly its five operations through `pulp::tool-timeline`. Do not
copy their input schemas into `pulp_mcp.cpp`; regenerate the artifact from the
timeline manifest and let the server consume it. The MCP render result can be
fed to `pulp_audio_compare` for an advisory before/after judgment when the
opt-in Audio Quality Lab tool is installed.

## Scope boundary

This subsystem owns authored take/comp state, the durable `JournalSink`
ordering seam, and native `FileJournal`, but not package/container I/O,
publication, realtime playback or automation delivery, launch slots, nesting,
device implementations, routing, audio, format adapters, or UI. Add those in
their owning modules instead of widening the command and persistence core
opportunistically.

## Launch model and follow actions

`Slot`, `Scene`, and `FollowAction*` in `clip_launch.hpp` are durable authored
values owned by `Sequence`; both structural types are registered schemas and
their IDs live in the Project identity directory. Sequence schema v5 introduced
the required `scenes` array after v4 introduced groove. The v4→v5 migration
inserts an empty array, while v5→v4 refuses to discard a non-empty one.
Quantization and follow-action choices round-trip canonically, but sample-accurate
launch progress remains runtime state in `core/playback/clip_launch.*`.
`core/interchange` treats `clip.launch` as model-detectable and records each
authored scene in the canonical census, so export loss manifests cannot omit
launcher state silently.

A random follow action (`Any`, `Other`, or a weighted candidate draw) must stay
a **stateless hash of (session seed, slot id, draw index)**, never a stateful
RNG. Engine determinism requires that the same document plus journal plus
transport trace produce the same event stream, and a shared RNG breaks that in
a way no single-lane test catches: one lane's draw would depend on how many
other lanes happened to resolve earlier in the same block, so the result would
track evaluation order. Hashing the deciding slot's own id makes the draw
independent of every other lane. `FollowDraw::value()` is that hash; keep new
random behaviour derived from it rather than adding a second source of entropy.

A follow-action period is a bare `TickDuration`, not a `LaunchQuantize` — it is
measured from the launch, so there is no phase to author and a `LaunchQuantize`
there would carry a field nothing reads.

`core/timeline/PulpTimelineSources.cmake` is the canonical production source
inventory. Desktop targets consume its portable and native lists; WAM and
WebCLAP consume only the portable list. Add a new translation unit to that
manifest instead of duplicating source lists in platform CMake, and keep
`web_timeline_source_closure_check.py` green so an unclassified file cannot
silently disappear from a runtime.

## Validation

Build and run `pulp-test-timeline-model`, the automation curve and lane suites,
the commands, transactions, journal, and undo suites, plus
`pulp-test-timeline-schema-registry`, `pulp-test-timeline-schema-codegen`, and
`pulp-test-timeline-persistence` in Release and UBSan configurations.
Keep the 10k-clip edit test proving bounded node creation, subtree sharing, and
reclamation; a vector rebuild is not an acceptable persistent-index substitute.
Keep `pulp-test-timeline-replay-golden` green: it applies real journaled gain,
fade, and note edits, replays from the checkpoint, and compares the resulting
audio/MIDI byte stream with both the committed snapshot and pinned fixture.
Also verify installed-header consumption, `-fno-exceptions -fno-rtti`, and that
timeline translation units do not include or link `pulp::format`, `pulp::host`,
or `pulp::view`.

`test/cmake/sampler_runtime_tests.cmake` also registers sampler Heritage
runtime tests. That shared CMake inventory does not make profile JSON, capture
evidence, or sampler rendering part of the timeline document schema; keep those
contracts in `pulp::audio` unless a future version explicitly adds a document
reference type.

### Writing a render-continuity assertion

A "gap-free during playback" test must distinguish two things that both look
like silence at the head of the stream:

- **PDC compensation delay is not a dropout.** A latency-reporting node in the
  graph legitimately zeroes the first `latency_samples()` samples. Start the
  continuity scan *after* that window, and say so in the test, or the assertion
  reports a gap on every healthy run.
- **A continuity scan alone can pass vacuously.** If the edit under test never
  actually took effect, an unbroken stream proves nothing. Assert the rendered
  level changes across the edit as well, so the test fails both when the stream
  gaps and when the edit was silently dropped.

`PlaybackProgramStore::read()` returns a non-copyable, non-assignable
`ReadGuard`. A render loop cannot reassign one guard as it swaps programs —
hold a separate guard per program and drive the blocks through a helper, which
also keeps the transport position continuous across the swap.

### Widening `AutomationTarget` is guarded, and the guard is load-bearing

`AutomationTarget` is a `std::variant`, and `core/timeline` compiles
`-fno-exceptions`. That combination makes `std::get<Alternative>` on a
mismatched target call `std::terminate` rather than throw — so a consumer that
reads the target with `std::get` is a latent process abort the moment the
variant grows. Three encoder/transaction/document sites do exactly that, and
they carry a `static_assert` on `kAutomationTargetAlternativeCount` for it.

The opposite mistake is quieter and worse. A consumer that visits with a generic
lambda (`[](const auto&)`, or an `if constexpr` chain with no `else`) keeps
compiling and silently ignores the new alternative. A target that exists in the
document but is missing from a census or an export loss manifest reads as
"nothing was there" — a manifest claiming no loss while dropping data is worse
than a refusal.

So visit through `AutomationTargetCases`, the overload set with no generic
fallback. Adding an alternative then fails the build at every call site until
someone decides what it means. When you do widen the variant, expect the
`static_assert`s to fire: that is the design, and each message names the
decision that site owes.

### `pulp_audio_compare` is advisory and opt-in

`handle_audio_compare` delegates to the Audio Quality Lab, a managed Python
tool that is deliberately **not** installed by default and is resolved relative
to a Pulp project root. A test must not assume a measured judgment comes back:
when the lab is absent the handler still returns a well-formed typed envelope
carrying its install hint. Assert the typed envelope and the absence of an
argument refusal — that is what proves the loop reached the compare stage — and
leave the measurement to the tool's own suite.

## Foreign-format import (interop)

Read [references/dawproject-import.md](references/dawproject-import.md) before
changing the DAWproject importer or adding another foreign-format importer. It
defines the native/web source boundary, parser constraints, fail-closed subset,
sealed media identity, and pre-growth resource limits.

Read [references/smf-interop.md](references/smf-interop.md) before changing
Standard MIDI File import/export. It covers why the conversion stays in the
musical domain rather than resolving to seconds, which header divisions convert
exactly against `kTicksPerQuarter`, why the vendored `choc::midi::File` reader
cannot back this path, and the byte-stream traps (running-status cancellation,
system-exclusive framing, FIFO note matching) a hand-rolled decoder must get
right.
