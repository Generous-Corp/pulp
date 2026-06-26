# Inspect Module Map

This is the ownership map for shrinking the Pulp inspector surface under
`inspect/`. It documents the current seams, target extraction modules, and
invariants that must survive future refactors. It is not a claim that the split
has already happened.

The inspector already builds as a dedicated `pulp-inspect` target from
`inspect/CMakeLists.txt` and exposes the `pulp::inspect` alias. That umbrella
target links view/render/state/events/audio, but several inspector translation
units are pure protocol or CPU-side helpers. The strongest current seam is to
make those low-dependency leaves explicit before taking on riskier overlay or
domain-handler splits.

The largest source hotspot is still `InspectorOverlay`: it concentrates canvas
tool state, text editing, reflow gestures, tweak reconciliation, paint panels,
zoom, pass viewer, eyedropper, and source-jump entry points behind a large
public header. Treat the overlay as one shared object spread across companion
translation units, not as a set of independent libraries. `DomainHandler` is
the integration hub and should split late because it depends on almost every
inspector surface.

Future inspector extraction PRs should preserve the contracts below or update
this file, CMake wiring, tests, and any newly added hotspot ceilings in the same
change. `hotspot_size_guard.json` tracks the largest current inspect hotspots
and watches newly added `inspect/**` files for large-file warnings.

## Dependency Tiers

Classify extraction candidates by their minimum dependency, not by the current
umbrella target link. The goal is to move code without accidentally dragging
GPU/window-host requirements into pure inspector tests.

| Tier | Current modules | Dependency shape | Extraction signal |
| --- | --- | --- | --- |
| Tier 0: CPU-only leaves | `protocol`, `tweak_store`, `editor_url`, `console_capture`, `audio_inspector`, `state_inspector` | Choc/standard library plus at most one core subsystem such as audio or state; no overlay paint, render pass, or window host dependency. | Safest first. Can become a lower-level inspect support target or direct test source set. |
| Tier 1: protocol-coupled view helpers | `source_jump`, `motion_inspector`, `motion_scrubber` | Protocol plus focused view/motion/source-location dependencies. | Extract after Tier 0 helpers and keep launch/routing guards adjacent to their implementations. |
| Tier 2: overlay/window cluster | `inspector_overlay*`, `inspector_window` | Shared `InspectorOverlay` state, paint, view tree mutation, render/readback hooks, and floating window UI. | Higher-risk. Split only as companion `.cpp` files or after public/private API seams are clear. |
| Tier 3: integration hubs | `domain_handler`, `inspector_server` | Dispatch/transport over most lower tiers. | Extract last. Preserve optional-source error contracts and transport boundaries. |

`test/CMakeLists.txt:4568` already compiles CPU-only domain helper sources
directly without linking the full `pulp::inspect` target. That currently covers
`audio_inspector`, `console_capture`, `editor_url`, and `state_inspector`.
Future Tier 0/1 splits should extend that benefit to remaining pure helpers
instead of hiding them behind the GPU-gated umbrella target.

## Current Source Map

