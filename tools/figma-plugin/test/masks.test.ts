// Sibling-mask lowering for the plugin lane (audit item 9). The contract
// under test: a child with `isMask: true` paints NOWHERE — its outline clips
// the siblings painted after it via a synthetic `(mask scope)` wrapper whose
// `style.clip_path` carries the outline in the parent's space, siblings below
// the mask stay outside the wrapper, and inexact lowerings (luminance / soft
// alpha) say so with a structured diagnostic. Kept in lockstep with the .fig
// lane (tools/import-design/fig/scene.mjs::walkChildren) and the REST lane
// (tools/import-design/figma_rest_export.py).

import { test } from "node:test";
import assert from "node:assert/strict";

import { extractScene } from "../src/extract";
import {
  transformSvgPathData,
  boxOutlinePath,
  maskParentSpaceTransform,
  maskClipOutline,
  assessMaskFidelity,
} from "../src/extract-pure";
import type { ExtractedFigmaNode } from "../src/extract-model";

// extractScene's token pass reaches for the figma sandbox global; an empty
// variables surface keeps the walk itself pure (same stub as tokens.test.ts).
(globalThis as unknown as { figma: unknown }).figma = {
  variables: { getLocalVariableCollectionsAsync: async () => [] },
};

const bounds = (x: number, y: number, w: number, h: number) => ({
  x, y, width: w, height: h,
});

const IDENTITY = [[1, 0, 0], [0, 1, 0]] as const;

const sceneNode = (over: Record<string, unknown>) => over as unknown as SceneNode;

const opaqueFill = [{ type: "SOLID", visible: true, opacity: 1, color: { r: 0, g: 0, b: 0 } }];

/// A 400×300 panel whose children are [mask, contentA, contentB] plus
/// siblings BELOW the mask that must stay unclipped. The TEXT label also
/// keeps the frame out of the pure-vector-illustration capture path, so the
/// walk recurses into the children where mask lowering lives.
function maskedPanel(maskOverrides: Record<string, unknown> = {}): SceneNode {
  return sceneNode({
    type: "FRAME",
    id: "1:1",
    name: "Panel",
    visible: true,
    width: 400,
    height: 300,
    absoluteBoundingBox: bounds(0, 0, 400, 300),
    absoluteTransform: [[1, 0, 0], [0, 1, 0]],
    children: [
      {
        type: "TEXT",
        id: "1:8",
        name: "Label",
        visible: true,
        absoluteBoundingBox: bounds(0, 280, 60, 20),
        characters: "Label",
        fontSize: 12,
        fontName: { family: "Inter", style: "Regular" },
        fills: opaqueFill,
        textAlignHorizontal: "LEFT",
      },
      {
        type: "RECTANGLE",
        id: "1:2",
        name: "Below",
        visible: true,
        absoluteBoundingBox: bounds(0, 0, 400, 300),
        fills: opaqueFill,
      },
      {
        type: "RECTANGLE",
        id: "1:3",
        name: "Window",
        visible: true,
        isMask: true,
        width: 200,
        height: 100,
        cornerRadius: 0,
        absoluteBoundingBox: bounds(40, 30, 200, 100),
        absoluteTransform: [[1, 0, 40], [0, 1, 30]],
        relativeTransform: [[1, 0, 40], [0, 1, 30]],
        fills: opaqueFill,
        ...maskOverrides,
      },
      {
        type: "RECTANGLE",
        id: "1:4",
        name: "ContentA",
        visible: true,
        absoluteBoundingBox: bounds(0, 0, 400, 300),
        fills: opaqueFill,
      },
      {
        type: "RECTANGLE",
        id: "1:5",
        name: "ContentB",
        visible: true,
        absoluteBoundingBox: bounds(10, 10, 100, 100),
        fills: opaqueFill,
      },
    ],
  });
}

test("a sibling mask moves the siblings above it into a clip-path wrapper", async () => {
  const res = await extractScene([maskedPanel()], { faithfulVector: false });
  const root = res.roots[0];
  // The sibling BELOW the mask stays outside the wrapper — Figma's scope.
  assert.deepEqual(root.children.map((c) => c.name), ["Label", "Below", "Window (mask scope)"]);
  const scope = root.children[2];
  assert.equal(scope.type, "frame");
  assert.equal(scope.figma_node_id, "1:3/mask-scope");
  assert.equal(scope.audio_widget, "none");
  // The wrapper spans the parent from (0,0) so the outline (parent space)
  // clips where the design put the mask.
  assert.equal(scope.style.position, "absolute");
  assert.equal(scope.style.left, 0);
  assert.equal(scope.style.top, 0);
  assert.equal(scope.style.width, 400);
  assert.equal(scope.style.height, 300);
  assert.equal(scope.style.clip_path,
    'path("M40 30 L240 30 L240 130 L40 130 L40 30 Z")');
  // The masked siblings are its children, coordinates preserved.
  assert.deepEqual(scope.children.map((c) => c.name), ["ContentA", "ContentB"]);
  // The mask itself is painted NOWHERE.
  const flat: string[] = [];
  const collect = (n: ExtractedFigmaNode) => { flat.push(n.name); n.children.forEach(collect); };
  collect(root);
  assert.ok(!flat.includes("Window"));
  // An exact geometric lowering raises no mask diagnostic.
  assert.equal(res.diagnostics.filter((d) => d.code.includes("mask")).length, 0);
});

