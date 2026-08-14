# Timeline Plugin Proof

**Category**: validation
**Type**: silent instrument
**Path**: `examples/timeline-plugin-proof/`
**Format**: CLAP

## Summary

This example is the smallest loadable plugin that owns a Timeline
`DocumentSession`, embeds a real `PianoRollView` through
`Processor::create_view()`, submits validated pointer-neutral note intents, and
round trips the current `Project` through plugin-owned state.

## What It Demonstrates

- A plugin-owned `Project`, `DocumentSession`, `WriterToken`, and MIDI clip
- Insert lowered as a closed `Single` edit through
  `timeline_editor::lower_note_edit_intent()`, while move and resize publish
  continuously through `EditGestureIdentityAllocator` and one writer-owned undo
  group
- A 400x300 host-visible root and piano roll that project the full two-quarter
  clip into the editor bounds; interaction tests enter through that root
- Two live piano-roll views rebound to each immutable snapshot after accepted or
  rejected submissions, undo, and state restore
- `Cancel` restores the pre-Begin note, and terminal phases or successful atomic
  state replacement discard the retained gesture provenance
- One pinned `DocumentView` supplies the note span and revision for each lowered
  submission, so a gesture never combines values from different snapshots
- Processor-first editor teardown detaches retained views before their host
  pointer can become stale
- Lock-free audio-thread publication of `UiPlayhead` through the editor host
- Canonical Timeline JSON stored through `serialize_plugin_state()`
- Transactional state restore: the prior four-quarter empty-clip state migrates
  to the two-quarter MIDI proof while preserving its clip start and all other
  authored project, sequence, and track state; malformed
  projects, other non-MIDI proof clips, and incompatible clip durations leave
  the live project and its undo/redo history unchanged
- An inbound link-floor assertion that requires `format`, `timeline`,
  `timeline_editor`, and `timeline_view` in the measured CLAP closure

## Build and Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=ON
cmake --build build --target timeline-plugin-proof-test TimelinePluginProof_CLAP
ctest --test-dir build -R 'timeline plugin proof|clap-dlopen-TimelinePluginProof'
```

The focused tests verify real continuous move and resize through the plugin's
`DocumentSession`, one-group undo/redo, cancellation, two-view convergence,
state-replacement provenance reset, legacy `Single` edits, canonical state round
trip, and fail-closed stale or incompatible input. The CMake configure also writes
the resolved closure to `build/link-floor/TimelinePluginProof_CLAP.txt`.

## Scope

Audition is explicitly unsupported and the processor emits silence. The example
proves editor/session integration and persistence; it does not claim a playback
engine or audio routing.