| Region | Current evidence | Owns today | Extraction owner |
| --- | --- | --- | --- |
| Build target | `inspect/CMakeLists.txt:1`, `inspect/CMakeLists.txt:10` | The `pulp-inspect` library, public include directory, and current source list. | Keep as the target manifest. Every moved or added `.cpp` must be added here in the same PR. |
| Overlay facade and host hooks | `inspect/include/pulp/inspect/inspector_overlay.hpp:34`, `inspect/src/inspector_overlay.cpp:110`, `inspect/src/inspector_overlay.cpp:1461`, `inspect/src/inspector_overlay.cpp:1748` | Overlay lifecycle, root hooks, active tool switching, high-level keyboard and mouse dispatch, selected/hovered tree state, and source-jump entry. | Keep `InspectorOverlay` as the facade; move feature state machines into focused overlay companion files. |
| Overlay paint panels | `inspect/src/inspector_overlay_paint.cpp:33`, `inspect/src/inspector_overlay_paint.cpp:534`, `inspect/src/inspector_overlay_paint.cpp:1031`, `inspect/src/inspector_overlay_paint.cpp:1440` | Canvas drawing for highlights, panel sections, tweak rows, reconcile tab, atlas tab, drift drawer, text edit overlay, drag/drop affordances, and same-frame hit row caches. | Keep paint-only drawing here. Do not add view mutation, disk writes, or protocol routing. |
| Field edit companion | `inspect/src/inspector_overlay_field_edit.cpp:43`, `inspect/src/inspector_overlay_field_edit.cpp:95`, `inspect/src/inspector_overlay_field_edit.cpp:184` | Property-field hit testing, edit-buffer read/write, commit/cancel, and field-edit key handling. | Preserve as a separate overlay companion. Do not mix with the text tool caret/selection editor. |
| Zoom companion | `inspect/src/inspector_overlay_zoom.cpp:35`, `inspect/src/inspector_overlay_zoom.cpp:76`, `inspect/src/inspector_overlay_zoom.cpp:110` | Zoom activation, zoom factor, pixel sampling, and zoom panel paint. | Preserve as a separate overlay companion. |
| Pass viewer companion | `inspect/src/inspector_overlay_pass_viewer.cpp:57`, `inspect/src/inspector_overlay_pass_viewer.cpp:103`, `inspect/src/inspector_overlay_pass_viewer.cpp:133` | Render-pass frame capture, pass attribution, and pass-attribution paint. | Preserve as a separate overlay companion. |
| Text tool state | `inspect/src/inspector_overlay.cpp:443`, `inspect/src/inspector_overlay.cpp:494`, `inspect/src/inspector_overlay.cpp:555`, `inspect/src/inspector_overlay.cpp:594`, `inspect/src/inspector_overlay.cpp:651` | Inline text editing, UTF-8 caret movement, selection, clipboard, insert/delete, commit/cancel, and target reachability checks. | `inspector_overlay_text_edit.{hpp,cpp}` or a `.cpp` companion that implements private `InspectorOverlay` methods. |
| Gesture, drag, and reflow state | `inspect/src/inspector_overlay.cpp:775`, `inspect/src/inspector_overlay.cpp:998`, `inspect/src/inspector_overlay.cpp:1121`, `inspect/src/inspector_overlay.cpp:1188`, `inspect/src/inspector_overlay.cpp:1320` | Layout/tweak snapshots, drag handle hit testing, cursor affordances, absolute move, grid/body drop targets, reflow commit, reparenting, and undo seeding. | `inspector_overlay_gestures.{hpp,cpp}` or a `.cpp` companion that keeps the facade event entry points thin. |
| Tweak panel and reconciliation state | `inspect/src/inspector_overlay.cpp:298`, `inspect/src/inspector_overlay.cpp:827`, `inspect/src/inspector_overlay.cpp:874`, `inspect/src/inspector_overlay.cpp:912`, `inspect/src/inspector_overlay_paint.cpp:1031` | Emitting tweaks from selection, snapshotting tweak IDs, drift/reconcile reports, and tweak-panel row action state used by painting. | `inspector_overlay_tweaks.{hpp,cpp}` for overlay state only. Persistent schema and disk behavior stay in `TweakStore`. |
| Eyedropper state | `inspect/src/inspector_overlay.cpp:233`, `inspect/src/inspector_overlay.cpp:238`, `inspect/src/inspector_overlay.cpp:249`, `inspect/src/inspector_overlay.cpp:268`, `inspect/src/inspector_overlay_paint.cpp:76` | Eyedropper activation, color resolution, sampling, apply-to-selection, and cursor paint. | `inspector_overlay_eyedropper.{hpp,cpp}` plus existing paint helper. |
| Inspector window | `inspect/include/pulp/inspect/inspector_window.hpp:73`, `inspect/src/inspector_window.cpp:176`, `inspect/src/inspector_window.cpp:192` | Floating inspector window tabs, tree/property refresh, console/performance/state rendering, selection reflection, and wiring annotations. | Keep separate from canvas overlay. Do not route canvas overlay paint or gesture state through the floating window. |
| Protocol constants and codec | `inspect/include/pulp/inspect/protocol.hpp:10`, `inspect/include/pulp/inspect/protocol.hpp:35` | Inspector message encode/decode and domain method names. | Keep centralized until a domain split proves duplicated names are stable; domain modules include it, not redefine strings. |
| Domain dispatch | `inspect/include/pulp/inspect/domain_handler.hpp:23`, `inspect/src/domain_handler.cpp:45`, `inspect/src/domain_handler.cpp:69`, `inspect/src/domain_handler.cpp:485`, `inspect/src/domain_handler.cpp:774` | Protocol request dispatch across motion, inspector, DOM, CSS, performance, state, console, runtime, audio, capture, and live-constant domains. | Keep `DomainHandler::handle()` as the facade; split per-domain helpers only after preserving missing-data error contracts. |
| Tweak store | `inspect/include/pulp/inspect/tweak_store.hpp:60`, `inspect/src/tweak_store.cpp:97`, `inspect/src/tweak_store.cpp:350`, `inspect/src/tweak_store.cpp:619`, `inspect/src/tweak_store.cpp:731` | Tweak record schema, batch application, bypass/lock state, JSON load/save, atomic writes, drift diff, and auto-save. | Already its own module. Do not duplicate schema, disk path, bypass normalization, lock preservation, or atomic-write behavior in overlay/domain code. |
| Editor URL | `inspect/src/editor_url.cpp`, `test/test_editor_url.cpp:60` | Editor URL template validation, environment/config/default precedence, and URL formatting. | Keep URL policy centralized so overlay hotkeys and protocol calls share the same config behavior. |
| Source jump | `inspect/include/pulp/inspect/source_jump.hpp:37`, `inspect/src/source_jump.cpp:52` | Source-location resolution, dry-run/default behavior, and guarded external launch. | Keep launch guards welded to the spawn path. Overlay and domains request jumps through this surface. |
| Motion inspection | `inspect/include/pulp/inspect/motion_inspector.hpp:22`, `inspect/include/pulp/inspect/motion_scrubber.hpp:37`, `inspect/src/motion_inspector.cpp`, `inspect/src/motion_scrubber.cpp` | Live motion trace capture, scrubber fixture playback, scrubber sinks, and motion protocol ownership checks. | Already split. Domain routing must respect `MotionScrubber::owns_method()` and scrubber sink concurrency invariants instead of reimplementing method-name checks. |
| Audio inspection | `inspect/include/pulp/inspect/audio_inspector.hpp:58`, `inspect/src/audio_inspector.cpp` | Audio telemetry snapshots, callback timing, level reporting, MIDI log, underruns, and runtime telemetry. | Already split. Domain code may serialize telemetry but should not own audio runtime state. |
| State and console helpers | `inspect/src/state_inspector.cpp`, `inspect/src/console_capture.cpp`, `test/CMakeLists.txt:4568` | State snapshot helper behavior and console capture buffers used by domains and windows. | Keep CPU-only and direct-testable. Do not force these helpers through overlay/window dependencies. |
| Inspector server | `inspect/include/pulp/inspect/inspector_server.hpp`, `inspect/src/inspector_server.cpp` | Inspector transport server and sink fanout. | Keep transport isolated from overlay feature logic and domain payload construction. |

