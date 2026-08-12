---
name: timeline
description: Build, edit, validate, explain, render, import, or integrate Pulp timeline projects through the CLI, MCP tools, or C++ SDK. Use for sequencers, arrangements, clips or notes, tempo and meter maps, automation, takes and comps, freeze, capture-to-timeline workflows, durable journals, project persistence, DAWproject import, Standard MIDI File import and export, and agent-driven timeline operations.
---

# Timeline document model

## Choose the surface

- Use MCP for agent-driven project inspection, command application, diff,
  undo/redo, validation, explanation, render, export, and import. Its ten
  operations are
  `pulp_timeline_project_open`, `pulp_timeline_command_apply`,
  `pulp_timeline_diff`, `pulp_timeline_undo`, `pulp_timeline_redo`,
  `pulp_timeline_validate`, `pulp_timeline_explain`,
  `pulp_timeline_render`, `pulp_timeline_export`, and `pulp_timeline_import`.
  Seven operations retain stateless `pulp::tool-timeline` entry points; diff,
  undo, and redo are MCP-local operations backed by a live `DocumentSession`.
- Use `/seq` for the agent-guided inspect, validate, edit, explain, import, and
  consent-gated export workflow. Use `pulp seq` and `pulp render` directly for
  shell scripts, CI, and human-operated headless workflows. Prefer `seq apply`
  with typed command envelopes over inventing one-off mutation flags.
- Use the C++ SDK when embedding an editor, transport, compiler, renderer,
  recorder, or durable session. Keep document mutation in `DocumentSession`,
  playback derivation in `PlaybackProgramCompiler`, realtime rendering behind
  immutable programs and transport snapshots, and capture publication as
  ordinary timeline commands. A `ProgramCompileRequest` must declare its exact
  `sample_rate` explicitly and carry a `CompiledTempoMap` built at the same
  normalized `RationalRate`; omission or disagreement is a synchronous invalid
  request rather than an inferred default.
- Link `Pulp::timeline-agent-view` when an agent or remote client needs a
  bounded, deterministic projection of one pinned `DocumentView`. `AgentView`
  exposes a committed outline, cursor-paged clip regions, and a projection of
  one adjacent commit's `DirtySet` without widening the dependency-minimal
  `Pulp::timeline` model.
- Link the optional `Pulp::dawproject-import` SDK target only when ingesting
  DAWproject XML, and `Pulp::smf-interop` only when reading or writing Standard
  MIDI Files; keep the dependency-minimal model on `Pulp::timeline`.
- Use the generated schema surfaces to discover command/document shapes; do
  not hand-copy schema vocabularies into a client.

Start with `project_open` or `seq validate` when the source is unfamiliar.
MCP `project_open` returns a bounded process-local `session_id`; use that
identifier for apply, diff, undo, and redo. A session keeps the real
`DocumentSession` undo stack and latest canonical `DirtySet`; session handles
expire when that server process exits and may expire after it reaches its
bounded session capacity, so preserve the returned project snapshot as the
durable handoff.
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
- `TimeConform` is clip-level document intent: `None` is the default and keeps
  legacy behavior, `Resample` requests pitch-coupled varispeed, and `Stretch`
  requests tempo-preserving stretch. Non-default intent is valid only on a
  musical `MediaRef`; absolute clips and every non-media content alternative
  fail with `InvalidTimeConform`. Clip schema v2 persists the required lowercase
  spelling; v1 loads as `None`, and v2→v1 refuses either non-default value.
  Playback consumes `Resample` as pitch-coupled varispeed by mapping source
  phase across the authored musical tick interval, including tempo ramps and
  precise host beat mapping. `None` retains native-rate playback. `Stretch`
  compiles a timeline-rate immutable artifact off the audio thread with exactly
  the authored frame count, then plays it 1:1. A missing, over-capacity, or
  length-mismatched Stretch artifact fails compilation; it never degrades to
  native-rate playback.
- `MidiContent` is a flat POD array sorted by `(start, ItemId)`. Note durations
  are positive, pitch is MIDI 0-127, and channel is 0-15. Beside the notes it
  carries `MidiExpressionLane` controller streams, each an owned identity with
  points in `(position, id)` order and at most one lane per address. The document
  side is complete — authoring, persistence, id remap, and copy all carry lanes —
  but **playback refuses to compile a lane-bearing clip** rather than dropping the
  lanes silently, so a document that authors one cannot be played until the note
  program can carry controller values. See the playback skill for the two refusal
  codes and which of them the renderer is allowed to delete.
- `SequenceRef` makes a musical clip a non-owning placement of another
  sequence. Its source window begins at `source_start`; project construction
  rejects missing targets, cycles, and nesting deeper than eight reference
  edges. Sequence
  identity remains project-owned, so removing a referenced sequence fails.
  Playback expands references off the audio thread into immutable root-track
  programs. Stage 1 accepts child note/audio clips and rejects child devices,
  automation, takes, freeze, record-arm state, absolute clips, and non-neutral
  reference gain/fades. Source windows that intersect child audio fades also
  fail closed because Stage 1 cannot represent an envelope offset. A complete
  nested media clip preserves its `TimeConform` intent, but a source window
  that trims a conforming clip fails with `NestedSequenceUnsupported` until
  playback has a conform-aware source-range mapping. Expansion
  is bounded by `ProgramCompileRequest::max_expanded_note_events` and
  `ProgramCompileRequest::max_expanded_clips` across materialized clips,
  reference traversal, and reused track programs. The independent
  `audio_limits.max_clips` ceiling counts compiled audio regions only.
- Shared edits affect every placement. Use `build_diverge_transaction()` for
  eager copy-on-edit: it emits a complete-ID `CloneSequence` followed by
  `SetClipSequenceRef`. Pass two command IDs allocated from the session
  `WriterToken`; the helper consumes item IDs from `Project::next_item_id`
  before publication but never synthesizes writer-scoped IDs.
- For incremental compilation, construct `ProgramCompileRequest::invalidation`
  from the shared context registry and exact `CommitResult`; `submit()`
  builds and resolves the snapshot-scoped index against that same request. It
  maps direct edits, child dirtiness, built-in MIDI groove reads, and nested
  context readers to root tracks. The input pins that result's snapshot,
  revision, exact predecessor snapshot, and an immutable registry copy. If the
  predecessor is not the currently published project, sparse reuse is refused
  and the cumulative target snapshot is rebuilt in full. A later registry
  generation also forces a full compile without fabricating a document revision.
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
  Sequence-owned launcher state is an immutable persistent store: scene and
  slot identity indexes, authored-order links, and reverse clip/Jump references
  live together in `sequence_scene_internal.cpp`. Ordinary scene/slot edits
  path-copy only affected AVL paths; unrelated Sequence edits must retain the
  launcher store unchanged. Never replace this with scene-vector rebuilding or
  validate deletions by scanning every scene.
  `InsertScene` / `RemoveScene` and `InsertSlot` / `RemoveSlot` reduce through
  `transaction_scene_internal` with exact inverse and tombstone ownership.
- **A track owns eight identity kinds across four levels, and an incomplete
  owned set cannot fail at remove time.** `InsertTrack` / `RemoveTrack` reduce
  through `transaction_track_internal`. `plan_identity_deactivate` validates
  nothing — it emits one `Deactivate` per identity handed to it — so a missed
  kind leaks an identity that stays `active` with its owner gone, and only
  surfaces later as an unrelated `IdentityNotAvailable`, or as undo failing
  because tombstone restore requires each id to exist and be inactive. The two
  that get missed are the lane-parented pair, `AutomationPoint` and `Take`.
  Never hand-write the list: `visit_track_owned_identities()` in
  `owned_identity_traversal.hpp` is the single enumeration, and
  `visit_sequence_owned_identities()` calls it, so the two cannot diverge.
  `has_same_owner` compares only kind and parent while `target_error` compares
  all four coordinates, so right ids with a wrong coordinate cache survive both
  undo and redo and reject the next command two edits later — assert the
  complete `ItemLocation` of every level, not just `active`.
  `RemoveTrack`'s inverse is `InsertTrack{sequence_id, removed, following}`;
  `following` is what restores authored position exactly instead of appending.
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
- Clip schema v2 adds required `time_conform`. Its v1 upgrade inserts `none`;
  its v2 downgrade succeeds only for `none`, and a v1 envelope that illegally
  carries the field is rejected rather than normalized.
- Asset schema v2 adds optional `loop_info`. Its v2 to v1 downgrade succeeds
  only when that field is absent, and a v1 envelope that illegally contains
  the field is rejected rather than silently normalized.