test("a luminance mask keeps the outline clip and says what it lost", async () => {
  const res = await extractScene(
    [maskedPanel({ maskType: "LUMINANCE" })],
    { faithfulVector: false },
  );
  const scope = res.roots[0].children.find((c) => c.name === "Window (mask scope)");
  assert.ok(scope, "best-effort outline clip still emitted");
  assert.ok(scope!.style.clip_path);
  const diag = res.diagnostics.find((d) => d.code === "mask-luminance-approximated");
  assert.ok(diag, "luminance loss is diagnosed");
  assert.equal(diag!.severity, "warning");
});

test("a soft alpha mask keeps the outline clip with a complex-mask diagnostic", async () => {
  const res = await extractScene(
    [maskedPanel({
      maskType: "ALPHA",
      fills: [{ type: "SOLID", visible: true, opacity: 0.4, color: { r: 0, g: 0, b: 0 } }],
    })],
    { faithfulVector: false },
  );
  const scope = res.roots[0].children.find((c) => c.name === "Window (mask scope)");
  assert.ok(scope);
  assert.ok(res.diagnostics.some((d) => d.code === "complex-mask-flattened"));
});

test("a mask inside an auto-layout parent degrades honestly", async () => {
  const tree = maskedPanel();
  (tree as unknown as { layoutMode: string }).layoutMode = "VERTICAL";
  const res = await extractScene([tree], { faithfulVector: false });
  const root = res.roots[0];
  // No wrapper, siblings flow unmasked — and the mask still never paints.
  assert.ok(!root.children.some((c) => c.name.includes("mask scope")));
  assert.ok(!root.children.some((c) => c.name === "Window"));
  assert.ok(res.diagnostics.some((d) =>
    d.code === "mask-approximated" && /auto-layout/.test(d.message)));
});

test("a hidden mask neither paints nor clips, and an empty scope is pruned", async () => {
  const hidden = await extractScene(
    [maskedPanel({ visible: false })], { faithfulVector: false });
  assert.deepEqual(
    hidden.roots[0].children.map((c) => c.name),
    ["Label", "Below", "ContentA", "ContentB"]);

  // Mask with nothing above it: the scope wrapper collapses.
  const tree = maskedPanel();
  (tree as unknown as { children: unknown[] }).children =
    (tree as unknown as { children: { name: string }[] }).children.slice(0, 3);
  const empty = await extractScene([tree], { faithfulVector: false });
  assert.deepEqual(empty.roots[0].children.map((c) => c.name), ["Label", "Below"]);
});

test("a second mask opens a nested scope so stacked masks intersect", async () => {
  const tree = maskedPanel();
  (tree as unknown as { children: unknown[] }).children.push({
    type: "RECTANGLE",
    id: "1:6",
    name: "Inner",
    visible: true,
    isMask: true,
    width: 50,
    height: 50,
    absoluteBoundingBox: bounds(0, 0, 50, 50),
    absoluteTransform: [[1, 0, 0], [0, 1, 0]],
    fills: opaqueFill,
  }, {
    type: "RECTANGLE",
    id: "1:7",
    name: "Deep",
    visible: true,
    absoluteBoundingBox: bounds(5, 5, 20, 20),
    fills: opaqueFill,
  });
  const res = await extractScene([tree], { faithfulVector: false });
  const outer = res.roots[0].children.find((c) => c.name === "Window (mask scope)")!;
  const inner = outer.children.find((c) => c.name === "Inner (mask scope)")!;
  assert.ok(inner.style.clip_path);
  assert.deepEqual(inner.children.map((c) => c.name), ["Deep"]);
});

test("vector masks clip via their fill geometry transformed to parent space", () => {
  const parent = sceneNode({
    type: "FRAME", id: "2:1", name: "P", width: 100, height: 100,
    absoluteBoundingBox: bounds(0, 0, 100, 100),
    absoluteTransform: [[1, 0, 0], [0, 1, 0]],
  });
  const mask = sceneNode({
    type: "VECTOR", id: "2:2", name: "V", isMask: true,
    width: 10, height: 10,
    absoluteBoundingBox: bounds(20, 30, 10, 10),
    absoluteTransform: [[1, 0, 20], [0, 1, 30]],
    fillGeometry: [{ windingRule: "NONZERO", data: "M0 0 L10 0 L10 10 Z" }],
  });
  assert.equal(maskClipOutline(mask, parent), "M20 30 L30 30 L30 40 Z");
});