## Target Overlay Modules

Extract overlay code in small, behavior-preserving moves. Prefer companion
translation units implementing private `InspectorOverlay` methods before adding
new public headers. `inspector_overlay.hpp` is already part of the public
include surface, so reducing private declarations from that header is a later
and higher-risk step.

| Target module | Owns | Must not own |
| --- | --- | --- |
| `inspector_overlay_text_edit` | Text tool target resolution, caret/selection movement, UTF-8 word/home/end helpers, clipboard operations, insert/delete, commit/cancel, and stale-target clearing. | Property field editing, view paint, source file rewrites. |
| `inspector_overlay_gestures` | Drag handles, absolute move, resize, reflow drop-target resolution, reparent commit, cursor affordances, layout/tweak snapshots, and undo seeding. | Tweak JSON schema, generated source mutation, panel painting. |
| `inspector_overlay_tweaks` | Selection-to-tweak emission, prior-tweak snapshot/restore, drift/reconcile state, lock/bypass row actions, and in-memory panel selection state. | Persistent tweak storage, atomic disk writes, protocol string constants. |
| `inspector_overlay_eyedropper` | Eyedropper activation, color resolution, readback sampling, selected-property application, and sampled-color state. | Zoom loupe sampling state, image fixtures, or hard-coded design tokens. |
| `inspector_overlay_panels` | Non-paint panel hit tests and row/action selection helpers if they become too large for the paint file. | Canvas drawing, view mutation, or protocol dispatch. |
| `inspector_overlay_core` | Constructor, activation, root hooks, top-level key/mouse dispatch, selected/hovered view tracking, and thin delegation into feature companions. | Feature-specific state machines once extracted. |

Existing companions stay in place: `inspector_overlay_field_edit.cpp`,
`inspector_overlay_zoom.cpp`, `inspector_overlay_pass_viewer.cpp`, and
`inspector_overlay_paint.cpp`.

## Target Domain Modules

Split `DomainHandler` only after overlay extraction has stabilized enough that
domain helpers can call narrow overlay APIs. Keep `DomainHandler::handle()` as
the single public dispatch entry.