- **A command schema is widened with optional fields at its current version —
  never by bumping it.** Document schemas walk registered `upgrades` /
  `downgrades` chains, but `decode_command` gates on exact version equality and
  has no upgrade hook, and no command schema declares a migration edge. Raising
  a command's version therefore rejects every envelope already written with
  `UnsupportedSchemaVersion`. Declare the new field `required = false` in
  `schema_registry.cpp`, read it with `JsonValue::find` in
  `serialize_command_decode.cpp`, and make absence decode to what the payload
  meant before the field existed (`replace_note_content`'s modifier arrays and
  `set_clip_playback_properties`'s `fade_shape` both do this). Command decode
  never validates a payload against `schema->fields`, so the registry entry is
  documentation and generated-artifact input, not the gate — the decoder is.
  Widening a command payload also means updating `equivalent()` (the
  idempotency cache answers a repeated transaction id with its first result, so
  a field it ignores lets a retry apply one payload and report another's
  outcome) and `retained_size()` (it falls through to `sizeof(T)`, so a field it
  forgets is under-counted rather than refused).
- **A brand-new command type is a different case: register it at version 1 with
  every field required.** Nothing has ever written an envelope naming it, so
  there is no older meaning to preserve and an omitted field is a malformed
  payload rather than a legacy spelling. The exact-equality gate still applies to
  it from then on, which is what pins it at 1 for good.
- `schema_release.hpp` records exact shipped structural type/version sets,
  including `v0.736.0` (Track v1), `v0.744.0` (Track v2), `v0.748.0`
  (Track v3), and `v0.750.0` (Track v4, before SequenceRef content and sequence
  mutation commands).
  `serialize_project_for_release()` applies the registry's
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
- **Adding a field to a persisted entity touches more than encode and decode, and
  the sites people miss lose data silently.** Beyond `serialize_encode.cpp` /
  `serialize_project_decode.cpp` you must also update the schema policy header
  (`current_version` plus an `<field>_introduced_version` predicate),
  `schema_registry.cpp` (declare the field, register BOTH migrations),
  `structural_registry_validation.cpp`, and `schema_json_preflight.cpp`. Then two
  more that no gate points at: **`id_remap.cpp`**, or every copy/paste/import
  quietly resets the field to its default, and **`snapshot_equivalence.cpp`**, or
  the journal-replay checkpoint guard treats documents differing only in that
  field as identical — and it is also the round-trip oracle, so a round-trip test
  asserted through `equivalent()` passes even when the field was never persisted.
  Grow the oracle in the same change, and prove a round-trip test fails with the
  encode disabled before trusting it.
- **Field order in the canonical JSON is alphabetical, so a new field renumbers
  its neighbours.** `track_order` sorts before `tracks` (`_` < `s`), which moved
  `tracks` from member index 9 to 10 in the preflight walk. A wrong index
  **compiles and silently reads the neighbouring member**. Re-derive every index
  after inserting a field rather than appending to the end of the list.
- A migration that adds an optional collection should write it **empty** rather
  than materializing a default, when the model already normalizes empty to the
  default: `Sequence::create` turns an empty `track_order` into identity order, so
  the v5→v6 upgrade writes `[]` and every upgraded document loads correctly
  without bloating. The **downgrade** is the asymmetric half — it must fail closed
  with `MigrationFailed` when the value is not equivalent to the default, exactly
  as the v5→v4 scenes downgrade refuses a non-empty scene list.
- Versioned persistence fixtures live under `test/fixtures/timeline/vN/` and
  remain permanent compatibility inputs. Exercise unknown envelopes from those
  files instead of rebuilding equivalent JSON inside a test so whitespace,
  escapes, and member order cover the exact-byte re-save contract.
- **Not every `.json` under `test/fixtures/timeline/` is a project.** The corpus
  holds three shapes and nothing in the files distinguishes them: complete
  `pulp.timeline.project` envelopes; single-entity **fragments** such as
  `v4/sequence-before-scenes.json`, a bare `pulp.timeline.sequence` at v4 used to
  drive `registry.migrate()` directly; and raw **payloads** such as
  `v1/unknown-content-envelope.json`, which is never parsed as timeline JSON at
  all — it is embedded as `OpaqueContent` to prove unknown bytes survive a round
  trip. Decoding a fragment or a payload as a project fails `InvalidSchema`, and
  that is correct refusal, not a broken fixture. `test/fixtures/timeline/corpus.index`
  declares each entry's kind so the distinction is stated rather than guessed;
  a new fixture must be listed there with its kind.
- `pulp-fixture-runner` validates every `document` entry against a sibling
  `<path>.expect` manifest: schema envelope version, identity, structural counts
  from `peek_project_summary()`, the interchange concept census, and the ordered
  identities of every collection in the arrangement spine. Regenerate
  manifests with `pulp-fixture-runner --corpus test/fixtures/timeline --update`;
  generation is deterministic, so a regeneration that dirties the tree means a
  document actually changed. The manifest is checked in **both** directions — an
  entry declared but not observed fails too, because a document that *lost* an
  entity would otherwise pass with every observed value still matching.
- **`test/fixtures/timeline/` is exhaustively indexed: every file must appear in
  `corpus.index`, and a host-side sweep enforces it.** Adding a fixture without an
  index line reddens that sweep on a file your diff never mentions. Two
  consequences worth knowing before you merge anything touching that tree. First,
  a `.expect` manifest is *excluded* from the sweep, so an unindexed fixture with a
  manifest is dead weight no gate can see. Second, and the one that actually bites:
  **any branch cut before the index existed cannot index its own fixture**, so the
  breakage appears only when the two branches meet on main — neither PR's CI can
  see it alone. Check `corpus.index` covers the tree before merging a
  fixture-adding branch.
- **The corpus runs twice per PR: natively under ctest, and compiled to WASM.**
  `core/interchange/wasm/CMakeLists.txt` is an Emscripten-only root that builds
  the runner from the portable sources, and `tools/ci/wasm-fixture-lane.sh`
  drives it (job `Timeline fixture corpus (WASM)` in `web-plugins.yml`). Two
  things follow. The wasm root **hand-lists** its sources, because linking
  `pulp::timeline` would drag in `pulp::runtime`'s mbedTLS/HTTP — so a source
  the desktop build gains is one the wasm build can silently miss; the root
  fails its configure when `core/interchange/src` drifts from its list, but the
  timeline list it borrows from `PulpTimelineSources.cmake` is shared and stays
  honest on its own. And the lane deliberately runs the corpus a second time
  against a broken copy and requires red, so a wasm build that validated
  nothing cannot pass. Run it locally with
  `source ~/emsdk/emsdk_env.sh && tools/ci/wasm-fixture-lane.sh <build-dir> 6`.
- A new `ProjectSnapshotCounts` field is asserted by the corpus only if it is
  also emitted by `collect_summary()` in
  `core/interchange/tools/fixture_runner_main.cpp`, which lists the counts one by
  one and is not generated. Add the count and skip that list and every manifest
  regenerates clean while the new entity goes uncounted in every fixture — the
  corpus reports green on a document whose new structure it never looked at.
- **A count cannot see an ordering, so a new ordered collection needs a line in
  `collect_identity_orders()` too** — same file, same hazard as the counts list
  above. The `order.*` manifest keys exist because counts plus idempotence are
  jointly blind to a lost order: dropping a sequence's authored `track_order`
  leaves `counts.tracks` intact, and an empty order re-serializes consistently,
  so a round trip agrees with itself on the wrong answer. Two classes are worth
  recording — **authored** orders (`track_order`, `scenes`, `device_chain`),
  which exist only in the document, and **value-derived** orders (`markers`,
  `regions`, `clips`), whose sort makes identity order a proxy for the positions
  behind it. Leaf content below a track stays on counts by design.
- **An order assertion only bites while the fixture's order is not the identity
  order.** `Sequence::track_order()` presents the identity order of `tracks()`
  for a sequence that never recorded one, so a manifest cannot distinguish "no
  authored order" from "authored order equals identity order" — a fixture in
  either state stays green when the order is dropped.
  `v6/sequence-track-order.json` is deliberately built with a non-identity order
  so the check can fail at all, and a case in `test/test_fixture_runner_cli.cpp`
  asserts its two orders still differ. Do not "fix" a red order assertion by
  rerunning `--update`; that regenerates from observed output without comparing
  and bakes the regression in as the new baseline.
- The census the runner records is `pulp::interchange::census()`, which lives in
  `core/interchange`, **not** `core/timeline`. Anything reaching for it takes an
  interchange dependency; that is on the portable floor, but it is a dependency
  edge worth knowing before adding one. That call is also why the runner binary
  is owned by `core/interchange/CMakeLists.txt` rather than by the test tree —
  see the placement convention below.
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
  package container. Asset locators describe possible package-relative bytes.
  Use `pulp::project-package` to hash-verify, fence, and no-replace publish
  content-addressed blobs before atomically replacing the stable package root's
  validated `project.json` generation. Generic file and directory publication
  is also no-replace, and unpublished staging remains unreachable; archive
  formats and interchange policy stay in their format/tooling layers.
- Project and subtree remapping are two-pass: allocate all owned IDs first, then
  rebuild the snapshot and fix references. `MediaRef::asset_id` is external to
  Clip/Track/Sequence remaps and is translated by `ExternalIdFixup`; failure is
  atomic and does not advance the caller's allocator. Preflight the complete
  owned subtree for duplicate IDs before allocating; this includes parent IDs,
  cross-track collisions, clips, note events, automation lanes, and automation
  points. Lane and point IDs remap as owned identities, target placement IDs
  remap as internal references, and opaque parameter IDs remain unchanged.
- Fallible public APIs return `pulp::runtime::Result`; do not throw.

### AgentView is a bounded projection of one immutable pin

`pulp::timeline_agent_view::AgentView` is created from one non-null
`DocumentView` and never follows a live `DocumentSession`. Every read therefore
requires the pin's exact revision; a caller that needs freshness must pin the
session again after a commit. `outline()` returns deterministic sequence,
track, and clip rows plus the complete `ProjectSnapshotCounts` census. Bounded
details are fail-closed commitments, not silent truncation: each omitted row
set carries a count and SHA-256, and the outline carries a commitment to the
canonical project snapshot.

`region()` pages clips in canonical `(start, ItemId)` order over one half-open
start-position window `[start, end)`. A continuation cursor is valid only for
the exact version, revision, sequence, anchor, and original window that issued
it; changing either bound while retaining an in-range cursor key is still
`InvalidCursor`. The request limit must be non-zero and no larger than
`Limits::max_page_items`.

`diff()` projects an already-produced `DirtySet` into deterministic outline
changes. Its `DirtyRevisionRange` must be exactly one adjacent transition ending
at the pin (`after != 0`, `before == after - 1`); stale, non-adjacent, and
underflow-shaped ranges fail with `InvalidProvenance`. This adjacency check does
not authenticate the `DirtySet`: the public type has no session-issued origin
token, so callers must pair it with the exact `CommitResult` that produced it.
Do not present AgentView as an arbitrary since-revision diff or as a substitute
for session provenance.

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

### MIDI content stores the wire's numbers, and cannot reach `core/midi` to name them

`MidiContent` carries notes, their deterministic modifiers, and the clip's
controller/expression lanes. It is tempting — and specs sometimes ask — to type
a lane's controller family against `core/midi`'s UMP/MPE/RPN declarations so the
document model and the wire cannot disagree. **`core/timeline` cannot include
`pulp/midi/*` at all.** `MODULE_FLOORS` in
`tools/scripts/timeline_engine_dependency_floor_check.py` gives `timeline` the
floor `{timeline, timebase, platform, runtime}`; `midi` belongs to `playback`'s
floor, one layer up. The check scans every include under `core/timeline/` and
fails the build gate, so this is discovered late if it is discovered by
compiling.

The resolution is not a parallel enum either — it is to store the wire's own
numeric domain and name nothing. `MidiLaneAddress` is five raw bytes (UMP group,
channel, status nibble, controller bank, controller index) and a lane point's
value is the full 32-bit channel-voice width that 7-bit and 14-bit MIDI 1.0
values scale into. `NoteEvent` set this precedent already with bare `pitch` /
`channel` / `velocity` integers. Only bounds the wire itself imposes are
validated here (group, channel, and status are 4-bit fields); *which* addresses
carry meaning is a playback question and does not belong in the document model.

The per-stream layout is also load-bearing rather than incidental. Lanes are
sorted by `(address, id)` and each lane's points by `(position, id)`, because
the question asked on every seek is "what did this one stream last say at or
before `t`" — two binary searches against that layout, versus a scan over every
event of every stream if lanes were flattened into one interleaved list.
Flattening them is the change that quietly makes seeking O(n).

### A new owned identity is added to the traversal, and nowhere else

`id_remap.cpp` enumerates what a clip, a track, and a sequence own **only**
through `owned_identity_traversal.hpp`. Both passes each level runs go through
it: `owned_ids()` collects for preflight's validity/duplicate check, and
`allocate_owned_subtree()` issues the destination identities. The same traversal
also feeds the identity index that `Project::create` and serialization build,
and the size check in the carried-id `remap_ids(Sequence, carried_ids, fixups)`
overload. So a kind added to the traversal reaches every one of those at once,
and the compiler is not what keeps them in step — *not restating the list* is.

Do not hand-write an owned-set walk at a call site, even when the level you are
adding to already has an obvious loop. Both ways of getting it wrong are quiet
and land far from the cause: an allocation walk that misses a kind returns a
copy without those objects, and a collection walk that misses one lets the new
identities skip validation entirely and then fails a later carried-id transfer
with `InvalidIdentityTransition` naming a **size mismatch**, which names no kind
at all.

Three cases pin this down, one per level, and each asserts a hand-counted
identity total — a distinctness or agreement check alone passes when a walk
silently emits fewer identities:
`Copying a clip issues fresh identities for its lanes and points` and
`A sequence copy and its carried-id transfer agree on the whole owned set`
(`test_timeline_midi_content.cpp`), and
`A track copy issues a fresh identity for every kind a track owns`
(`test_timeline_take_comp.cpp`), which carries every kind a track can own at
once because a per-kind case passes while a sibling kind goes missing.

One sharp edge remains: `rebuild_*()` dereferences `table.find(id)` for every id
it rewrites, assuming the allocation walk covered it. A kind dropped from the
traversal therefore reaches an empty-optional dereference — undefined behaviour,
not a checked error. In practice the garbage id it reads is refused by the next
`create()` and the remap fails, which is what the cases above observe, but the
diagnostic points at the rebuilt object and never at the walk.

Two more places a new `ItemKind` must land, neither of which the compiler will
point at: `restore_identities()` in `model.cpp` recomputes `parent_id` from
`(sequence, track, clip)` for every kind **except** an explicit
`AutomationPoint / Take / Slot` list, so any kind whose parent is a *lane* has to
join that list or every document carrying it fails to deserialize with
`ModelRejected` at `/data/identities` — an error that names no kind at all. And
`item_kind_name()` / the decode parser are two hand-maintained tables that must
gain the same spelling; the encoder's `switch` is exhaustive and will complain,
but the decoder's `if`-chain silently returns a failure for the unknown name.

## Editing contracts

- `InsertClip`, `RemoveClip`, `InsertAutomationLane`, `RemoveAutomationLane`,
  `MoveClip`, `SetNoteVelocity`, `SetClipPlaybackProperties`, `SetTempoMap`,
  `ReplaceNoteContent`, `SetNoteEvents`, `SetMeterMap`, `CreateAsset`, `RemoveAsset`, `InsertTakeLane`,
  `RemoveTakeLane`, `InsertTake`, `RemoveTake`, `SetRecordArm`,
  `SetActiveTakeLane`, `SetTakeComp`, `SetTrackFreeze`, `InsertMarker`,
  `RemoveMarker`, `InsertRegion`, `RemoveRegion`, `SetChordScaleLane`,
  `SetGroove`, and `SetTrackMixer` are the bounded mutation
  vocabulary. Automation commands attach or tombstone complete Track-owned
  lanes; map commands carry exact expected/replacement document values and
  participate in the same transaction, journal, undo, and replay machinery.
  `reduce_transaction()` is pure: it returns a new snapshot, exact canonical
  dirty set, and reverse-ordered inverse commands.
- `NoteTransformRegistry` is the control-thread apply-time extension point for
  pure `(note span, canonical params JSON, seed) -> note array` functions.
  `ApplyNoteTransform` is a typed preparation request, not a durable `Command`:
  `preview()` invokes extension code once, assigns fresh IDs only to outputs
  whose ID is invalid, and lowers the result to one `ReplaceNoteContent`
  transaction. The returned snapshot is speculative and the authoritative
  session is unchanged. Commit by submitting the exact returned transaction;
  a changed revision rejects it as stale instead of rerunning the transform.
  The journal therefore contains only canonical note data, never a callback,
  and undo/redo use the ordinary expected/replacement inverse while preserving
  note identity tombstones. A transform may retain an input ID, but a foreign
  or duplicate output ID is refused. Parameters must be a JSON object, are
  parsed under a 1 MiB bound, and are canonicalized before callback invocation.
  The expected plus output arrays share the durable command's five-million-note
  quota.
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

### Rebuilding a `MidiContent` from its notes alone silently drops everything else

A `MidiContent` carries four things, not one: the note array, a sparse
`NoteModifier` companion array keyed by note identity (probability, every-Nth /
first / fill condition, ratchet count), the `modifier_seed` those probability
draws are derived from, and the clip's controller/expression lanes.
`MidiContent::create` is overloaded, and the shorter overloads default away
exactly the state a rebuild means to keep — they **compile wherever the
four-argument overload does**. So any path that rebuilds a clip's content by
handing the notes back to `create` erases authored document state with no error
and no diagnostic. Reach for the four-argument overload on every rebuild path.

Re-attachment is **not uniform across the three companions**, and making it
uniform is its own bug:

- **Filter the modifier array to the surviving note ids.** Modifiers key on
  `note_id`, and `create` rejects one that names a note the content does not
  carry (`MissingItem`), so a blind pass-through converts an ordinary note
  deletion into a hard model failure. Dropping the orphan is the right answer,
  not an error: the modifier has no identity left to key to, and the same edit
  already tombstones that note's identity.
- **Carry the seed verbatim, even when no modifier survives.** It is authored
  document state that selects the replay, not a cache — zeroing it changes which
  notes sound as soon as a modifier is authored again.
- **Pass the lanes through unfiltered.** A `MidiExpressionLane` keys on a
  `MidiLaneAddress` — `{group, channel, status, bank, index}`, a channel-voice
  stream address that references no note at all. Applying the surviving-note-id
  filter to lanes as well is the reflexive "keep these consistent" edit, and
  because no lane identity is ever a note identity it deletes the clip's
  controller streams the moment an edit removes a note. Only a test that
  authors lanes *and* shrinks the note set catches it.

The rebuild paths that must each handle all three independently are
`reduce_replace_note_content` (`transaction_note_internal.cpp`), `rebuild_clip`
(`id_remap.cpp`, which additionally rewrites each `note_id`, lane id, and point
id through the remap table while leaving the address alone), and
`finish_pending_leaf` (`core/playback/src/sequence_content_lowerer.cpp`, where a
nested clip trimmed to its audible window filters modifiers to the retained
notes and refuses a clip with lanes outright — trimming has no defined answer
for a point that sits before the retained window yet still sounds inside it).

`ReplaceNoteContent` carries the modifiers as well as the notes, in optional
`expected_modifiers` / `replacement_modifiers` arrays. An authoring caller leaves
both empty and the reducer derives the surviving set from the clip; a
reducer-built inverse fills them in, which is what makes undo exact for an edit
that *removes* a modifier-bearing note — the dropped modifier is gone from the
content the inverse reduces against, so no filter over live state can recover it.
Lanes need no such treatment: they never leave the clip.

### Choosing between the two note-set commands

`SetNoteEvents` and `ReplaceNoteContent` both rewrite note values, and the
difference is what each one gates on.

- **`SetNoteEvents` names a subset and cannot change the note set.**
  `replacement[i]` must name the same note as `expected[i]`, so the reduction
  allocates no identity and tombstones none. Reach for it for any edit that moves,
  resizes, or retunes notes that already exist — a drag, a nudge, a velocity
  sweep. It carries only the notes under the gesture, so its journal cost tracks
  the selection rather than the clip.
- **`ReplaceNoteContent` gates on the clip's entire current note array** and is
  the only one of the two that can add or remove a note. Its `expected` is
  rejected unless it equals the whole note set, so a one-note edit in a
  10,000-note clip still costs 10,000 notes twice over.

Both emit exactly one command per transaction regardless of how many notes they
touch. `JournalLimits::max_commands` is a **count** ceiling, so a per-note command
shape would exhaust it far sooner than the byte ceiling it would relieve — which
is why neither of these is singular.

### A whole-content note edit is affordable per gesture, not per frame

`ReplaceNoteContent` carries both note arrays, so `DocumentSession` charges one
edit `retained_size(forward) + retained_size(inverse)` — roughly `128 * N` bytes
for an `N`-note clip against an 8 MiB `UndoLimits` default. That number is what
makes "a note editor cannot use this command" look obvious, and it is only true
for **one shape**: an open gesture group.

`candidate.closed` is set for `Single`, `End` and `Cancel`, and the eviction loop
in `document_session.cpp` advances only while the oldest group is `closed`. So:

- A **`Single`-phase** edit is charged once, closes on admission, and is
  immediately evictable. Any number of commit-on-release edits succeed; the only
  cost is undo depth (`8 MiB / 128N` groups). A single edit larger than the whole
  budget — past roughly 65k notes at the default — is still refused outright,
  because nothing can be evicted to make room for it.
- A **`Begin`/`Update`/.../`End`** drag coalesces every step into one group that
  stays open, and an open group is not evictable by anything. The charge
  accumulates per frame and the gesture dies partway through with
  `ConflictCode::UndoFull` — an in-progress drag that stops responding.

So a piano roll that commits on release persists through this command today; one
that streams `Update` per frame needs granular note commands first. Test the
distinction as a pair — the same command at the same size against the same
budget, reaching opposite outcomes — since either case alone passes against a
session with no budget at all
(`test_timeline_undo.cpp`).

Sizing a session's real ceiling, note that the **journal** binds before the undo
stack: `JournalLimits` defaults to 16 MiB / 1024 transactions and has **no
automatic eviction at all**, only an explicit `checkpoint()`. A test that means
to exercise the undo budget must widen the journal explicitly or it will measure
`JournalFull` instead.

### An identity rewrite must copy the source input, not re-enumerate it

`remap_ids` is the copy / paste / import path: it rewrites every owned
`ItemId` and carries authored value state across untouched. `rebuild_track`
used to express that as a designated-initializer `TrackInput{...}` naming every
field. A designated initializer **does not warn on an omitted member** — unlike
positional aggregate init, which `-Wmissing-field-initializers` catches — so the
omitted field takes its default and the rebuild compiles clean. That is how a
track's authored `mixer` came to be reset to unity gain and centre pan on every
copy, paste, and import, with no error and no diagnostic.

The shape that fails closed is copy-and-mutate over the source's own input:
`detail::track_input_of(track)` (`track_input_access.hpp`, implemented beside
`Track::Data` in `track.cpp`) returns the complete authored `TrackInput`, and
the rewrite then assigns **only** the identity-bearing fields over it. A newly
authored value field is carried by construction; the one place that must stay
exhaustive sits next to the storage it reads. Enumerate identity, inherit value.

The same reasoning applies to any rebuild of a model struct from a source
value. `SequenceInput` and `ProjectInput` in `id_remap.cpp` are still
enumerated in full — audit them against `model.hpp` whenever either struct
grows, because nothing in the compiler will.

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
  `CompileInvalidationIndex` bundles the nested dependency and context-reader
  reverse indices, and `resolve_dirty_tracks()` turns a `DirtySet` into an
  exact `DirtyTrackSet`.
- A `RegisteredContent` value is valid only under its exact schema identity and
  codec provenance. Build one immutable `SchemaRegistry` with built-ins plus
  the application's content schemas, use it to create or load the payload, and
  pass it when declaring the matching `ContentRendererRegistration` in
  playback. The declaration and its hook are process-local capabilities, not
  serialized Timeline state. An exact content value without an exact renderer
  must fail compilation as `UnresolvedRegisteredContent`; it must not become
  opaque silence.
- A context edit emits **two** things: a `DirtyContext{sequence, kind}` (which
  names what changed) and a companion `DirtyItem` flagged `DirtyFlags::Context`
  with no owning track (so an item-scanning consumer still sees the transaction
  changed something rather than silently seeing an empty dirty set). A
  trackless item that is *not* Context-flagged means a structural sequence edit.

Adding a context kind is a data change here plus a reverse-index case in the
compiler. It is never a reason to widen an invalidation.

The runnable cross-module contract is
`examples/timeline-sdk-consumer/registered_chord_renderer.cpp`. It registers the
chord-pattern schema and renderer, establishes a baseline compile, commits a
`SetChordScaleLane`, constructs `CompileInvalidationInput` from that exact
`CommitResult`, waits for the submission epoch, and proves that only the
context-reading generated track changes while an ordinary MIDI track reuses its
compiled owner. It also pins deterministic values and hash, unresolved and
bounded quota diagnostics, and weakest production aggregation. Update and run
that installed consumer whenever schema identity, registered content, context
dirty semantics, or playback hook declarations change.
The current compiler contract is notes-only, reset-state-only, and capped at
4096 fragment notes per clip. A nested `SequenceRef` that trims registered
content is `TrimmedRegisteredContentUnsupported` because the hook input has no
source-window offset. Renderer production declarations live with the process;
`ProgramWire` refuses nondefault declarations instead of transporting a claim
without its trusted hook.

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

Playback applies this context to built-in MIDI at compile time. The owning
sequence's groove moves onset and release by one shared delta, scales velocity
at the original onset, and is inherited unchanged by ratchets. Nested MIDI reads
the child groove exactly once rather than composing it with the parent. A
trimmed nested MIDI leaf with authored groove is currently refused because the
source-window chase rule for displaced notes is intentionally undefined.

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

### A version-gated field on the clip touches four readers, and the policy struct is what keeps them agreeing

`ClipSchemaVersionPolicy` (`clip_schema_policy.hpp`) is not bookkeeping — it is
the only thing that stops the readers of a clip envelope from disagreeing about
which version must carry which field. A field introduced at version N needs a
`requires_<field>(version)` predicate on that struct, plus a `static_assert` that
it is false at N-1 and true at N, and then every reader asks the predicate rather
than testing a literal:

- `schema_json_preflight.cpp` — require the shape when the predicate holds,
  reject the field's *presence* when it does not. Omitting the second half lets a
  document claim a field at a version whose readers ignore it.
- `serialize_project_decode.cpp` — the same two-sided gate, producing the model
  value.
- The migration pair, whose guard is the predicate's introduced version, not a
  literal.
- `structural_registry_validation.cpp` and the registry entry, whose `required`
  flags must match each other exactly or the registry self-check fails.

The downgrade refuses on anything but the field's default, for the reason in
*Downgrade refusals* above: a shape a v(N-1) reader cannot express is not an
annotation it can drop, it changes what the document sounds like.

### Drop-vs-refuse is decided per field, not per version

Two fields introduced by the *same* schema version can land on opposite sides of
the refusal rule, and the version number tells you nothing about which. Ask what
an older reader that silently discarded the field would then believe:

- If it would believe something **different about the music**, refuse. A chord
  event's bass turns `C/E` into `C`; the older reader states a different chord,
  not a less annotated one.
- If it would believe the **same thing, less precisely**, drop. A region's
  section role sits beside a free-text name the older reader still sees at the
  same position over the same span; the information is degraded, not falsified.

So one migration pair can refuse over one member and drop another in the same
pass. Say which and why in the migration's own comment — the next reader cannot
recover the reasoning from the code, and "it entered at the same version" is the
wrong reason to make them match.

### A member on an array element migrates per element, not by splicing `data`

Every earlier sequence migration inserts or erases one member of the top-level
`data` object, so the recipe reads as "find the neighbouring member's span and
splice". A member that lives on a **chord event** or inside a **region
envelope's `data`** is not reachable that way: each element needs its own edit,
and the count varies with the document.

`apply_edits` already takes a sorted `span<RawEdit>`, so a `std::vector<RawEdit>`
built by walking the array works — sort order is established inside
`apply_edits`, and overlapping or out-of-order edits fail closed there. Two
details bite:

- An object's `begin`/`end` bracket its braces, so an insert at the head goes at
  `begin + 1` and at the tail at `end - 1`. Check `end > begin + 1` first, or an
  empty object produces an inverted edit.
- Erasing a member needs the **key** offset, which `JsonValue` does not store —
  only value spans. Locate it with `source.find("\"<key>\"", <previous value>.end)`
  for a leading member, or `source.find(',', <previous value>.end)` for a
  trailing one, and assert the offset lands before the value's span before
  trusting it.

Prove the pair round-trips by asserting `upgrade(downgrade(x)) == x` on the raw
bytes, not just that both calls succeeded.

### Marker, region, scene, and slot envelopes carry a version they do not own

These nested types are written with `write_envelope(..., 1, ...)` and appear in
the registry with `current_version` 1, which reads like an independent version
axis. It is not one: they only ever exist inside a sequence, and the **sequence**
version is what decides their shape. Gate a new member on
`sequence_schema_policy`, leave the nested envelope's version alone, and say so
in a comment — bumping it instead makes "sequence v7 with region v1" a
representable state that means nothing, and forces two numbers to be kept in
agreement forever.

### Commands are not version-gated, so a shared decoder needs three states, not two

A document's schema version decides exactly whether a member is present, so the
decoder's natural parameter is a `bool requires_x`. But some entity decoders are
shared with the **command** path (`decode_region` via `insert_region`,
`decode_chord_scale_lane` via `set_chord_scale_lane`), and a command payload is
authored input with no migration path of its own. Forcing the member there
breaks every hand-written and previously-journalled command; forbidding it drops
the data silently.

The shape that works is a tri-state (`MemberPolicy { Forbidden, Required,
Optional }`): documents pass `member_policy_for(requires_x)`, commands pass
`Optional`. When a version introduces **several** members together, `Optional`
must still reject a payload carrying only some of them — half the detail is
neither spelling, and filling in the rest invents data.

### A "one field" document feature is often several schema bumps

Fields land on entities, and entities have their own schemas. A tuning reference
at project scope plus an override at instrument scope is `pulp.timeline.project`
**and** `pulp.timeline.track`, each with its own policy header, both migration
directions, and its own registry entry — plus a shared nested type if the value
is a struct. Size the work by the entities touched, not by the number of fields.

Two consequences downstream:

- **Content-address a payload reference rather than pointing at an `ItemId`.** A
  `ContentHash` needs no remapping, so a copy, paste, or import carries it with
  no `id_remap.cpp` change; an identity reference would need one.
- **Every suite that pins a current schema version breaks at once.** The literal
  `"type_name":"pulp.timeline.sequence","version":N` appears in several suites,
  in both raw-string and escaped-quote spellings — grep for both. Worse is the
  `migrate(domain, type, N, N - 1, saved, ...)` idiom, where `saved` is a fresh
  serialization: after a bump it starts from a version the payload no longer has
  and fails for a reason unrelated to what the test is about. A refusal test
  written that way passes for the wrong reason and stays green.

### `Sequence` grows through its named input, and each new owned field needs a version predicate

`Sequence` is pimpl'd behind `shared_ptr<const Data>` and built through
`create()`. Add full-fidelity owned state to `SequenceInput`; keep the existing
partial overloads source-compatible by having them construct the named input
with the new field's default. Do not extend the positional overload chain for
new durable state.

What does need care is `sequence_schema_policy.hpp`: each owned field gets its
own `<field>_introduced_version` plus a `requires_<field>(version)` predicate,
because the decoder and the preflight both have to know which document versions
carry it and which must *not*. Both sides are checked — a field present in a
version that predates it is as much a rejection as one missing from a version
that requires it — so reusing another field's predicate silently accepts
malformed documents on one of the two paths.

### Build a `Sequence::Data` successor by copying and naming, never positionally

Every `Sequence` edit but `create()` produces its successor with
`auto next = *data_;`, assignment to the fields it changes, and
`make_shared<const Data>(std::move(next))`. This is not style. `Data` carries
fourteen fields, two of which — `track_order` and `outgoing_sequence_refs` —
are both `std::vector<ItemId>`, so a positional brace-initializer that
transposes them compiles silently and corrupts authored order. The positional
form also made every new field a mechanical edit at every construction site,
which is how a field gets dropped from one of them.

Mutate the copy in place (`next.tracks.push_back(...)`) rather than building a
new vector and assigning it, so a copy-and-mutate site costs the same single
deep copy the positional form did.

### A track reorder needs its own primitive; erase + insert is wrong three ways

`Sequence::move_track` rewrites `track_order` and nothing else. Reaching for
`erase_track` + `insert_track` instead is wrong for a reorder because it:

- rebuilds `tracks`, `track_id_index`, and `outgoing_sequence_refs` — a whole
  subtree rewrite for an edit that permutes one `vector<ItemId>`;
- mints a fresh compile-structure token, so `Project::replace_sequence` bumps
  `sequence_compile_structure_token()` and invalidates a compiled program that
  never observed display order;
- **fails outright** for a legal edit: `erase_track` refuses to strand a
  launcher slot that sources one of the track's clips, so drag-reordering a
  track that carries a launcher clip would be rejected for a non-reason.

The same shape applies to any future authored-order edit. Authored position is
encoded as "the item this one stands before", with an empty value meaning last —
one encoding serves `InsertTrack::before_track_id`, `RemoveTrack`'s inverse, and
`MoveTrack`'s optimistic gate, and swapping expected/replacement is the exact
inverse. A destination that names the moved item itself must be refused: the
item is lifted out before the destination is located, so the request would
otherwise land silently at the end.

### `Command` `retained_size()` must account for heap the alternative owns

`retained_size()` has a chain of `if constexpr` arms and a `sizeof(T)` fallback.
A new alternative that owns a vector or a string falls into the fallback and
reports only its inline size, so the journal's memory accounting silently
under-counts and the bound it enforces stops meaning what it says. This does not
fail to compile and no test notices unless one is written for it.

### Adding a `Command` alternative is checked in three places, all fail-closed

`Command` has no exhaustive-visitor guard of the `ClipContentCases` kind, so the
guards are assembled rather than inherent. Three of them fire, and it is worth
knowing which, because they fail in different ways and one used to not fire at
all:

- `command.cpp`'s `equivalent()` is an `if constexpr` chain ending in a generic
  `else` that reads `.track_id`/`.clip_id`. A new alternative without those
  fields is a **compile error**. Note this fires *first* and can mask the others:
  an alternative that happens to carry those fields sails past it.
- `transaction.cpp` carries a `static_assert` that every alternative is claimed
  by **exactly one** reduce dispatch branch, reading the lists in
  `transaction_dispatch_internal.hpp`. Each family predicate (`is_track_command`
  and friends) is derived from its list by `std::visit` rather than repeating it,
  so the predicate and the claim cannot drift. Exactly-one, not at-least-one:
  requiring one claim also catches an alternative two families both handle, which
  branch order would otherwise resolve silently.
- `retained_size()` has a `sizeof(T)` fallback and so fails **open** — see the
  section above.
- each **family reducer** carries a `static_assert(!is_X_command_type<T>, …)` in
  the `else` of its `std::visit`, so a command claimed by the family with no arm
  in the reducer is a **compile error**. This closes the rung below the second
  guard: `transaction.cpp` proves which *family* claims an alternative, but the
  arms inside that family were a second statement of the same list in a
  different file, tied to it by nothing. Add a command to `is_note_command_type`,
  forget the arm, and it used to compile, route, fall off the end and be
  rejected at runtime as `ModelInvariant` — a command the document should have
  applied.

The reason the second guard exists: `pulp-timeline` builds `-fno-exceptions`, so
an alternative that reaches the end of the dispatch chain is not a
`bad_variant_access` and not a `ConflictCode` — it is a process `abort()` inside
a DAW. The chain's tail is now a rejection (`ConflictCode::ModelInvariant`)
rather than a bare `std::get`, so the abort is gone even if the assertion is
ever removed; verified by unwiring an alternative from its family, which aborts
with **SIGABRT (exit 134)** against the old bare `std::get` and returns a clean
`ModelInvariant` rejection against the current tail.

**A count-based check is not enough here, and this is the trap.** Asserting
`variant_size_v<Command>` against the sum of the family sizes passes when one
alternative is claimed twice and another not at all — the two errors cancel.
Verified: with `MoveTrack` claimed by both the scene and track families and
`SetTrackName` claimed by none, the claim total is still 39 and a count check
passes while the identity check fails. Compare identities, not counts.

**Two lists that must agree is the shape worth recognising here.** Every guard
above exists because one fact is written down twice — the claim list and the
arms, the variant and the type-name array, the variant and the envelope batch —
and nothing forces the copies to match. When you find a runtime rejection
guarding a pair of lists, the fix is to state the list once and let the compiler
check it, not to add a test that notices the drift later: a lint over two lists
can be skipped or can rot, while a `static_assert` in the same translation unit
cannot. A sweep comparing every family's claim list against its handled arms
reports zero discrepancies today, so the reducer guard above is regression
insurance rather than a fix for a live defect.

The distinction that decides whether this generalises: a branch guarding a
**relationship between two lists** is enumerable, so it can be made a compile
error. A branch guarding a **value invariant** — an overflow test, a range check
— is not enumerable and stays review discipline. Do not go looking for a tool
for the second kind.

Also extend the two coverage guards that pin the vocabulary, and note that
neither is ordered the way you would guess:

- the type-name array in `test_timeline_schema_registry.cpp`, sized by a
  `static_assert` against `variant_size_v<Command>` and compared elementwise
  against the registry, which sorts by type name — so the array is **alphabetical
  by schema type name**, not variant order. Insert in the right alphabetical
  slot; appending fails the elementwise compare.
- the encoded-envelope batch in `test_timeline_command_persistence.cpp`. Its real
  invariant is **one decoded command per alternative** — `commands.size() ==
  variant_size_v<Command>` plus one `holds_alternative` assertion per index,
  naming each alternative exactly once. It is *not* in variant order: the batch
  carries `InsertTake`, `RemoveTake`, `SetRecordArm` at indices 14-16 where the
  variant declares `SetRecordArm`, `InsertTake`, `RemoveTake`. That divergence is
  long-standing and harmless — do not "fix" it, and do not renumber the batch to
  match the variant. Appending a new envelope and asserting it at the final index
  is correct.

### A `ConflictCode` is a wire ordinal, and adding one changes nothing outside the process

- **Append at the end, never next to the semantic neighbour.**
  `tools/mcp/timeline_session_store.cpp` emits `static_cast<unsigned>(error.code)`
  verbatim as the `numeric_code` field of every transaction-failure envelope, so
  the ordinals are observable to MCP clients. Nothing *persists* them —
  `core/timeline/native/` has zero `ConflictCode` hits, so the file journal does
  not encode one — but the wire is enough. Verified by inserting one enumerator
  above `JournalDurability`: the tree builds clean and `JournalDurability` silently
  moves from 20 to 21. `test_timeline_transactions.cpp` pins the ordinals for this
  reason; extend it when you append.
- **There is no `switch` over `ConflictCode` anywhere, so a new code is a silent
  fallthrough rather than a compile-time event.** The only dispatch is the
  if/else chain in `transaction_failure()`
  (`tools/mcp/timeline_session_store.cpp`), whose tail maps every unhandled code
  to the string `"transaction_conflict"`; every other consumer is a two-way `==`.
  A new enumerator therefore compiles with zero `-Wswitch` warnings and reaches
  clients as the same generic string it always did. If a client must distinguish
  the new cause, extend that chain in the same change — the enum alone is
  invisible past the process boundary.
- **`CommandJournal::replay` must relabel a reducer failure, not propagate it.**
  Replay re-reduces each journaled entry; returning the reducer's error unchanged
  makes "this entry stopped reducing" byte-for-byte identical to "the model
  refused your live edit" — same `ModelInvariant`, and same *populated*
  `model_error`, which is the one field that otherwise discriminates the two.
  Overwrite `code` with `ReplayDivergence` and keep `item`, `related_item`, and
  `model_error` as the explanation. The general rule for any code path that
  re-runs recorded work: name the frame that failed, not its callee.
- **`TransactionError::code` defaults to `Unspecified`, and no producer may rely
  on that default.** Every site assigns `code` on the next statement or through a
  code-taking helper (`journal_error()`, `error()`, `reduction_error()`). The
  default exists only so that a forgotten assignment surfaces an obviously wrong
  value instead of a plausible real cause; removing it entirely is worse, because
  `TransactionError error;` would then leave a scalar indeterminate rather than
  fail the build.

### The edit vocabulary sits at the editor rung, and "cannot see `view`" is not why

`EditIntent` (`core/timeline_editor/include/pulp/timeline_editor/edit_intent.hpp`)
is device-neutral because the module cannot link `view`, so the header cannot name
a pointer type. True — but **that fact does not choose where the type lives**, and
reasoning from it is the trap. Check `MODULE_FLOORS` in
`tools/scripts/timeline_engine_dependency_floor_check.py`: the `timeline_editor`
row is a strict superset of the `timeline` row, and *neither* admits `view`. The
neutrality guarantee is identical at both addresses.

The argument that does discriminate runs the other way. `core/timeline`'s floor
excludes `timeline_editor`, so with the verbs at the editor rung the gate rejects a
reducer, migration, or serializer that reaches for one — an edge it simply does not
have while the verbs are in the model, where every file may include them and no
gate can object. `Draw`/`Erase`/`Move`/`Resize` are hit-test verbs: `Move` and
`Resize` lower to the *same* command and are distinct only because a front-end
tells a clip body from its edge. A headless importer, a `.pulpgraph` loader, and a
plugin that wants only commands should not carry that distinction.

Both directions are pinned by `--selftest`, so neither can be relaxed silently.

### Editor snapping stays in integer ticks and restarts at authored bars

`timebase::next_grid_boundary(double, double)` is a convenience for finite beat-domain
calculations, not an authoritative editor snapper. It has no meter input, and near the signed
tick endpoints one double ULP spans more than a thousand ticks. Converting a pointer's target
tick through it can therefore make two exact document positions collapse onto the same grid
answer.

Keep sequencer snapping in integer ticks. Use `CompiledMeterMap::tick_to_bar()` to obtain the
bar-local phase, generate the neighboring straight-grid candidates exactly, and only then pass
those candidates through `swing_position()`. Restarting at each bar prevents a time-signature
change from inheriting the previous signature's grid phase. Treat the authored bar end as a
candidate even when a custom interval leaves a partial final cell, and pin the nearest-tie
policy explicitly (the editor kernel chooses the later tick). Tests need both a meter change
whose new bar starts off the tick-zero grid and the signed endpoints; an all-4/4 fixture near
zero proves neither property.

Consequences worth knowing before you touch this:

- `pulp-timeline-editor` is **not header-only**. It carries `src/edit_intent.cpp`
  and is built `-fno-exceptions -fno-rtti` to match `pulp-timeline`. Those flags
  are the target's own exception-free proof — the rung sits outside
  `pulp-test-timeline-no-exceptions`, which covers only the portable timeline
  list, so a TU added here that needs exceptions must fail at its own build.
- The verbs are therefore **absent from the WAM/WebCLAP lanes**, which compile
  `PulpTimelineSources.cmake`'s portable list directly and do not link the editor
  rung. A browser build that wants to lower intents adds the rung; it does not add
  the file back to the timeline manifest.
- `GesturePhase` belongs in `core/timeline/command.hpp` by the same test that
  places the verbs: an undo group opens and closes on a bracket whether or not an
  editor produced it, so it is transaction-level vocabulary, not a tool verb.
- `EditIntentHost` (`= SequencerUiHostT<EditIntent>`) is declared beside the
  vocabulary. `SequencerUiHostT`'s parameter exists so the playback seam and the
  intent vocabulary can evolve apart; the alias is what keeps that parameter bound
  to something real instead of only ever meeting a test stand-in.
- Piano-roll gestures use the sibling `NoteEditIntent` and
  `NoteEditIntentHost`. Insert carries only `replacement`, erase carries only
  `expected`, and move/resize/velocity carry both with one identity;
  `ValidatedNoteEditIntent::create` rejects malformed or ambiguous shapes, and
  `NoteEditIntentHost` accepts only that wrapper so invalid raw values cannot
  cross the host seam. There is intentionally no note lowerer until the granular
  note commands land — do not route these through the O(clip)
  `ReplaceNoteContent` command as an interim implementation.

The corollary for anyone extending this: a front-end resolves device differences
**before** it builds an intent, and hands the kernel only resolved scalars. Hit
tolerance is the worked example — `HitMetrics` lives in `core/view` and projects a
pointer type onto one number; the kernel takes the number and never learns what
produced it.

Two things not to do here:

- **Do not add a fourth `GesturePhase`.** There are already three
  (`core/timeline/command.hpp`, `core/view/input_events.hpp`,
  `core/state/sequencer_state_channel.hpp`). Intents reuse the `command.hpp` one.
  A second spelling of an existing concept is a defect, not a feature.
- **Do not add a verb that lowers to zero commands.** Select, marquee and
  zoom-to-range are deliberately absent: they are view state, and routing them
  through the document channel puts transient selection into undo history.

### An intent lowerer validates the gesture; the model keeps its own rules

`lower_edit_intent` and `lower_track_edit_intent` are pure and hold no `Project`,
so the only things they can check are properties of the *gesture*: identity
agreement between the transaction and command ids, the undo-group bracket a
non-`Single` phase requires, and ids that are structurally invalid. Everything
that needs the document — does the destination exist, is the track in this
sequence, does the optimistic gate still hold — belongs to the reducer.

**Resist re-checking a model rule in the lowerer even when it looks cheap.**
`Sequence::move_track` refuses a track named as its own destination, and says why
(the track is lifted out before the destination is located, so the request would
silently land at the end). Copying that check up into the lowerer would put the
same rule in two places that can drift, and the editing paths enforce the model's
copy. Pin *where* the refusal comes from with a test that lowers successfully and
asserts the session rejects it — otherwise a later reader adds the duplicate
"for a better error message."

The line worth drawing: **a neighbour id that is present but structurally invalid
is a malformed gesture** (the front-end never resolved it) and belongs in the
lowerer; **an id that is well-formed but absent from the document** is the
reducer's `MissingItem`. An `std::optional` destination left empty is neither —
it is a request for last position.

### A negative control on a compound condition can exercise half of it

Disabling one disjunct of an `if (A || B)` guard and seeing the suite stay green
does **not** mean the guard is untested — it may mean your case only ever
exercised `B`. This bit here for real: a control rewritten as

```cpp
if (false && (expected && !expected->valid()) || (replacement && !replacement->valid()))
```

parses as `(false && A) || B`, because `&&` binds tighter than `||`. The
replacement half still fired, the suite passed, and the control looked like a
false alarm. It was not — it had found a genuine gap, because the test asserted
only the *destination* neighbour and never the *gate* neighbour.

Two rules from it: **delete the whole condition rather than negating one operand**,
so a surviving disjunct cannot answer for the one you meant to disable; and
**assert every disjunct separately**, so the two halves fail at different lines.
A control that passes is either a blind test or a malformed mutation, and the two
are indistinguishable until you read the mutated source.

### A boundary test that never moves the boundary is satisfied by a constant

`undo_gesture_budget`
(`core/timeline_editor/include/pulp/timeline_editor/gesture_budget.hpp`) predicts
how many steps one open gesture can commit, and the obvious way to test it is a
self-controlling pair: size a session's budget for N steps, assert N commit and
the N+1th comes back `ConflictCode::UndoFull`. Same command, same size, same
budget, one variable, opposite outcomes — which is the right shape and is still
not enough.

**A prediction that ignores its inputs and returns the literal N passes that pair
perfectly.** The budget was built from N, so a hardcoded N sits exactly on the
boundary and both halves agree with it. Confirmed by mutation, not by argument:
replacing the whole computation with `return 4` failed only the payload-growth
case and passed both halves of the pair.

The fix is to run the pair at **two** budget sizes (`test_timeline_gesture_budget.cpp`
uses 4 and 7). A constant then fails at whichever one it is not. Generally: a pair
pins a boundary, but only a boundary that **moves** pins the rule that places it —
so any threshold test that exercises one threshold is satisfiable by that
threshold as a literal. This applies well beyond this function: quota, limit and
capacity assertions across `SessionLimits`, `JournalLimits` and `UndoLimits` all
have the same shape.

Two further traps this function's tests exist to avoid:

- **Set `JournalLimits` wide, explicitly.** It binds the same gesture
  independently, defaults to 16 MiB / 1024 transactions, and has **no automatic
  eviction** — only `checkpoint()`. Left at its defaults it is what a test aiming
  at the undo budget ends up measuring, and `JournalFull` is not `UndoFull`.
- **Assert the conflict code, not just the failure.** A gesture stopped by a stale
  revision or a malformed bracket also stops at the step you are pointing at.

## Schema codegen & drift gate

### Bumping a schema version touches two hand-written validators the codegen never reaches

The registry entry, the migration pair, and the encode/decode sites are the
obvious edits. Two more are hand-maintained, are not generated, and produce no
compile error when they go stale:

- `core/timeline/src/structural_registry_validation.cpp` holds a literal
  `ExpectedField[]` per structural type plus its `{current_version,
  oldest_readable_version}` pair, and rejects a registry whose field list or
  version does not match exactly. Miss it and `serialize_project` fails on a
  document you just built in memory, with `InvalidSchema` /
  `UnsupportedSchemaVersion` and no path — it reads like an encoder bug.
- `core/timeline/src/schema_json_preflight.cpp` gates the structural scan on a
  literal version (`content_type != "…" || version != 1`). A version bump that
  leaves it pinned sends every new document down the opaque-passthrough branch
  and skips its validation entirely — a silent weakening, not a failure.

Both must accept the whole readable range, not just the newest version: a v1
document is preflighted *before* migration runs, so the older shape has to pass
too. Field lists in the validator are ordered, and the registry lists fields
alphabetically, so a new field goes in its sorted position, not at the end.

`schema_json_preflight.cpp` has a second, sharper trap inside it. Each structural
scan declares its wanted members as a `std::array` of `JsonSpanMember` and then
indexes the results **positionally**, even though the lookup itself matches by
name. The arrays are kept alphabetical, so a new field inserted in sorted order
renumbers every index after it — and the result still compiles, still runs, and
quietly shape-checks the wrong member. Renumber the whole block by hand whenever
a field lands anywhere but the end.

A **new Track-owned collection** has a fourth hand-written table on top of those
two: `valid_track_data_shape` in `core/timeline/src/track_schema_migrations.cpp`.
Every track migration calls it, and it rejects any member it does not know, so a
field added to the registry and the encoder but not here makes the *migration*
fail rather than the encode — a different symptom from the same omission. The
order the four bite in is worth knowing, because only the first one you hit
tells you anything: `structural_registry_validation.cpp` fails the ENCODER at
path `/` with `InvalidSchema` on a document just built in memory, which reads
like an encoder bug and gives no hint that two more tables exist behind it.

An optional collection is far cheaper to add than a required one, and the
`mixer` v6→v7 pair is the pattern: write absence for the empty value, make the
upgrade a version stamp and nothing else, and make the downgrade **refuse** when
the member is present rather than dropping it. Nothing else in the migration
needs to move bytes.

### `Track::create` moves its input partway through, so late validation reads an empty collection

`Track::create` hands `input.device_chain` to a `shared_ptr` about two thirds of
the way down, and `input.automation_lanes` right after it. Validation added
below that point must read `*device_chain`, not `input.device_chain` — the
moved-from vector is empty, every reference check against it fails, and the
result is a model that rejects every document that uses the feature while the
unit tests for the collection itself still pass. The compiler says nothing: a
moved-from `std::vector` is a valid object.

Version-gating a field in the preflight also **changes the error a caller sees**.
The preflight runs before the decoder, so an omitted version-gated field is
rejected there as `InvalidSchema` (that is `require_shape`'s default
`missing_code`), and the decoder's own `MissingField` branch for the same field
becomes unreachable through `deserialize_project`. Keep the decoder check — it
still guards the decoder driven directly — but assert `InvalidSchema` in a
document-level test, or the test is describing a layer it is not exercising.

A bump also invalidates any test that pins a **"future version" sentinel** —
`test_timeline_persistence_limits.cpp` asserts that a known built-in type name at
an unknown version stays opaque and quota-terminal, using a literal version one
past the current one. When the schema catches up to that literal the case still
passes trivially while no longer testing the opaque path at all, so raise the
sentinel in the same change.


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

**A tool that claims to be portable belongs under `core/`, not under `test/`,
even when its inputs live in `test/`.** `pulp-fixture-runner` is the worked
example: its source is `core/interchange/tools/fixture_runner_main.cpp` while its
corpus stays at `test/fixtures/timeline/`, and `test/cmake/timeline_tests.cmake`
holds only the two ctest registrations. Two things break if such a tool lives in
`test/`. First, `add_subdirectory(test)` is gated on
`PULP_BUILD_TESTS AND NOT ANDROID AND NOT IOS`, so the binary cannot be
configured at all on the mobile lanes it exists to serve. Second — and this is
the one that stays silent — `timeline_engine_dependency_floor_check.py` walks
`core/<module>/` and nothing else, so a `#include <pulp/view/…>` added to a
runner under `test/` passes every gate while the file's own comment still claims
a portable floor. Relocating into an owning module puts the file inside the
existing scan with no new `MODULE_FLOORS` row. Pick the module at the *top* of
what the tool links (interchange, not timeline, since it calls `census()`).

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

Three things bite when adding a row:

- **`verify()` reports `missing required engine module` when a row names a
  directory that does not exist.** A row and its target therefore land in the
  same change, always — you cannot declare a floor ahead of the code it
  constrains. That is the intended discipline, not an obstacle to work around:
  it is what stops a floor from being written around a violation that already
  shipped.
- **Allow both spellings of the module's own name.** The row key is the
  directory (`timeline_editor`), but `LINK_RE` reads a CMake alias verbatim, so
  `pulp::timeline-editor` yields the token `timeline-editor`. A row carrying
  only the underscore spelling reads a helper target's self-link as an
  outside-floor violation.
- **The generic selftest loop proves detection using `pulp/render`, which is in
  no floor at all.** When a row's defining rule is that it cannot reach a module
  that *is* in the table — `timeline_editor` and playback — assert that pair by
  name too, or the rule survives someone widening the other row.
- **A row constrains the whole link closure, not the module's own build file.**
  The check follows `target_link_libraries` through `core/<module>/CMakeLists.txt`
  to a fixed point, so a row is only honest if every module it reaches
  transitively is in the set. Measure before declaring: a row that names one
  in-floor dependency can still be breached three links down, and the error names
  the chain (`playback -> audio -> state`) rather than only the destination.
  Where the closure genuinely exceeds the floor, record the surplus in
  `LINK_CLOSURE_DEBT` instead of widening the row — the row also governs
  includes, and folding a link fact into it silently grants a header permission
  nobody argued for.

Two rules keep the walk honest and are easy to break by accident:

- **Executables are not followed.** Nothing can link an executable, so a helper
  binary in a dependency (`pulp-timeline-schema-emit`, `pulp-fixture-runner`)
  must not raise the floors of everything above it. Its links are still policed
  at depth zero by the direct scan of its own module's build file — that is the
  half the selftest's helper-target assertions cover.
- **A token that resolves to no `core/` directory is not a module.**
  `pulp-tracing` (defined in `tools/cmake/PulpTracing.cmake`) and
  `pulp-cpp-httplib` (an INTERFACE shim inside `core/runtime/CMakeLists.txt`) are
  build plumbing reachable from every row. They are skipped transitively rather
  than added to five floors; a module's own build file naming one is still caught
  at depth zero.

**A `core/` module that links a declared engine module but owns no row is the
other half of the contract.** The intuitive rule — flag an engine module that
links something undeclared — is useless: it fires on `platform`, `runtime`,
`audio` and `midi`, which are floor primitives by design. The rule that bites is
the inverse, and `ENGINE_CONSUMERS` carries the ones that exist, each with a
reason. `format` and `host` are the sanctioned upper layers; `sequence`, `smf`
and `dawproject` sit above or beside the engine and have not been given floors.
The list rejects an entry with no reason and an entry whose subject no longer
links the engine, so it can neither grow silently nor rot.

A new `core/<module>` directory also drifts `codecov.yml`, whose flags and
components mirror `core/*`. Add the flag and the component alongside the target;
`tools/scripts/gates.sh` catches the omission before CI does.

**A new rung takes a new row — never widen an existing one.** `timeline_editor`
carries no `view` and no `canvas`, which is correct for the editor kernel and
will look like a gap the first time a view lands beside it. It is not. Widening
that row to admit `view` makes every consumer of the kernel pay for `core/view`,
which is the exact coupling the rung split exists to prevent. A view target gets
its own directory and its own row, sitting above this one.

**A type two rungs both need goes in their floors' intersection, not in either
rung.** `playback` and `timeline_editor` exclude each other, and `timebase` is
in both rows — so it is where a shared value type lives, and moving one there is
legal with no floor change at all. `timebase::LoopRegion` is the worked case: the
editor's `UiLoopRegion` and playback's `LoopRegion` had been the same three
fields in the same order. Check `MODULE_FLOORS` for the intersection before
placing anything; if the rows do not admit the home you want, the placement
argument is wrong and widening a row is not the repair.

The same reasoning bounds what may *not* move down. A converter between two
rungs' types names both, so it belongs above both — not in either rung, and not
in `timebase`, which may name neither. Until an engine above both exists there is
no legal home for one, and adding it anywhere reachable would mean widening a row.

`UiPlayhead` speaks the editor's nouns rather than mirroring the transport's:
`UiTransportState` in place of `is_playing`/`scrubbing`, and `continuity_epoch`
in place of `playback_epoch` — the rung's floor excludes `playback`, and a host
backed by a scripted value or a plugin's own engine has a continuity guarantee
without having a playback epoch. `continuity_epoch` is what a view checks before
smoothing motion between two readings; `program_generation` cannot substitute,
because a loop wrap breaks continuity without recompiling anything.

### The floor has two directions and they need different instruments

`timeline_engine_dependency_floor_check.py` is outbound: for each engine module,
does it reach up? `tools/cmake/PulpLinkFloor.cmake` is the inbound counterpart:
given a consumer — a plugin, an app — what does linking it actually cost, and
did it say so? `pulp_assert_link_floor(<target> TIER <name>)` gates,
`pulp_report_link_closure(<target>)` measures and writes
`${CMAKE_BINARY_DIR}/link-floor/<target>.txt`.

Reaching for the Python checker to answer the inbound question does not work,
and the reasons are properties of CMake rather than of taste:

- **A PRIVATE link to a static library still propagates.** CMake records it as
  `$<LINK_ONLY:dep>` in `INTERFACE_LINK_LIBRARIES`, because a static archive
  cannot resolve its own dependencies. Flipping a keyword from `PUBLIC` to
  `PRIVATE` leaves the consumer's link line byte-identical. A checker reading
  source text sees the keyword and has to model that rule; one reading the
  resolved property observes it. Modelling it wrongly under-reports, which is
  the one failure mode that makes a floor check worthless.
- **A plugin's link edges are not written down anywhere a parser can read.**
  `pulp_add_plugin(X FORMATS CLAP)` creates `X_CLAP` inside a function, links
  `${_PULP_VIEW_TARGET}` — a variable `_pulp_pick_target` chooses — and only
  when `PULP_HAS_CLAP`. Following that from text means evaluating CMake
  variables, functions and conditionals. The engine check gets away with text
  only because it reads `core/<module>/CMakeLists.txt`, where links are literals.

Gotchas once you are reading the resolved graph:

- **Name a module from `SOURCE_DIR`, never from the target name.** `pulp-view`,
  `pulp-view-core` and `pulp-view-script` are all `core/view`, and there is no
  naming convention that stays true on its own.
- **A dependency contributes `INTERFACE_LINK_LIBRARIES`, not `LINK_LIBRARIES`.**
  Only the root target's own direct links come from `LINK_LIBRARIES`. Reading a
  dependency's would invent edges: a shared library resolves its PRIVATE links
  inside its own artifact and charges the consumer nothing.
- **The walk must close over what it has seen.** `core/host` links
  `pulp::format` while format reaches back through view, so a walk without a
  visited set hangs on the real repo rather than in a fixture.
- **The declared-graph closure is a superset of the link line, and correctly
  so.** Cross-checking `StepSequencer_CLAP` against
  `CMakeFiles/<t>.dir/link.txt` shows the two agree on every archive and differ
  by exactly the header-only libraries: `pulp-signal` is an `INTERFACE` target,
  so it is a real dependency that costs headers and produces no `.a`. Do not
  "fix" the walk to match the archive list — the archive list is the narrower
  instrument.

**Every plugin links the view stack, drawing or not.** VST3, CLAP and AU each
link `${_PULP_VIEW_TARGET}` unconditionally in `PulpPluginFormats.cmake` and
`FATAL_ERROR` if it is absent, so the closure runs
`<plugin> -> pulp-view -> pulp-view-script -> pulp-view-core -> pulp-host ->
pulp-playback`. Two consequences worth knowing before reading a report as a
statement about the plugin's own code: a plugin acquires a transport it never
asked for, and a sequencer plugin's reach to `core/timeline` today arrives
through that chain rather than through its own engine. A red reading here is
usually the format packaging's bill, not the plugin's doing.

**Tier versus debt.** A tier is a bound several targets share, declared centrally
so no single target can widen it alone; `PULP_LINK_FLOOR_DEBT_<target>` records
what that target's closure drags in beyond the tier. Keep measured-but-unearned
reach in debt, never in the tier — an entry is a fact to pay down, and deleting
it once the edge is cut tightens the gate with no other edit. The check rejects a
debt entry that is no longer linked and one that duplicates its tier, so the list
cannot outlive its subject.

**Debt is declared per target but measured per CONFIGURE, and that asymmetry
bites.** A closure is whatever *this* configure links, and the project does not
have one: `core/render` is behind `PULP_ENABLE_GPU`, `core/host` is behind
`NOT IOS`. Under a narrower configure the modules those guards remove are absent
by construction — and the plain debt list reads that absence as rot, failing with
*"debt entry 'render' is no longer linked. Delete it to tighten the bound"*. The
gate is asking you to delete an entry the wider configure genuinely needs, so
**taking the advice literally trades a false failure on GPU-less trees for a
false pass on GPU-enabled ones.** Declare such entries in
the entry under that same condition instead — `if(PULP_ENABLE_GPU)
list(APPEND PULP_LINK_FLOOR_DEBT_<target> render)` — so where the condition
holds the entry is an ordinary debt entry and is still rot-checked, and only the
configure that cannot have the edge stops asking. Guard on the condition that
decides whether the module is **built**, never on one edge: `render` has two
independent edges (`pulp_add_plugin` under `PULP_HAS_SKIA`, and `core/view`'s
`if(TARGET pulp-render)`), so guarding either leaves it unfalsifiable from one
side. `playback` and `timeline` are guarded on the same `NOT IOS` for a
*different* reason worth stating wherever it is written: both **are** built on
iOS and are guarded because the target's only route to them runs through `host`.
Establish that by configuring both ways — **not** from the closure report, which
records one shortest chain per module (`PATHS_OUT`, BFS first-arrival), so a
second longer edge is invisible in it. Note the
blast radius before dismissing this as a test failure — the assertion runs at
configure time from the plugin's `CMakeLists.txt`, so with
`PULP_BUILD_EXAMPLES=ON` a mismatch kills `cmake --build` at
`cmake_check_build_system`, and a headers-only `external/skia-build` (an ordinary
fresh-worktree state) forces `GPU=OFF` on its own.

Two readings that cost real time here: the same assertion text covers different
failures — an iOS Simulator configure loses `host`, `playback`, `render` and
`timeline` (the `NOT IOS` guard severs `view-core -> host` and takes the whole
chain), while a desktop `GPU=OFF` configure loses only `render` — so **match the
literal entry list, not the first line.** And `cmake-link-floor-selftest` passing
says nothing about this: its battery configures its own fixture project, so it is
green on a desktop tree while the real gate is broken on two narrower ones.

**A tier is an upper bound and proves only absence.** `TIER` says nothing outside
it is reached; it cannot say anything inside it *is*. "Reaches nothing extra" is
satisfied most easily by reaching nothing at all, so a tier naming a module the
target never links stays green while reading, to anyone scanning the row, as a
proven link. Do not answer "does the piano roll ship inside the plugin?" from a
tier — that question needs the other bound, and a tier that has been widened to
look like an answer is worse than no answer, because it is a false one.

**`REQUIRE <modules...>` is the lower bound.** Optional, listed at the call site
next to `TIER`, and every module named must appear in the measured closure or the
configure fails naming the ones that do not:

```cmake
pulp_assert_link_floor(MyPlugin_CLAP
    TIER    sequencer-plugin
    REQUIRE format state)
```

`REQUIRE` grants no reach of its own — a required module must also be declared in
the tier or the debt list, or no closure could satisfy both bounds at once, and
the check reports that contradiction rather than letting `REQUIRE` become a third
permission list. Pair the two whenever the criterion is about what the artifact
*contains*: the upper bound stops the editing stack leaking in, the lower bound
stops the claim being satisfied by the module quietly disappearing.

**What the two sequencer plugins prove.** Measured on a macOS
configure, it reaches eighteen modules, and `timeline_editor` is not among them —
`pulp-timeline-editor` is a live target in that same configure and this plugin
does not link it. `timeline` it reaches only through
`pulp-view -> … -> pulp-host -> pulp-playback -> pulp-timeline`, with no
`pulp/timeline` include anywhere in the example's own sources, so `timeline` is
recorded as debt rather than claimed by the tier. Its `REQUIRE format state` is
correspondingly narrow: this plugin's own code contributes no module edge beyond
the adapter it is packaged as. So the honest reading of a green run here is "a
step sequencer packaged as a CLAP, carrying no editing stack" — **not** "a piano
roll inside a plugin".

`TimelinePluginProof_CLAP` is the positive counterpart. Its processor owns a
`Project` and `DocumentSession`, implements `EditIntentHost`, returns a native
view from `create_view()`, and stores canonical Timeline JSON in plugin-owned
state. It claims `sequencer-plugin-editor` and requires `format timeline
timeline_editor`, so configure fails if any of those three disappears. Its view
is deliberately only a ruler/playhead shell: the target proves the integration
boundary, not a piano-roll interaction surface.

**Know where the verdict runs.** The assertion lives in the consumer's own
`CMakeLists.txt`, so it is evaluated only where that consumer is configured. For
an example plugin that means the Shipyard mac/windows lanes
(`PULP_BUILD_EXAMPLES=ON`) and ordinary dev builds — not the GitHub-hosted legs
of `build.yml`, which configure `PULP_BUILD_EXAMPLES=OFF`. It also reports one
configuration: a link that only exists under `WIN32` is invisible to a macOS
configure. `tools/scripts/link_floor_selftest.py --mutate` is what runs
everywhere; it weakens the checker eleven ways — including one that stops
checking `REQUIRE` at all — and requires each to be caught.

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
`core/timeline/tools/schema_mcp_emit.py` emits the fixed ten operations
(project open, command apply, diff, undo, redo, validate, explain, render,
export, and import) into
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
advertisement and exact-ten operation dispatch. Export's accepted-loss enum is
projected from the committed interchange concept authority, excluding
`unknown`; never hand-copy or broaden that vocabulary.

```
python3 core/timeline/tools/schema_mcp_emit.py \
    --out core/timeline/schema/timeline_mcp_tools.json
```

The `timeline-mcp-drift` ctest byte-checks the artifact;
`timeline-mcp-selftest` (`core/timeline/tools/test_schema_mcp_emit.py`) proves
determinism, exact operation membership, complete domain projection,
fail-closed empty command behavior, and confirm-the-failure.

### One clean drift run proves one artifact, and a clean auto-merge proves nothing

Two ways to believe a generated tree is in sync when it is not.

**A bare `schema_drift_check.py` guards only its own `DEFAULT_ARTIFACT`**
(`timeline_schema.json`). It is a reusable gate, not a whole-repo check, so its
exit 0 says nothing about the four sibling projections or the three interchange
artifacts. The enforced set is eight registrations, each naming its own
`--artifact`/`--emit-cmd`: five in `test/cmake/timeline_tests.cmake` (the
manifest plus `.d.ts`, CLI verbs, JS facade, MCP tools) and three in
`test/cmake/interchange_tests.cmake` (`concepts.hpp`, `capability_tables.hpp`,
`docs/reference/interchange-matrix.md`). Verify the set — `ctest -R 'drift'` —
rather than a single hand run. Note the interchange three come from
`capability_emit.py` over `core/interchange/capabilities/*.json`, a different
generator with different inputs; `--update` on the schema gate does not touch
them.

**After a merge that touches any generator's inputs, regenerate every artifact
that generator produces — whether or not git reported a conflict.** These
artifacts are single-line or densely minified, so git will often auto-merge them
cleanly and be wrong, with no conflict to alert you. A merge bringing in schema
changes from both sides can leave `.d.ts`, CLI verbs, and the JS facade silently
stale while the manifest's own gate passes. "Never resolve a generated artifact
as text" covers only the conflicting case; the set that matters is every output
of every generator whose inputs the merge touched.

Two adjacent invariants the drift gates do not cover, both fail *after* a merge
looks fine: `tools/mcp/CMakeLists.txt` FATAL_ERRORs at configure time unless
`timeline_mcp_tools.json` carries exactly ten tools in an exact name order, and
`core/interchange/capabilities/concepts.json` is append-only **and
order-significant** because position fixes the generated enum ordinal. For
`concepts.json`, check that the *other* side's entries survived at their
original ordinals — confirming only your own additions passes a resolution that
silently dropped theirs, and both wrong resolutions compile.

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
pulp seq export <project.json> --format <smf|dawproject> --plan
pulp seq export <project.json> --format smf --out <new-directory> \
pulp seq export <project.json> --format dawproject --out <new-file.dawproject> \
  [--accept-loss <concept-id>]...
pulp seq import <file.mid|file.dawproject> --format <smf|dawproject> \
  --out <new-directory>
pulp render <project.json> --out <file.wav> [--sample-rate <hz>]
```

Do not add hand-authored mutation verbs to `cmd_seq.cpp`; `apply` consumes the
registry-derived typed command envelopes. `render` emits Float32 WAV and does
not silently instantiate hosted devices or invent plugin delay compensation.
The headless explain result reports unknown PDC offsets as JSON `null`.

Import and export refuse every existing destination, stage into a private
sibling directory, and publish the complete directory atomically. Never add a
force, overwrite, or accept-all path. Export requires separate consent for
every planned lossy concept so a newly introduced loss stops an unattended
pipeline. Run `--plan` first: it returns the canonical manifest and
`required_consent` without writing anything, even when the project is lossless.
Planning rejects `--out` and `--accept-loss`; it does not invent a destination.
Publishing requires `--out`. MCP uses the equivalent outputless
`plan_only: true` input and rejects `output` or `accept_losses` in that mode.
Refusal and successful export
results carry the same manifest object. SMF exports contain `project.mid`;
DAWproject export publishes one standard `.dawproject` ZIP containing a root
`project.xml`, the manifest, and referenced media entries. DAWproject import
requires a `.dawproject` ZIP, rejects unsafe or unsupported archive entries,
confines media resolution to safe package-relative entries, and publishes
canonical `project.json` plus sealed sibling artifacts into a new directory.

The live MCP server embeds `timeline_mcp_tools.json` at configure time and
dispatches exactly ten operations. Seven operations retain stateless
`pulp::tool-timeline` entry points; diff, undo, and redo retain an actual
`DocumentSession` in the bounded MCP process. The CLI has no `pulp seq diff`,
`undo`, or `redo` session verbs. The production store admits at most 32 sessions
and applies a 64 MiB aggregate admission charge equal to twice each session's
canonical JSON size plus its fixed history reservation. This deterministic charge
is a resource proxy, not a direct heap measurement. Each complete encoded MCP
result is independently capped at 64 MiB. The store evicts the oldest session
first when a new or changed session would exceed the count or aggregate charge.
With the default limits, each session reserves half of the aggregate charge
divided across the session cap for journal and undo history; a sufficiently long
uncheckpointed session can therefore refuse a later edit without changing its
state. Sessions are process-local and expire on eviction or server restart.
`diff` describes only the latest successful apply, undo, or redo transition and
returns its exact dirty set plus `before_revision` and `after_revision`; it is not
an arbitrary since-revision query. Do not copy their input schemas into
`pulp_mcp.cpp`; regenerate the artifact from the
timeline manifest and let the server consume it. The MCP render result can be
fed to `pulp_audio_compare` for an advisory before/after judgment when the
opt-in Audio Quality Lab tool is installed.

## Production mode and reproducibility class

Generated live-event batches use monotonic half-open tick spans plus playback
epoch. Keep producer-owned revisable staging physically separate from the
immutable committed SPSC ring; publish a complete batch with one release cursor.
Starvation advances the requested span, counts lag, emits zero events, and
requests an active-note flush so a missed note-off cannot hang. Seek/restart
must quiescently begin a new playback epoch, invalidating pending work. Validate
each UMP packet's word count from its message-type nibble. Deadline degradation
selects only a producer-supplied fallback policy; it never invents thinning.

`production_mode.hpp` declares `ProductionMode` (`Synchronous` / `Buffered`) and
`ReproducibilityClass` (`Deterministic` / `Tolerance` / `Materialized` /
`BestEffort`). Three rules that are easy to get wrong:

- **They are in-memory declarations with no schema registration**, so nothing
  round-trips them yet. Do not treat a declaration as persisted state or write a
  round-trip assertion against one until a content type actually carries it.
- **Lookahead is wall-clock milliseconds and is never latency.** Production cost
  is wall-clock, so a tick-declared lookahead silently shrinks as tempo rises.
  Nothing here may reach a latency or delay-compensation computation. A producer
  that needs *musical* context is declaring a compile-context subscription, not a
  lookahead.
- **Parsing fails closed.** `production_mode_from_name` /
  `reproducibility_class_from_name` return `nullopt` on an unknown name rather
  than defaulting, so a document from a newer build never silently reads as the
  strongest claim. Aggregate several classes with `weakest`, never by picking the
  first.

## Track mixer state

A `Track` owns a `TrackMixer` — a linear `gain_linear` and a stereo balance
`pan` — and `AutomationTarget` has a `TrackMixerTarget` alternative naming one of
those controls. Sends, mute, solo, and routing are deliberately absent.

Things that are easy to get wrong here:

- **A default mixer is written as absence.** `write_track` emits the `mixer`
  member only when the value differs from `TrackMixer{}`, so a document that
  never touched a fader stays byte-identical to its pre-mixer form and existing
  golden renders do not shift. The v6→v7 upgrade is therefore a version stamp
  and nothing else; the v7→v6 downgrade refuses outright when a `mixer` member
  is present, because a v6 track has nowhere to put a gain and dropping one
  silently changes how the document sounds.
- **A mixer target references no identity.** Validation must not demand a device
  placement for it (`validate_attached_automation`), the insert reduction must
  not prove one live, and the remap must return it unchanged. The
  one-lane-per-control rule still applies: duplicate detection compares a
  normalized key whose leading discriminator keeps a mixer target from colliding
  with a device target that shares its zeroed placement ID.
- **The mixer *value* is authored state a rebuild must carry.** It is not
  identity, so every path that reconstructs a `TrackInput` has to bring it
  across — see "An identity rewrite must copy the source input, not
  re-enumerate it" for why the remap dropped it silently and what shape stops
  the next field from going the same way.
- **Widening `AutomationTarget` reaches four consumers.** `serialize_encode.cpp`,
  `automation_document_internal.cpp`, `transaction_automation_internal.cpp`, and
  `core/interchange/src/census.cpp`. Dispatch through `AutomationTargetCases`
  (no generic fallback) at every one so a future alternative is a compile error
  rather than a `std::terminate` under `-fno-exceptions` or a silent omission
  from a census.
- **Adding a document field touches `structural_registry_validation.cpp` too.**
  That table is an exact field-by-field comparison against the registry, so a
  new field or a new registered type there fails serialization with
  `InvalidSchema` at path `/` until the table matches — an error that names
  nothing useful if you do not know to look.
- **Graph-hosted mixing is post-device.** `TimelineGraphPlaybackBinding` owns a
  stable mixer node per track. A hosted device chain with a nontransparent mixer
  must provide `post_device_audio_source` and
  `post_mixer_audio_destination`; the binding transactionally replaces that
  exact direct audio edge and restores it when the route or binding goes away.
  Program adoption fails closed if a hosted mixer becomes nontransparent
  without that route. Keep stopped automation parked, and evaluate precise
  host-mapped ticks without round-tripping through integer samples.

## A view rung consumes the editor kernel; it never owns a projection

`core/timeline_view` sits above `core/timeline_editor` and is the **first production consumer** of
that kernel — everything else referencing it is build files, the floor script, docs and tests.

**The convention that keeps it composable: a view takes resolved scalars, never a projection
object.** The arranger is handed an origin tick and a `px_per_tick`, not a `ViewportProjection`;
hit-testing takes a resolved `tolerance_px`, not a device-pixel-ratio to interpret. Two consequences
worth keeping:

- Two slices can build a viewport and a view **concurrently without racing**, because the view has
  no projection to diverge from. The shell feeds the projection's *output* into the seam.
- A view stays testable headlessly with plain numbers — no viewport fixture, no DPI plumbing.

**The consumption chain, which is what makes a view worth shipping rather than deferring:**

```
arranger view -> EditIntent -> SequencerUiHost::submit_intent
              -> lower_edit_intent -> timeline::Transaction -> reducer -> serialize_project
```

Every link existed before the arranger and **every link except the arranger was exercised only by
tests.** When adding to this stack, check the chain more than one level up: this codebase has a
long history of capabilities whose caller had no caller.

**Acceptance for an editing surface is a `serialize_project` round trip with values asserted** —
drive it with `simulate_click` / `simulate_drag` and assert the document's values survive, not that
the view's internal state changed.

### What "never a projection object" actually forbids

Read literally the rule above would bar `TickProjection` and `PitchProjection`, and the piano roll
consumes both. The line is not between a struct of floats and a class: it is between **resolved
values and viewport policy**. A type that owns zoom, scroll position, or a device-pixel ratio — and
therefore has to be kept in sync with the shell — is what a view must not hold. A `TickProjection`
is a visible tick range plus a pixel span, and a `PitchProjection` is an inclusive pitch range plus
a pixel span; neither decides anything, so handing one over is the same act as handing over
`origin_tick` + `px_per_tick`, with the arithmetic named once instead of twice.

Prefer consuming them. The arranger minted its own scalars and left both projections with no
consumer outside their own test, which is how a capability ends up shipped and unused.

### Note editing is a whole-content replacement, so the gesture shape is part of the design

A note edit lowers to `ReplaceNoteContent`, which carries both note arrays. That makes the gesture
bracket a design decision rather than a detail:

- **Commit on release** — one `GesturePhase::Single` intent per edit. The session closes it on
  admission and it becomes evictable, so a session takes any number of them.
- **A continuous `Begin`/`Update`/.../`End` drag** coalesces into a group that stays open, and an
  open group is evictable by nothing, so it dies partway through with `ConflictCode::UndoFull`.

`lower_note_edit_intent` therefore refuses a non-Single phase with
`NoteLoweringError::ContinuousGestureUnsupported` instead of emitting a transaction that fails
mid-gesture. Serving a continuous drag needs granular note commands, not a different lowering of
this one.

The lowering takes the clip's **current note array as a parameter** rather than looking a project
up. That keeps it pure for the same reason the clip lowering is pure, and it turns a stale view into
a named refusal (`ExpectedNoteMismatch`) instead of a reducer conflict the caller has to decode.

### Two traps when building a note-editing surface

- **A fresh note identity must be at or above `project.next_item_id()`.** `plan_identity_insert`
  rejects anything below it as `IdentityNotAvailable`, even for an id nothing has ever used. This is
  why an insert gesture takes its identity from a caller-supplied factory: a view genuinely cannot
  mint one, and the model enforces it rather than trusting the convention.
- **`TickProjection::tick_at` clamps to the visible end**, so a gesture that reads its result can
  never produce a tick past the viewport. A trailing-edge resize therefore stops exactly at the
  clip end and needs no bounds refusal, while a *move* still can leave the clip, because the grab
  offset is subtracted after the clamp. A bounds check written against the resize path alone is
  unreachable code that looks like safety.

## Scope boundary

This subsystem owns authored take/comp state, durable launch scenes, slots, and
follow actions, the durable `JournalSink` ordering seam, and native
`FileJournal`, but not package/container I/O, publication, realtime playback,
launch scheduling or automation delivery, nesting, device implementations,
routing, audio, format adapters, or UI. `core/project_package` owns durable
publication: no-replace content-addressed blobs and generic artifacts, plus
validated atomic replacement of `project.json` within a stable package root.
It also owns bounded cleanup of its private staging files without moving
canonical Timeline serialization or archive-format semantics out of their
existing owners. Package-wide recovery and reachability GC remain a follow-on
layer. Add other concerns in their owning modules instead of widening the
command and persistence core opportunistically.

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
Keep the 4k-scene/16k-slot launcher test proving bounded node creation, high
structural sharing, zero launcher allocation for annotation/context edits, and
zero sharing for an independently built equal launcher.
Keep `pulp-test-timeline-replay-golden` green: it applies real journaled gain,
fade, and note edits, replays from the checkpoint, and compares the resulting
audio/MIDI byte stream with both the committed snapshot and pinned fixture.
Also verify installed-header consumption, `-fno-exceptions -fno-rtti`, and that
timeline translation units do not include or link `pulp::format`, `pulp::host`,
or `pulp::view`.

`test/cmake/timeline_tests.cmake` is the manifest that registers the
`pulp-test-playback-*` suites alongside the document-model ones, so a new
playback-side suite is added there rather than in a new manifest — including
`pulp-test-playback-program-wire`, which covers the flat program wire the web
lane publishes generations over (`pulp/playback/program_wire.hpp`; the format's
own rules live in the `playback` skill). Sharing the manifest does not make the
program wire part of the document schema: it encodes a *compiled* program, and
a document-model change reaches it only through the compiler.

`test/cmake/sampler_runtime_tests.cmake` also registers sampler Heritage
runtime tests. That shared CMake inventory does not make profile JSON, capture
evidence, or sampler rendering part of the timeline document schema; keep those
contracts in `pulp::audio` unless a future version explicitly adds a document
reference type.

`test/cmake/timeline_tests.cmake` also owns the playback RT-safety suite
registrations, which carry a two-backend shape: a `$<BOOL:${UNIX}>` split
between `native_components/rt_intercept_test_support.cpp` and
`harness/rt_allocation_probe.cpp`, plus `pulp::native-components`,
`${CMAKE_DL_LIBS}`, and a `PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1` define.
Do not simplify that to a plain source list. Dropping the trap source while
keeping the define fails at link on `RtNoAllocScope`'s out-of-line constructor,
which is the intended behavior — the `playback` skill explains why that guard
exists and what still needs a hand-run control.

### A quota axis is added in five places, and the fuzz oracle is the one that makes it real

A new bounded collection needs a `DecodeLimits` field, a tightened value in
`DecodeLimits::web_defaults()`, a counter on `ProjectSnapshotCounts`, a
`governed_array` call in the preflight walker, and a `bounded_increment` in the
decoder. Stop after the decoder and the quota exists but is never differentially
checked. `test/fuzz/timeline_document_oracle.*` closes it in two rows: the
`StructureCensus` field plus one `quota_axes()` entry binding the measured count
to its declared ceiling. That pairing is what makes the tightening sweep able to
fail — the census is walked off the decoded model while the counts come from an
independent scan of the source bytes, so an axis present in one and absent from
the other is a quota nothing enforces.

### A mutation control on a fast target silently scores the previous mutation

Proving a test fails without its implementation means editing production code,
rebuilding, and re-running. On a small target the whole cycle finishes inside
one filesystem timestamp tick, so `cp` + `touch` of the restored source leaves
make believing the object is current — and the next control runs against the
*previous* mutation's binary. The failure mode is not "no result": it is a
confident wrong verdict, in both directions, and it is indistinguishable from a
real one. Two independent fixes are needed together:

- Delete the object (`find build -name '<file>.o' -delete`) rather than relying
  on timestamps, so the rebuild is unconditional.
- Assert `Building CXX object.*<file>` actually appears in the build log, and
  report a harness error rather than a verdict when it does not.

Add a no-op mutation that must read GREEN. A control set where every entry reads
RED has not shown that the instrument can report anything else.

### Fuzzing the untrusted-document surface

`pulp-test-timeline-document-fuzz` (registered under the `fuzz` ctest label)
drives `deserialize_project`, `peek_project_summary`, and
`deserialize_commands` over a mutated corpus seeded from
`test/fixtures/timeline/corpus.index`. The oracles live in
`test/fuzz/timeline_document_oracle.*` and are shared with the optional
libFuzzer target, so there is one definition of what a finding is.

Three things about it are easy to get wrong:

- **A rejection is not a finding.** Refusing malformed input is the surface
  working. The oracles that carry the quota contract are the *tightening
  sweep* — lower one declared ceiling below the document's own measured count
  and require a `LimitExceeded` that reports `actual > limit` — and *admission*,
  which rejects a document accepted over a ceiling that was in force. A harness
  whose only oracle is "did not crash" would pass with every structural quota
  unenforced.
- **The census must not come from the parser's own counters.** The decode path
  is measured by walking the returned `Project`; the scan path reports
  `ProjectSnapshotCounts` from the source bytes. Comparing the two is what
  detects a counter that is never incremented. Measuring a quota with the
  counter that enforces it confirms itself.
- **`max_clips` and its siblings are enforced twice** — once in
  `schema_json_preflight.cpp` and again in `serialize_project_decode.cpp`.
  Weakening only the preflight leaves `deserialize_project` rejecting and
  surfaces on `peek_project_summary` alone. Expect a finding to name one
  surface and not the other, and do not read that as the oracle being flaky.

Adding a structural ceiling to `DecodeLimits` means adding a row to
`quota_axes()`; the axis-count assertion fails otherwise, which is deliberate —
a ceiling with no axis is a quota the sweep silently never exercises.

`PULP_ENABLE_FUZZING=ON` builds the coverage-guided target and is **off by
default**: `-fsanitize=fuzzer` needs a compiler runtime Apple's clang does not
ship, so on macOS it requires a Homebrew LLVM. The configure step fails with
that cause named rather than at link time. The deterministic replay needs no
special toolchain and is the lane that runs on the PR path.

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

## `std::int64_t` is a different type on Linux and Darwin — never let CTAD see a bare `LL`

`std::int64_t` is **`long` on LP64 Linux** and **`long long` on Darwin**. A bare `0LL` literal is
`long long` on *both*. So a sibling written with `LL` alongside a `std::int64_t` expression **unifies
on macOS and diverges on Linux** — and `std::array` class-template argument deduction requires every
element to have the *same* type, so it fails on exactly one platform:

```cpp
// BROKEN on Linux, compiles on macOS. kTicksPerQuarter is std::int64_t.
const std::array windows{
    std::pair{kTicksPerQuarter / 4, 3 * kTicksPerQuarter / 4},  // pair<long,long> on Linux
    std::pair{0LL,                  3 * kTicksPerQuarter / 4},  // pair<long long,long>  <-- diverges
};
```

GCC reports `no matching function for call to 'array(std::pair<long int, long int>…)'` plus a
`no type named 'type' in 'struct std::enable_if<false, …>'` — which reads like a container problem
and is a *type-identity* problem.

**Fix by spelling the element type, not by correcting the literal.** Correcting `0LL` leaves CTAD
deducing, so the next sibling reintroduces it:

```cpp
using TrimWindow = std::pair<std::int64_t, std::int64_t>;
const std::array<TrimWindow, 3> windows{ TrimWindow{…}, TrimWindow{0, …}, TrimWindow{…} };
```

**Why this class is dangerous here specifically:** the required gate is **macOS**, and there is *no
macOS signal at all* — the code compiles cleanly on the platform that gates the merge. Only the
advisory Linux lane can see it.

**Reproduce it without a Linux toolchain** — a standalone TU with `using i64 = long;` in place of
`std::int64_t` gives the identical deduction error on Apple clang, so the diagnosis is
positive-controlled locally even though the confirming build is CI's.

Applies to any deduced aggregate over timebase quantities — ticks, frames, sample counts — since
those are `std::int64_t` throughout.

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

The committed interchange capability data is the authority for both SMF and
DAWproject admission and loss accounting. Each format JSON carries a stable,
contiguous `ordinal`; never infer `Format` ABI order from filenames. Reader and
writer implementations keep compile-time lists of the concepts they actually
support, so widening a generated table row without matching code must fail the
build. When the model gains a detectable semantic distinction, add a concept,
teach the census to name it, and declare both formats' behavior in the same
change before claiming a lossless path.

SMF has two deliberately separate export surfaces. `pulp::timeline::export_smf`
strictly checks the clip, event, and time-grid shapes it visits, but does not
census unrelated project state. `pulp::smf::writer()` is a non-callable,
format-bound adapter for `interchange::run_export()`: it serializes the project
snapshot owned by the plan, and only the central runner appends
`pulp-loss-manifest.json`. Never capture or pass a second `Project` into an
interchange writer, and never add a direct adapter invocation path. The
deprecated `dawproject::writer(project, options)` compatibility overload is the
sole exception at the source boundary: it deliberately ignores `project` and
delegates to `writer(options)`. Only the interchange adapter is the
project-wide consent surface.

### An absent capability row is a decision the generator writes down — and the two directions differ

`capability_emit.py` materializes a closed world, so a concept a format's JSON
omits still gets a row. It is not a hole, and it does not fail the build. But
the two directions are **not** symmetric, and only one of them is safe to leave
implicit:

- **Absent export row** → `ExportLevel::Drop`, `LossClass::Dropped`, and a
  generated sentence, `"<display name> declares no support for <concept id>"`.
  Truthful and specific enough to ship. `clip.media-window` on SMF is the
  committed example.
- **Absent import row** → `ImportLevel::None` with an **empty** `refusal`
  string. There is no generated fallback, so the refusal names nothing.

So write the import row even when the level equals the default; the export row
is a refinement of an already-true sentence rather than a correction of a false
one. What is genuinely broken without any row at all is a concept that does not
*exist*: `LossManifest` is keyed by `Concept`, so a construct with no id cannot
appear in a manifest at any level, and the export reports a clean bill.

### A capability table declares the ADAPTER, not the FORMAT

`smf.json` describes Pulp's bounded Standard MIDI File subset — its reader and
writer carry exactly the concepts listed in `smf_import.cpp` / `smf_export.cpp`
— not what a `.mid` file could theoretically hold. The distinction is invisible
until it bites: a Standard MIDI File carries control change natively, and Pulp's
writer does not, so a format-worded loss sentence ("Standard MIDI Files have no
…") would be **false** where the existing rows' phrasing is fine. When the format
can carry something the adapter cannot, word the loss after the writer.

### Adding a concept regenerates FOUR artifacts, and the fourth is outside `core/interchange`

`concepts.json` is append-only and order-significant because position fixes the
generated enum ordinal. Three artifacts come from `capability_emit.py`, gated
separately in `interchange_tests.cmake` — run each individually and unpiped,
never as one `ctest` sweep whose exit code you read through a pipe:

```
python3 core/interchange/tools/capability_emit.py --emit concepts > core/interchange/include/pulp/interchange/generated/concepts.hpp
python3 core/interchange/tools/capability_emit.py --emit tables   > core/interchange/include/pulp/interchange/generated/capability_tables.hpp
python3 core/interchange/tools/capability_emit.py --emit docs     > docs/reference/interchange-matrix.md
```

**The fourth has a different generator, in a different module, and it is the one
that breaks behavior rather than just drifting.** `core/timeline/tools/schema_mcp_emit.py`
also reads `capabilities/concepts.json`, and projects the vocabulary into
`pulp_timeline_export`'s `accept_losses` enum and its `x-pulp-loss-concepts`
list. Consent is per exact concept id with no blanket override, so a concept
missing from that enum is a loss **no MCP client can ever accept** — every
export of a document using it is permanently unauthorizable through that path.
Regenerate it in the same change:

```
python3 core/timeline/tools/schema_mcp_emit.py --manifest core/timeline/schema/timeline_schema.json > core/timeline/schema/timeline_mcp_tools.json
```

Gated by `timeline-mcp-drift` **and** `timeline-mcp-selftest` — the selftest
fails too, with `committed artifact matches a fresh emission`. Neither is run by
`tools/scripts/gates.sh`, so a fully green `gates.sh` proves nothing here.
A grep scoped to `core/interchange/` will not find this generator.

There is also a fifth surface that is data rather than code: any
`test/fixtures/timeline/**/*.json.expect` whose document uses the new concept.
`pulp-fixture-runner --corpus test/fixtures/timeline --update` rewrites them,
and `timeline-fixture-corpus` fails with the concept name and observed value
until you do.

### Naming a new SMF loss takes TWO edits, and each half fails silently on its own

The interchange table only governs exports routed through `interchange::run_export`. The public
`pulp::timeline::export_smf()` entry point is a *separate* surface that fails closed on shapes it
cannot carry, and it does not consult the capability table at all. So a concept the table declares
`drop` is still **silently discarded** by the raw API until you also teach `build_note_track` to
refuse it. Adding a concept without that leaves an SDK caller receiving a successful `.mid` with the
content gone — no error, no manifest.

The seam is `SmfExportLossPolicy` in `core/smf/src/smf_export_internal.hpp`. Both edits are
required, and they defend opposite failure modes:

1. **`smf_export.cpp`** — refuse when the content is present *and* the policy flag is unset. This is
   what makes the raw entry point fail closed.
2. **`smf_interchange.cpp`** — set that flag from `loses(plan, Concept::X)`. This is what lets the
   adapter proceed *after* the loss is accepted by exact concept id.

**Ship only half and the tests can still be green.** Verified by mutation: hardcoding the
`smf_interchange.cpp` flag to `false` — so consent can never clear the refusal and a lane-bearing
export becomes impossible through *every* path — left the whole SMF interchange suite passing,
because every existing test used a project without the new content. A refusal test alone does not
cover the clearing half; you need one export that **succeeds with the loss accepted** and asserts
the surviving content, or the second edit is unverified.

### A drift gate whose emit-cmd is a TARGET fails when that target is merely unbuilt

`timeline-schema-drift`'s `--emit-cmd` is `$<TARGET_FILE:pulp-timeline-schema-emit>`,
not a script. In a build directory where you only built the targets you needed,
that binary does not exist and the gate fails **with no diff and no useful
message** — indistinguishable from real drift, and easy to misattribute to
whatever you just changed. Build the emit target before believing it:

```
cmake --build build --target pulp-timeline-schema-emit
```

The interchange gates do not have this failure mode; their emit-cmds are Python.

### The fixture corpus manifest is count-only, so it cannot see owner identity

Measured, not assumed. Changing the census to record a lane concept against the
**clip** id instead of the **lane** id leaves the count unchanged, so
`timeline-fixture-corpus` passes (exit 0) while a unit test asserting
`owners(...)[0]` fails (exit 42). A census row that records per-item evidence
therefore needs a unit test asserting the owner *values*; the corpus fixture
alone will not defend it, and `contains(...)` or a bare count will not either.

## Asset confinement is two layers, and they are not redundant

A `PackageRelative` asset locator is checked twice, by checks with different
powers. Do not remove either on the grounds that the other covers it.

**Model (`core/timeline/src/model.cpp`, `valid_locator`)** — lexical only.
`Project::create` refuses a hint that is absolute or contains `..`, so an
escaping document cannot be constructed in memory, let alone serialized. This is
the layer that makes a bad locator unrepresentable rather than merely unusable.

**Loader (`tools/timeline/src/timeline_project_loader.cpp`,
`resolve_package_relative_asset`)** — resolves. It runs `fs::canonical` and then
`path_is_beneath(canonical_base, candidate)`.

The case that shows why both exist: a hint like `media/link.wav` that is
lexically spotless but whose target is a **symlink out of the package**. The
model has no basis to refuse it — nothing about the string is wrong. Only
canonicalization sees where it actually lands. Conversely the loader cannot help
with a document that was never loaded through it, which is why the model check is
not just belt-and-braces.

When testing this, note that hostile documents can no longer be *built* through
the API — the model refuses them. Author them by splicing the hint into
serialized JSON, which is also more faithful: the constructor never runs on bytes
someone else wrote. A refusal on that path surfaces as `"stage":"open"`, not
`"stage":"render"`.

## Widening the authoring surface can make a document unplayable

A model field, a command, or a schema property is not finished when it
round-trips. The playback compiler refuses several constructs this module lets a
user author — expression lanes on a clip, and six combinations reachable by
composing `set_clip_sequence_ref` with `set_clip_playback_properties`,
`set_track_mixer`, `set_track_freeze`, or the take, automation and device-chain
surfaces. Those documents save, reload, copy and round-trip; only playback says
no, and nothing at authoring time warns anyone.

`tools/scripts/negative_capability_check.py` (ctest
`playback-negative-capability`) is what makes that cost visible. It cross-refers
every refusal-shaped `CompileErrorCode` raise against this module's public
headers and `core/timeline/schema/timeline_schema.json`, and requires an owner
and a written reason for each refusal it can reach from here. The gate is
registered in `test/cmake/timeline_tests.cmake` beside the engine dependency
floor.

**When adding to the authoring surface, check the compiler consumes it** —
`core/playback/src/program_compiler.cpp` and
`core/playback/src/sequence_content_lowerer.cpp` are where a new field either
lowers or gets refused. Shipping the model half alone is how a negative
capability arrives. The check only sees refusals that name a code, so a field
the compiler silently drops or clamps passes it: that case needs a test, not a
gate.

## New public API in `core/timeline/include` needs a contract or docs fail

`tools/build-api-docs.sh` runs `tools/scripts/timeline_api_docs_check.py` over
the Doxygen XML and **exits non-zero** for any public callable without an API
contract:

```
model.hpp:768: error: public callable lacks an API contract:
  pulp::timeline::SequenceCompileStructureToken::valid
```

It is a real gate, not advisory, and it fires on `pull_request` via
`docs-material.yml`'s `core/**/include/**` path filter. Add the contract in the
same commit as the API — a one-line `///` summary is not enough if the callable
takes parameters or returns something whose meaning is not obvious from its type.

Run it locally before pushing a header change; it needs Doxygen but nothing else:

```bash
tools/build-api-docs.sh     # exits 1 and names the symbol
```

**A local pass does not guarantee CI passes.** CI installs ubuntu's Doxygen
(1.9.8) while a dev Mac usually has a much newer Homebrew build (1.17). They do
not agree on every diagnostic. The one that has already bitten:

```
schema_json.hpp:221: error: argument 'json' of command @param is not found in
  the argument list of pulp::timeline::ParsedJson::parse_json(std::string_view,
  const DecodeLimits &)
```

Note the scope in that message — `ParsedJson::parse_json`, not the free
function. Doxygen attached the free function's `@param` block to the **friend
declaration** inside the class, and that declaration had *unnamed* parameters,
so the names were unresolvable. 1.17 does not error; 1.9.8 does.

The fix is to name the parameters in the friend declaration so it matches the
definition. The general rule: if a documented free function is also declared
`friend` somewhere, both declarations need parameter names.

`build-api-docs.sh` now prints the local Doxygen version for exactly this
reason — when CI is red and local is green, check the versions before assuming
the tree differs.

## A wait for a published route must keep republishing it

`test_timeline_graph_binding_publication.cpp` drives `binding.prepare()` on the
control thread while an audio thread renders, and asserts the audio thread
observed **both** routes (`one_blocks > 0` and `two_blocks > 0`). Two traps
compound here.

First, the reprepare loop is an ordering budget that orders nothing: it can run
to completion before the audio thread is ever scheduled, leaving both counters
at zero.

Second — and this is the one a re-poll cannot fix — the loop **alternates**
routes and ends on `route_one`. A wait that only spins re-reading `two_blocks`
can never succeed, because `route_two` is never published again. The wait has to
keep alternating, not just keep looking.

Bound it, and make the pump cheap. Tearing down reprepared bindings costs
superlinearly in their count: a full-rate 10s wait spent 10s waiting and then
~30s in teardown (6663 reprepares), long enough to present as a CTest timeout
rather than the failure it is. A 1ms pump sleep with a 2s deadline holds the
whole failing run under 3s.
