# Design-import framework comparison (2026-10-04)

Compared against the refactor plan in `planning/2026-10-04-design-import-refactor-GOAL.md`.

| Reference | Evidence at studied commit | Transfer decision |
|---|---|---|
| Octane `322162cb79e72a322783d8765c97ec8ab0947bbe` | `packages/octane/src/compiler/compile-universal.js` lowers child blocks and preserves keyed identity; its benchmark corpus measures compiled templates and keyed updates. | Adopted the identity and block-planning idea, using Pulp's existing `stable_anchor_id` rather than introducing a second key field or compiler. |
| Inferno `5c343f5f3` | `packages/inferno-create-element/src/patching.ts` and `patchKeyedChildren.spec.ts` keep keyed and non-keyed paths explicit; tests cover reorder, insert, remove, and mixed lists. | Adopted a fail-closed split: unique anchors use keyed matching; missing or duplicate anchors use a conservative positional plan and surface `ambiguous_keys`. |
| React Native Skia `e72c9647a` | `packages/skia/src/sksg/Reconciler.ts` turns commits into Skia pictures; `RNSkPictureRenderer::applyUpdatesTo` applies recorder values without rebuilding the picture. | Deferred retained picture ownership. Pulp's native materializer does not yet expose a safe recorder/picture lifetime seam, so adding one here would exceed a safe importer slice. |

## Adopted slice

`core/view/include/pulp/view/design_update.hpp` and `core/view/src/design_update.cpp`
add a pure `plan_design_child_updates()` function. It emits retained, moved,
inserted, and removed operations plus contiguous `DesignUpdateBlock` runs. The
planner is read-only and does not alter code generation or native materialization;
it is therefore safe to use as the next bridge between re-import and retained
views.

The tests cover keyed reorder plus insert/remove and the duplicate-key negative
control. Ambiguous siblings are replaced wholesale, so a future materializer
cannot accidentally retain a view by position. It can consume the blocks, while the current import
path remains byte-compatible.

## Measurements

Host: `Daniels-Mac-Studio-m3.local`, Darwin 27.0.0, arm64; Apple Clang
21.0.0; macOS SDK 27.0. Build type: Release. The governed target build
completed successfully (initial clean target: 1,118 Ninja steps; final
incremental rebuild after formatting: 40 steps). The resulting test executable
is 46,353,528 bytes with SHA-256
`6fcc4ac828e106475214059012b81091824bb6077aee53844eab751659eab11b`.

The two focused cases passed: 2/2, 0.25 seconds wall time. The required
negative control also passed: `confirm_failure.sh` broke the keyed comparison,
observed the reorder test fail, restored the source, and observed the test pass
again. The comparison itself found no evidence that a compiled-template
compiler or Skia retained recorder should be copied into Pulp before a measured
runtime seam exists.