test("an untransformable vector outline falls back to the box outline", () => {
  const parent = sceneNode({
    type: "FRAME", id: "2:1", name: "P", width: 100, height: 100,
    absoluteBoundingBox: bounds(0, 0, 100, 100),
    absoluteTransform: [[1, 0, 0], [0, 1, 0]],
  });
  const mask = sceneNode({
    type: "VECTOR", id: "2:3", name: "Arc", isMask: true,
    width: 10, height: 10,
    absoluteBoundingBox: bounds(20, 30, 10, 10),
    absoluteTransform: [[1, 0, 20], [0, 1, 30]],
    // Arcs don't survive a general affine; the approximate box clip beats
    // painting the mask or dropping the clip entirely.
    fillGeometry: [{ windingRule: "NONZERO", data: "M0 5 A5 5 0 1 0 10 5 Z" }],
  });
  assert.equal(maskClipOutline(mask, parent), "M20 30 L30 30 L30 40 L20 40 L20 30 Z");
});

test("transformSvgPathData applies the affine to every point", () => {
  assert.equal(
    transformSvgPathData("M0 0 L10 0 C10 5 5 10 0 10 Z", [[2, 0, 1], [0, 2, 2]]),
    "M1 2 L21 2 C21 12 11 22 1 22 Z");
  // H/V become L because horizontals stop being horizontal under rotation.
  assert.equal(
    transformSvgPathData("M0 0 H10 V5", [[1, 0, 0], [0, 1, 0]]),
    "M0 0 L10 0 L10 5");
  // Relative commands resolve against the current point first.
  assert.equal(
    transformSvgPathData("M10 10 l5 0 v5", [[1, 0, 0], [0, 1, 0]]),
    "M10 10 L15 10 L15 15");
  // An M's extra pairs are implicit line-tos.
  assert.equal(
    transformSvgPathData("M0 0 10 0 10 10", [[1, 0, 0], [0, 1, 0]]),
    "M0 0 L10 0 L10 10");
  // Arcs are unsupported → null (caller falls back to the box outline).
  assert.equal(transformSvgPathData("M0 0 A5 5 0 1 0 10 0", [[1, 0, 0], [0, 1, 0]]), null);
});

test("boxOutlinePath synthesizes rect, rounded-rect, and ellipse outlines", () => {
  assert.equal(boxOutlinePath(10, 6, IDENTITY), "M0 0 L10 0 L10 6 L0 6 L0 0 Z");
  const rounded = boxOutlinePath(20, 20, IDENTITY, { radii: { tl: 4, tr: 4, br: 4, bl: 4 } })!;
  assert.ok(rounded.startsWith("M4 0 L16 0 C"));
  const ellipse = boxOutlinePath(10, 10, IDENTITY, { isEllipse: true })!;
  assert.ok(ellipse.startsWith("M5 0 C"));
  assert.equal(boxOutlinePath(0, 10, IDENTITY), null);
});

test("maskParentSpaceTransform prefers absolute transforms (group-proof)", () => {
  // Child of a GROUP: relativeTransform is relative to the nearest non-group
  // container, so only the absolute-transform pair lands in the group's space.
  const group = sceneNode({
    type: "GROUP", id: "3:1", name: "G",
    absoluteTransform: [[1, 0, 100], [0, 1, 50]],
    absoluteBoundingBox: bounds(100, 50, 200, 200),
  });
  const child = sceneNode({
    type: "RECTANGLE", id: "3:2", name: "R",
    absoluteTransform: [[1, 0, 120], [0, 1, 60]],
    relativeTransform: [[1, 0, 120], [0, 1, 60]], // container space, NOT group space
    absoluteBoundingBox: bounds(120, 60, 10, 10),
  });
  assert.deepEqual(maskParentSpaceTransform(child, group), [[1, 0, 20], [0, 1, 10]]);
  // Bounding-box fallback when no transforms are exposed.
  const bare = sceneNode({
    type: "RECTANGLE", id: "3:3", name: "B",
    absoluteBoundingBox: bounds(130, 70, 10, 10),
  });
  assert.deepEqual(maskParentSpaceTransform(bare, group), [[1, 0, 30], [0, 1, 20]]);
});

test("assessMaskFidelity separates geometric, luminance, and soft alpha", () => {
  assert.equal(assessMaskFidelity(sceneNode({ maskType: "VECTOR" })), "geometric");
  assert.equal(assessMaskFidelity(sceneNode({ maskType: "LUMINANCE" })), "luminance");
  assert.equal(
    assessMaskFidelity(sceneNode({ maskType: "ALPHA", fills: opaqueFill })), "geometric");
  assert.equal(
    assessMaskFidelity(sceneNode({
      maskType: "ALPHA",
      fills: [{ type: "IMAGE", visible: true }],
    })), "soft_alpha");
  assert.equal(
    assessMaskFidelity(sceneNode({ fills: opaqueFill, opacity: 0.5 })), "soft_alpha");
  // No visible fill hides everything under an alpha mask — soft, not exact.
  assert.equal(assessMaskFidelity(sceneNode({ fills: [] })), "soft_alpha");
});
