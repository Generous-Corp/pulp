# Timeline Plugin Proof

**Category**: validation
**Type**: silent instrument
**Path**: `examples/timeline-plugin-proof/`
**Format**: CLAP

## Summary

This example is the smallest loadable plugin that owns a Timeline
`DocumentSession`, exposes the `SequencerUiHost` editor seam through a native
`Processor::create_view()`, submits pointer-neutral `EditIntent` values, and
round trips the current `Project` through plugin-owned state.

The view is intentionally a ruler and playhead shell, not a piano roll. It proves
the plugin/session/editor ownership boundary without preempting the interactive
note editor that belongs above the existing editor kernel.

## What It Demonstrates

- A plugin-owned `Project`, `DocumentSession`, and `WriterToken`
- Single-step clip edits lowered through `timeline_editor::lower_edit_intent()`
- Lock-free audio-thread publication of `UiPlayhead` to a native view
- Canonical Timeline JSON stored through `serialize_plugin_state()`
- Transactional state restore: malformed input leaves the live project unchanged
- An inbound link-floor assertion that requires `format`, `timeline`, and
  `timeline_editor` in the measured CLAP closure

## Build and Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=ON
cmake --build build --target timeline-plugin-proof-test TimelinePluginProof_CLAP
ctest --test-dir build -R 'timeline plugin proof|clap-dlopen-TimelinePluginProof'
```

The focused tests verify session/view creation, intent submission, canonical
state round trip, and fail-closed malformed-state handling. The CMake configure
also writes the resolved closure to
`build/link-floor/TimelinePluginProof_CLAP.txt`.

## Scope

Audition is explicitly unsupported, gesture brackets are rejected, and the
processor emits silence. Those are visible boundaries, not implied features.
The example exists to prove integration and persistence before a richer editor
adds note gestures and audio routing.
