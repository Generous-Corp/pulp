// Stroke extraction for the Figma plugin lane: uniform strokes keep the
// border shorthand, per-side weights (IndividualStrokesMixin, active when the
// four sides differ) lower to the discrete border_*_width fields, dashPattern
// maps to border_style "dashed" with the exact array preserved, and
// multi-paint / non-solid / oddly-aligned strokes are diagnosed or preserved,
// never silently flattened. Kept in lockstep with the REST lane
// (tools/import-design/figma_rest_export.py + test/test_figma_rest_export.py)
// and the .fig lane (tools/import-design/fig/scene.mjs).

import { test } from "node:test";
import assert from "node:assert/strict";

import { extractStrokeStyle } from "../src/extract-pure";

// extractStrokeStyle reads only stroke-cluster fields; the cast keeps each
// fixture to the fields under test rather than standing up a full SceneNode.
const sceneNode = (over: Record<string, unknown>) =>
  ({ name: "N", ...over }) as unknown as SceneNode;

const red = { type: "SOLID", visible: true, color: { r: 1, g: 0, b: 0 } };
const blue = { type: "SOLID", visible: true, color: { r: 0, g: 0, b: 1 } };
const gradient = { type: "GRADIENT_LINEAR", visible: true };

test("a uniform solid stroke keeps the border shorthand + discrete fields", () => {
  const out = extractStrokeStyle(sceneNode({ strokes: [red], strokeWeight: 2 }));
  assert.ok(out);
  assert.equal(out.style.border, "2px solid #ff0000");
  assert.equal(out.style.border_color, "#ff0000");
  assert.equal(out.style.border_width, 2);
  assert.equal(out.style.border_style, "solid");
  assert.ok(!("border_top_width" in out.style), "no per-side fields on a uniform stroke");
  assert.deepEqual(out.diagnostics, []);
  assert.equal(out.attributes, undefined);
});

test("per-side weights lower to border_*_width with the color per painted side", () => {
  const out = extractStrokeStyle(sceneNode({
    strokes: [red],
    // strokeWeight reads figma.mixed (a symbol) exactly when the sides differ.
    strokeWeight: Symbol("figma.mixed"),
    strokeTopWeight: 4, strokeRightWeight: 0, strokeBottomWeight: 1, strokeLeftWeight: 0,
  }));
  assert.ok(out);
  assert.equal(out.style.border_top_width, 4);
  assert.equal(out.style.border_right_width, 0, "explicit 0 side stays 0");
  assert.equal(out.style.border_bottom_width, 1);
  assert.equal(out.style.border_left_width, 0);
  assert.equal(out.style.border_color, "#ff0000");
  assert.equal(out.style.border_top_color, "#ff0000");
  assert.equal(out.style.border_bottom_color, "#ff0000");
  assert.ok(!("border_right_color" in out.style), "no color on an unpainted side");
  assert.ok(!("border" in out.style), "per-side widths replace the shorthand");
  assert.ok(!("border_width" in out.style));
});

test("four EQUAL side weights stay on the uniform path", () => {
  const out = extractStrokeStyle(sceneNode({
    strokes: [red], strokeWeight: 3,
    strokeTopWeight: 3, strokeRightWeight: 3, strokeBottomWeight: 3, strokeLeftWeight: 3,
  }));
  assert.ok(out);
  assert.equal(out.style.border, "3px solid #ff0000");
  assert.ok(!("border_top_width" in out.style));
});

test("a dashPattern maps to border_style dashed and preserves the exact array", () => {
  const out = extractStrokeStyle(sceneNode({
    strokes: [red], strokeWeight: 2, dashPattern: [4, 2],
  }));
  assert.ok(out);
  assert.equal(out.style.border, "2px dashed #ff0000");
  assert.equal(out.style.border_style, "dashed");
  assert.equal(out.attributes?.["figma:dash_pattern"], "4,2");
});

test("multi-paint and non-solid strokes flatten to the first solid WITH a diagnostic", () => {
  // Two solids: first wins, multi-paint-stroke says so.
  const two = extractStrokeStyle(sceneNode({ strokes: [red, blue], strokeWeight: 1 }));
  assert.ok(two);
  assert.equal(two.style.border_color, "#ff0000");
  assert.deepEqual(two.diagnostics.map((d) => d.code), ["multi-paint-stroke"]);
  assert.equal(two.diagnostics[0].kind, "capture_partial");

  // A gradient on top of a solid: flattened to the solid, diagnosed.
  const gradTop = extractStrokeStyle(sceneNode({ strokes: [gradient, red], strokeWeight: 1 }));
  assert.ok(gradTop);
  assert.equal(gradTop.style.border_color, "#ff0000");
  assert.ok(gradTop.diagnostics.some((d) => d.code === "multi-paint-stroke"));
  assert.ok(gradTop.diagnostics.some((d) => d.code === "complex-stroke-flattened"));

  // A gradient with NO solid: border dropped, diagnosed — the old code
  // dropped it in silence.
  const gradOnly = extractStrokeStyle(sceneNode({ strokes: [gradient], strokeWeight: 1 }));
  assert.ok(gradOnly);
  assert.deepEqual(gradOnly.style, {});
  assert.deepEqual(gradOnly.diagnostics.map((d) => d.code), ["complex-stroke-flattened"]);
});

test("invisible paints do not count; no visible stroke emits nothing", () => {
  assert.equal(extractStrokeStyle(sceneNode({ strokes: [] })), undefined);
  assert.equal(
    extractStrokeStyle(sceneNode({ strokes: [{ ...red, visible: false }], strokeWeight: 2 })),
    undefined,
  );
  // GROUP nodes have no strokes member at all.
  assert.equal(extractStrokeStyle(sceneNode({ type: "GROUP" })), undefined);
});

test("stroke provenance rides as figma:* attributes, non-default values only", () => {
  const fancy = extractStrokeStyle(sceneNode({
    strokes: [red], strokeWeight: 1,
    strokeAlign: "OUTSIDE", strokeCap: "ROUND", strokeJoin: "BEVEL", strokeMiterLimit: 10,
  }));
  assert.ok(fancy);
  assert.equal(fancy.attributes?.["figma:stroke_align"], "outside");
  assert.equal(fancy.attributes?.["figma:stroke_cap"], "round");
  assert.equal(fancy.attributes?.["figma:stroke_join"], "bevel");
  assert.equal(fancy.attributes?.["figma:stroke_miter_limit"], "10");

  // INSIDE is how a CSS box border already paints; NONE/MITER/4.0 are the
  // Figma defaults — none of them earns an attribute.
  const dflt = extractStrokeStyle(sceneNode({
    strokes: [red], strokeWeight: 1,
    strokeAlign: "INSIDE", strokeCap: "NONE", strokeJoin: "MITER", strokeMiterLimit: 4,
  }));
  assert.ok(dflt);
  assert.equal(dflt.attributes, undefined);
});
