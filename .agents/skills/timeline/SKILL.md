---
name: timeline
description: Pulp immutable timeline document model path-copy track indexes musical and absolute anchors media references sorted note content and scoped two-pass remapping.
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

## Scope boundary

The initial model does not own commands, undo, journals, persistence,
publication, playback, automation, launch slots, takes, nesting, devices,
routing, or UI. Add those in their scheduled slices instead of widening the
foundation types opportunistically.

## Validation

Build and run `pulp-test-timeline-model` in Release and UBSan configurations.
Keep the 10k-clip edit test proving bounded node creation, subtree sharing, and
reclamation; a vector rebuild is not an acceptable persistent-index substitute.
Also verify installed-header consumption and that timeline translation units do
not include or link `pulp::format`, `pulp::host`, or `pulp::view`.