| Target module | Owns | Must not own |
| --- | --- | --- |
| `domain_handler_inspector` | Inspector domain methods, overlay actions, source jump, tweak-store commands, and inspector-specific errors. | DOM/CSS serialization or motion scrubber routing. |
| `domain_handler_dom_css` | DOM and CSS query/update payloads over the current inspected tree. | TweakStore persistence or overlay panel state. |
| `domain_handler_performance` | Dirty tracker, render stats, frame timing, and performance payload serialization. | State-store or audio telemetry ownership. |
| `domain_handler_state` | State inspector and state-store payloads. | Live constant editor routing. |
| `domain_handler_console_runtime` | Console capture and runtime evaluation-style protocol payloads. | Audio or motion telemetry. |
| `domain_handler_audio` | Audio telemetry serialization and missing-audio-source errors. | Audio runtime collection. |
| `domain_handler_live_constant` | Live-constant protocol methods and editor integration. | Generic CSS/property edit behavior. |
| `domain_handler_motion` | Routing between live motion inspector and scrubber fixture methods. | Manual method-name substring checks outside `MotionScrubber::owns_method()`. |

## Ownership Rules

- `InspectorOverlay` owns canvas overlay interaction state. It may delegate to
  feature companions, but external callers should still enter through the
  facade until a smaller public API is deliberately designed.
- Overlay companions are co-owned pieces of one object. They share selected view,
  root, tweak store, edit history, and top-level key/mouse dispatch state; do
  not treat them as independently reusable libraries.
- `handle_key_event()` ordering is part of the contract. Mid-edit text handling,
  field-edit handling, tool shortcuts, Escape cancellation, and tool reset must
  keep their current precedence.
- Overlay paint helpers may read overlay state and cache same-frame row hit
  rectangles. They must not mutate the inspected view tree, write tweak files,
  launch editors, or route protocol requests.
- Field editing and text editing are separate features. Field editing mutates
  property rows; text editing mutates the selected text view through caret and
  selection state.
- Text editing must never dereference stale view pointers. Keep
  `text_edit_target_reachable()`, anchor-based resolution, and clear-on-dangling
  behavior with the text-edit module.
- Gesture code must preserve explicit mouse phases and the legacy `is_down`
  convention used by current tests. Reflow and resize changes must preserve
  snapshot/restore behavior for undo.
- Reparent source updates are emitted as anchor-only source edit requests.
  The overlay must not write generated files directly or decide whether a file
  is source-locked.
- `TweakStore` owns persistent tweak schema, atomic writes, default paths,
  drift diffing, bypass, lock, and auto-save. Overlay/domain code may call it
  but must not duplicate its JSON shape or disk behavior.
- The tweak on-disk schema is a cross-language contract with importer-side tweak
  consumers. Keep schema version tolerance, bypass representation, optional
  locked/source keys, lock preservation, and batch auto-save behavior in
  `TweakStore`.
- `DomainHandler` owns protocol dispatch and absent-data error responses.
  Split domain helpers must preserve the current behavior when optional
  dependencies such as overlay, state inspector, console capture, audio
  telemetry, dirty tracker, or motion inspector are missing.
- `DomainHandler::set_config()` and `set_overlay()` push editor/source-jump
  configuration into dependent surfaces. Preserve that config push-down when
  extracting domains.
- Motion protocol routing must keep `MotionScrubber::owns_method()` as the
  scrubber/live-inspector boundary.
- Source jump launch behavior must keep dry-run/default handling and the
  launch guards used by CI and tests. Do not launch an editor from tests that
  expect URL construction only.
- CPU-only inspector helper tests must stay CPU-only. Do not make them link the
  full `pulp::inspect` target or require GPU/window-host setup.
- The floating `InspectorWindow` and the canvas `InspectorOverlay` are separate
  UI surfaces. Selection can be reflected between them, but paint hooks,
  gestures, and root-gated overlay state must not move into the window class.

## Current Validation Anchors

Keep these tests green after each extraction step. Add focused coverage next to
the existing behavioral tests when a helper becomes unit-testable.

