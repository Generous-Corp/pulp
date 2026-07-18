---
name: timeline
description: Pulp immutable timeline document model, typed clip commands, atomic transactions, bounded journal, and inverse-command undo.
---

# Timeline document model

Use this skill when changing `core/timeline` document types or their construction
invariants.

## Contracts

- `Project`, `Sequence`, `Track`, and `Clip` are immutable snapshots. Validate
  once in their `create()` factories; reads must not repair, sort, or rebuild
  indexes.
- Every owned identity excludes zero and `UINT64_MAX`; the latter is the
  allocator's explicit exhausted sentinel. A project may store that sentinel
  after owning `UINT64_MAX - 1`; otherwise `next_item_id` is strictly larger
  than every owned ID. Allocation is monotonic and IDs are never reused.
- Tracks are sparse non-overlapping lanes. Their canonical clip order is
  `(anchor, start, ItemId)`. Timeline and ID indexes are persistent AVL trees;
  `replace_clip()` path-copies only search paths and shares untouched subtrees.
- `ClipTimeAnchor::Musical` follows tempo in ticks. `Absolute` uses
  `SamplePosition`, an integer sample count, and a normalized `RationalRate`,
  remaining fixed as tempo changes. Phase 1 rejects mixed anchors within one
  Track until a context-owned tempo/rate projection can compare them safely.
- `NoteContent` is a flat POD array sorted by `(start, ItemId)`. Note durations
  are positive, pitch is MIDI 0-127, and channel is 0-15.
- `MediaRef` ranges are checked locally for overflow and against their asset at
  project construction.
- Project and subtree remapping are two-pass: allocate all owned IDs first, then
  rebuild the snapshot and fix references. `MediaRef::asset_id` is external to
  Clip/Track/Sequence remaps and is translated by `ExternalIdFixup`; failure is
  atomic and does not advance the caller's allocator. Preflight the complete
  owned subtree for duplicate IDs before allocating; this includes parent IDs,
  cross-track collisions, clips, and note events.
- Fallible public APIs return `pulp::runtime::Result`; do not throw.

## Editing contracts

- `InsertClip`, `RemoveClip`, `MoveClip`, and `SetNoteVelocity` are the bounded
  Phase-1 mutation vocabulary. `reduce_transaction()` is pure: it returns a new
  snapshot, exact canonical dirty set, and reverse-ordered inverse commands.
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

This slice does not own persistence/schema codecs, a durable `JournalSink`,
playback, automation, launch slots, takes, nesting, devices, routing, audio,
format adapters, or UI. Add those in their scheduled slices instead of widening
the command core opportunistically.

## Validation

Build and run `pulp-test-timeline-model` plus the commands, transactions,
journal, and undo suites in Release and UBSan configurations.
Keep the 10k-clip edit test proving bounded node creation, subtree sharing, and
reclamation; a vector rebuild is not an acceptable persistent-index substitute.
Also verify installed-header consumption and that timeline translation units do
not include or link `pulp::format`, `pulp::host`, or `pulp::view`.
