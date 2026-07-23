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
- `ItemLocation` ownership is exactly `(kind, parent_id)`, where `parent_id` is
  the immediate owner. `sequence_id`, `track_id`, and `clip_id` are retained
  ancestor-navigation caches and must not become additional ownership keys.
  Add new item kinds by extending `immediate_parent_id()` rather than adding a
  new owner-specific field. Legacy identity records without `parent_id` derive
  it from their validated navigation fields during decode.
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
  `MoveClip`, `SetNoteVelocity`, `SetClipPlaybackProperties`, `SetTempoMap`,
  `SetMeterMap`, `CreateAsset`, `RemoveAsset`, `InsertTakeLane`,
  `RemoveTakeLane`, `InsertTake`, `RemoveTake`, `SetRecordArm`,
  `SetActiveTakeLane`, and `SetTakeComp` are the bounded mutation
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
- Journal mutation and tombstone restoration are session-internal. Public
  `reduce_transaction()` never revives tombstones, and replay rejects a
  checkpoint snapshot/revision mismatch or cross-entry writer-ID reuse.
- Undo and redo submit fresh ordinary transactions. They append to the journal;
  they do not delete or rewrite history.

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

## Scope boundary

This subsystem does not own a durable `JournalSink`, package/container I/O,
publication, playback or automation delivery, launch
slots, takes, nesting, device implementations, routing, audio, format adapters,
or UI. Add those in their owning modules instead of widening the command and
persistence core opportunistically.

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

## Foreign-format import (interop)

`import_dawproject_xml` (`core/timeline/import/dawproject_import.cpp`) builds a
`Project` from a DAWproject `project.xml` for a documented linear subset
(single tempo/meter, flat tracks, beats-timed `<Notes>`/`<Audio>` clips). It
consumes only the model's public construction API — a pure consumer, it never
changes the model layout. Gotchas for extending it or adding another importer:

- **It lives in `import/`, not `src/`, on purpose.** The web
  timeline-source-closure gate sweeps every `core/timeline/src/*.cpp` into the
  WAM/WebCLAP wasm DSP lanes, and those lanes do NOT link `pulp-runtime`. The
  importer needs pugixml (a runtime internal), so putting it under `src/` would
  drag pugixml into the wasm plugin. Keep foreign-format / heavy-dependency
  interop code out of `src/`.
- **The whole library is `-fno-exceptions -fno-rtti`.** pugixml's DOM API
  compiles clean under those flags; its XPath API does not (it throws). Parse
  with DOM traversal (`node.child()/children()/attribute()`), never
  `select_nodes`. The `runtime::XmlDocument` wrapper only exposes XPath-string
  scraping, too weak to correlate a clip with its parent lane's `track` — use
  pugixml directly (add `external/pugixml` as a PRIVATE include; its object code
  already ships inside `pulp::runtime`).
- **Fail closed, never silent-drop.** A clip/track/note the parser cannot map
  must produce a descriptive error, not a skipped element. Out-of-subset content
  (`<Warps>`, nested group tracks, `timeUnit="seconds"`, unknown timelines) is
  rejected explicitly. When iterating `children()`, skip non-element nodes
  before matching tag names.
- **Provisional asset identity.** DAWproject references audio by package path,
  and the bytes are inside the (not-yet-unzipped) container, so the durable
  `ContentHash` is derived from the path via `runtime::sha256_hex` and the path
  is preserved as a `PackageRelative` `AssetLocator` hint. Unzipping the
  container to hash real bytes, and the `pulp import` CLI verb, are follow-ups.
