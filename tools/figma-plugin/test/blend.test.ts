// Layer blend-mode lowering for the Figma plugin lane (audit: Blend / opacity).
// Every Figma mode with a CSS mix-blend-mode equivalent lowers to the CSS
// keyword; the two modes CSS lacks (LINEAR_BURN / LINEAR_DODGE) raise
// `blend-unsupported` instead of approximating — a wrong blend paints
// confidently wrong pixels. An isolate group (explicit NORMAL on a container
// whose subtree blends) raises `group-isolation-approximated`. Kept in
// lockstep with the REST lane (tools/import-design/figma_rest_export.py +
// test/test_figma_rest_export.py), the .fig lane
// (tools/import-design/fig/scene.mjs), and the consumer table
// (core/view/src/design_ir_json.cpp::is_supported_blend_keyword).

import { test } from "node:test";
import assert from "node:assert/strict";

import {
  BLEND_IS_DEFAULT,
  FIGMA_BLEND_CSS,
  lowerLayerBlendMode,
  collectGroupIsolationDiagnostics,
} from "../src/extract-pure";
import type { ExtractedFigmaNode } from "../src/extract-model";

// The shared supported-blend table, spelled out so drift in any lane's set is
// a test failure, not a silent divergence.
const SUPPORTED = [
  "DARKEN", "MULTIPLY", "COLOR_BURN", "LIGHTEN", "SCREEN", "COLOR_DODGE",
  "OVERLAY", "SOFT_LIGHT", "HARD_LIGHT", "DIFFERENCE", "EXCLUSION",
  "HUE", "SATURATION", "COLOR", "LUMINOSITY",
];

test("the supported-blend table is exactly the 15 CSS-equivalent Figma modes", () => {
  assert.deepEqual([...FIGMA_BLEND_CSS].sort(), [...SUPPORTED].sort());
  assert.deepEqual([...BLEND_IS_DEFAULT].sort(), ["NORMAL", "PASS_THROUGH"]);
});

test("every supported mode lowers to its CSS keyword, no diagnostic", () => {
  for (const mode of SUPPORTED) {
    const out = lowerLayerBlendMode(mode);
    assert.equal(out.css, mode.toLowerCase().replace(/_/g, "-"), mode);
    assert.equal(out.diagnostic, undefined, mode);
  }
});

test("default modes lower to nothing and stay silent", () => {
  for (const mode of [undefined, "NORMAL", "PASS_THROUGH"]) {
    const out = lowerLayerBlendMode(mode);
    assert.equal(out.css, undefined, String(mode));
    assert.equal(out.diagnostic, undefined, String(mode));
  }
});

test("a mode CSS lacks raises blend-unsupported instead of approximating", () => {
  for (const mode of ["LINEAR_BURN", "LINEAR_DODGE", "FUTURE_MODE"]) {
    const out = lowerLayerBlendMode(mode);
    assert.equal(out.css, undefined, mode);
    assert.ok(out.diagnostic, mode);
    assert.equal(out.diagnostic.code, "blend-unsupported");
    assert.equal(out.diagnostic.severity, "warning");
    assert.equal(out.diagnostic.kind, "unsupported_property");
    assert.match(out.diagnostic.message, new RegExp(mode));
  }
});

// ── group isolation ────────────────────────────────────────────────────────

const node = (
  over: Partial<ExtractedFigmaNode> = {},
): ExtractedFigmaNode => ({
  type: "frame",
  figma_type: "FRAME",
  name: over.name ?? "n",
  figma_node_id: over.figma_node_id ?? "0:0",
  parent_id: null,
  z_order: 0,
  absolute_bounds: { x: 0, y: 0, w: 10, h: 10 },
  relative_transform: [[1, 0, 0], [0, 1, 0]],
  visible: true,
  locked: false,
  opacity: 1,
  blend_mode: "PASS_THROUGH",
  style: {},
  layout: {},
  children: [],
  ...over,
}) as ExtractedFigmaNode;

test("an isolate group with a blending descendant is diagnosed once", () => {
  // Explicit NORMAL on a container = Figma's "isolate"; a DEEP descendant
  // with a lowered blend mode means the missing isolation layer changes
  // pixels — the descendant blends against the full backdrop.
  const root = node({
    name: "isolate", figma_node_id: "1:1", blend_mode: "NORMAL",
    children: [node({
      name: "inner", figma_node_id: "1:2",
      children: [node({
        name: "noise", figma_node_id: "1:3", blend_mode: "MULTIPLY",
        style: { mix_blend_mode: "multiply" },
      })],
    })],
  });
  const out = collectGroupIsolationDiagnostics(root);
  assert.equal(out.length, 1);
  assert.equal(out[0].code, "group-isolation-approximated");
  assert.equal(out[0].severity, "warning");
  assert.equal(out[0].kind, "capture_partial");
  assert.equal(out[0].path, "1:1");
  assert.match(out[0].message, /isolate/);
});

test("pass-through containers and inert isolate groups stay silent", () => {
  // PASS_THROUGH with a blending child: Figma default = web default, no
  // isolation lost. Explicit NORMAL with a non-blending subtree: isolation
  // is a no-op. A leaf's own NORMAL (the leaf default) never fires.
  const passThrough = node({
    blend_mode: "PASS_THROUGH",
    children: [node({ blend_mode: "SCREEN", style: { mix_blend_mode: "screen" } })],
  });
  const inert = node({
    blend_mode: "NORMAL",
    children: [node({ blend_mode: "NORMAL" })],
  });
  const leaf = node({ blend_mode: "NORMAL" });
  assert.deepEqual(collectGroupIsolationDiagnostics(passThrough), []);
  assert.deepEqual(collectGroupIsolationDiagnostics(inert), []);
  assert.deepEqual(collectGroupIsolationDiagnostics(leaf), []);
});

test("a blending container needs no isolation diagnostic", () => {
  // A container with its own non-default blend mode: CSS mix-blend-mode
  // itself forms an isolated group, matching Figma's semantics exactly.
  const blendingGroup = node({
    blend_mode: "MULTIPLY", style: { mix_blend_mode: "multiply" },
    children: [node({ blend_mode: "SCREEN", style: { mix_blend_mode: "screen" } })],
  });
  assert.deepEqual(collectGroupIsolationDiagnostics(blendingGroup), []);
});
