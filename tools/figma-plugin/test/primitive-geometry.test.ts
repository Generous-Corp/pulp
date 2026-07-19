// Primitive-shape provenance for the Figma plugin lane: arc/donut data,
// star/polygon point counts, corner smoothing, and the boolean-operation type
// are preserved as namespaced figma:* attributes so a future path renderer can
// rebuild the primitive without a re-export from Figma. The PNG capture keeps
// the pixels; these keep the semantics. Kept in lockstep with the REST lane
// (tools/import-design/figma_rest_export.py + test/test_figma_rest_export.py)
// and the .fig lane (tools/import-design/fig/scene.mjs + fig.test.mjs).

import { test } from "node:test";
import assert from "node:assert/strict";

import { extractPrimitiveGeometryAttrs } from "../src/extract-pure";

// extractPrimitiveGeometryAttrs reads only type + primitive fields; the cast
// keeps each fixture to the fields under test rather than a full SceneNode.
const sceneNode = (over: Record<string, unknown>) =>
  ({ name: "N", ...over }) as unknown as SceneNode;

test("an ELLIPSE with a partial sweep emits figma:arc_data", () => {
  const attrs = extractPrimitiveGeometryAttrs(sceneNode({
    type: "ELLIPSE",
    arcData: { startingAngle: 0, endingAngle: Math.PI, innerRadius: 0 },
  }));
  assert.ok(attrs);
  assert.equal(attrs["figma:arc_data"], "0,3.1416,0");
});

test("a donut ELLIPSE (full sweep, inner radius) emits figma:arc_data", () => {
  const attrs = extractPrimitiveGeometryAttrs(sceneNode({
    type: "ELLIPSE",
    arcData: { startingAngle: 0, endingAngle: Math.PI * 2, innerRadius: 0.5 },
  }));
  assert.ok(attrs);
  assert.equal(attrs["figma:arc_data"], "0,6.2832,0.5");
});

test("a plain full-circle ELLIPSE emits nothing — the default is not provenance", () => {
  // Figma stamps a default arcData on every ellipse; kiwi's float32 2π
  // (6.2831854820251465) must count as a full circle too.
  for (const end of [Math.PI * 2, 6.2831854820251465]) {
    const attrs = extractPrimitiveGeometryAttrs(sceneNode({
      type: "ELLIPSE",
      arcData: { startingAngle: 0, endingAngle: end, innerRadius: 0 },
    }));
    assert.equal(attrs, undefined);
  }
});

test("a STAR always emits point count and inner radius", () => {
  const attrs = extractPrimitiveGeometryAttrs(sceneNode({
    type: "STAR", pointCount: 7, innerRadius: 0.382,
  }));
  assert.ok(attrs);
  assert.equal(attrs["figma:star_point_count"], "7");
  assert.equal(attrs["figma:star_inner_radius"], "0.382");
});

test("a POLYGON emits its point count", () => {
  const attrs = extractPrimitiveGeometryAttrs(sceneNode({
    type: "POLYGON", pointCount: 6,
  }));
  assert.ok(attrs);
  assert.equal(attrs["figma:polygon_point_count"], "6");
  assert.ok(!("figma:star_point_count" in attrs), "polygon key, not star key");
});

test("a BOOLEAN_OPERATION emits its lowercased operation type", () => {
  for (const [op, want] of [
    ["UNION", "union"], ["SUBTRACT", "subtract"],
    ["INTERSECT", "intersect"], ["EXCLUDE", "exclude"],
  ] as const) {
    const attrs = extractPrimitiveGeometryAttrs(sceneNode({
      type: "BOOLEAN_OPERATION", booleanOperation: op,
    }));
    assert.ok(attrs);
    assert.equal(attrs["figma:boolean_operation"], want);
  }
});

test("cornerSmoothing > 0 emits figma:corner_smoothing on any cornered node", () => {
  const rect = extractPrimitiveGeometryAttrs(sceneNode({
    type: "RECTANGLE", cornerSmoothing: 0.6,
  }));
  assert.ok(rect);
  assert.equal(rect["figma:corner_smoothing"], "0.6");

  const frame = extractPrimitiveGeometryAttrs(sceneNode({
    type: "FRAME", cornerSmoothing: 1,
  }));
  assert.ok(frame);
  assert.equal(frame["figma:corner_smoothing"], "1");
});

test("zero cornerSmoothing (the circular default) emits nothing", () => {
  assert.equal(
    extractPrimitiveGeometryAttrs(sceneNode({ type: "RECTANGLE", cornerSmoothing: 0 })),
    undefined);
});

test("a node with no primitive metadata emits nothing at all", () => {
  assert.equal(extractPrimitiveGeometryAttrs(sceneNode({ type: "FRAME" })), undefined);
  assert.equal(extractPrimitiveGeometryAttrs(sceneNode({ type: "TEXT" })), undefined);
  assert.equal(extractPrimitiveGeometryAttrs(sceneNode({ type: "VECTOR" })), undefined);
});