| Surface | Current coverage |
| --- | --- |
| Inspect target source list | `inspect/CMakeLists.txt:1`, `inspect/CMakeLists.txt:10` |
| CLI inspector shellout | `test/CMakeLists.txt:498`, `test/test_cli_inspect_shellout.cpp:127` |
| CLI tweak shellout | `test/CMakeLists.txt:557`, `test/test_cli_tweaks_shellout.cpp:35` |
| Broad overlay/window/protocol behavior | `test/test_inspector.cpp:335`, `test/test_inspector.cpp:497`, `test/test_inspector.cpp:1907`, `test/test_inspector.cpp:3204`, `test/test_inspector.cpp:5783` |
| GPU pass viewer | `test/CMakeLists.txt:4596`, `test/test_inspector_gpu_passes.cpp:55` |
| Wiring annotations | `test/CMakeLists.txt:4602`, `test/test_inspector_wiring.cpp:56` |
| Source jump and editor URL | `test/CMakeLists.txt:4608`, `test/test_inspector_source_jump.cpp:103`, `test/test_editor_url.cpp:60` |
| Drift reconcile | `test/CMakeLists.txt:4614`, `test/test_inspector_drift_reconcile.cpp:61` |
| Atlas viewer | `test/CMakeLists.txt:4620`, `test/test_inspector_atlas_viewer.cpp:64` |
| Inspector server | `test/CMakeLists.txt:4627`, `test/test_inspector_server.cpp:184` |
| Domain handlers | `test/CMakeLists.txt:4639`, `test/test_inspector_domains.cpp:40`, `test/test_inspector_domains.cpp:137`, `test/test_inspector_domains.cpp:542` |
| Field edit | `test/CMakeLists.txt:4645`, `test/test_inspector_field_edit.cpp:91` |
| Eyedropper | `test/CMakeLists.txt:4651`, `test/test_inspector_eyedropper.cpp:98` |
| Tweak store schema, disk, diff, and protocol | `test/CMakeLists.txt:4657`, `test/test_tweak_store.cpp:105` |
| Editor URL/source jump protocol | `test/CMakeLists.txt:4663`, `test/test_editor_url.cpp:60` |
| Motion inspector and scrubber | `test/CMakeLists.txt:3939`, `test/test_motion_inspector.cpp:88`, `test/test_motion_scrubber.cpp:104` |
| CPU-only domain helpers | `test/CMakeLists.txt:4568`, `test/test_inspector_domain_helpers.cpp:21` |

## Extraction Order

1. Extract Tier 0 helpers first: `protocol`, `tweak_store`, `editor_url`,
   `console_capture`, `audio_inspector`, and `state_inspector`. The immediate
   new payoff is direct CPU-only coverage for `protocol` and `tweak_store`;
   `audio_inspector`, `console_capture`, `editor_url`, and `state_inspector`
   already have CPU-only helper coverage today.
2. Extract `source_jump` after `editor_url` and keep launch guards inside the
   launch function. Do not split URL formatting, dry-run/default behavior, and
   CI/test launch suppression into separate ownership surfaces.
3. Extract `motion_scrubber` and `motion_inspector` after protocol support is
   stable. Keep `MotionScrubber::owns_method()` adjacent to scrubber protocol
   constants and keep scrubber sink dispatch outside internal locks.
4. Leave the overlay cluster intact until low-dependency support modules are
   separated. When overlay work starts, move text-edit methods first because the
   stale-target and caret contracts are clearer than gesture/reflow behavior.
5. Move gesture/reflow methods next: drag handles, cursor affordances, snapshot
   helpers, reflow drop target resolution, reparent commit, and undo seeding.
6. Move eyedropper state after gesture extraction so color sampling stays
   separate from zoom and paint code.
7. Move tweak-panel state and reconciliation helpers after gesture extraction,
   while leaving persistent schema and disk behavior in `TweakStore`.
8. Keep `inspector_overlay.cpp` as the facade for activation, hook install,
   top-level key/mouse handling, and delegation to companions.
9. Split `DomainHandler` by domain only after the lower tiers and overlay
   methods used by inspector-domain requests have narrower names and test
   anchors. It is the integration hub, not the first extraction target.
10. Consider a later private-state reduction for `inspector_overlay.hpp` only
    after the companion `.cpp` files have proved stable. Do not combine that
    public-header cleanup with behavior extraction.
11. If a future PR registers inspect files in
    `tools/scripts/hotspot_size_guard.json`, lower the exact LOC ceiling in the
    same commit as any extraction that shrinks that tracked hotspot.

## Non-Goals

- Do not introduce a generic inspector framework layer just to move code. The
  current public contract is `pulp::inspect`, protocol messages, and the view
  hooks used by tests and hosts.
- Do not fold `TweakStore`, source jump, motion scrubber, audio telemetry, or
  inspector server code back into overlay/domain modules. Those are already
  separate ownership surfaces.
- Do not merge canvas overlay and floating inspector window responsibilities.
- Do not split `DomainHandler` by copying protocol constants into per-domain
  files. Use `protocol.hpp` until there is a deliberate protocol registry.
- Do not make CPU-only domain-helper tests depend on GPU/window-host setup as a
  side effect of moving implementation files.
