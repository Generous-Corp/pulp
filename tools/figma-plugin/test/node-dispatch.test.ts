// Node-type dispatch fidelity for the plugin lane. The contract under test:
// dispatch is exhaustive against the pinned plugin typings, so no node family
// reaches the envelope through a silent `default: frame`. Skipped families
// (SLICE, FigJam/Slides) emit NO node and a diagnostic; diagnosed families
// (SLOT, TEXT_PATH, unknown types) emit a node AND a diagnostic; plain
// containers (TRANSFORM_GROUP) dispatch explicitly with no noise. Kept in
// lockstep with the REST lane (tools/import-design/figma_rest_export.py::
// dispatch_node_type) and the .fig lane (tools/import-design/fig/scene.mjs).

import { test } from "node:test";
import assert from "node:assert/strict";

import { dispatchNodeType } from "../src/extract-pure";
import { extractScene } from "../src/extract";
import type { ExtractedDiagnostic } from "../src/extract-model";

// extractScene's token pass reaches for the figma sandbox global; an empty
// variables surface keeps the walk itself pure (same stub as tokens.test.ts).
(globalThis as unknown as { figma: unknown }).figma = {
  variables: { getLocalVariableCollectionsAsync: async () => [] },
};

// The walker reads bounds/visibility defensively, so a synthetic node only
// needs the fields the branch under test touches.
const bounds = (x: number, y: number, w: number, h: number) => ({
  x, y, width: w, height: h,
});

function syntheticTree(): SceneNode {
  return {
    type: "FRAME",
    id: "1:1",
    name: "Panel",
    visible: true,
    absoluteBoundingBox: bounds(0, 0, 400, 300),
    children: [
      {
        type: "TEXT_PATH",
        id: "1:2",
        name: "Curved Label",
        visible: true,
        absoluteBoundingBox: bounds(10, 10, 120, 24),
        characters: "WOW FACTOR",
        fontSize: 12,
        fontName: { family: "Inter", style: "Regular" },
        fills: [],
      },
      {
        type: "TRANSFORM_GROUP",
        id: "1:3",
        name: "Spun Group",
        visible: true,
        absoluteBoundingBox: bounds(10, 50, 80, 80),
        children: [],
      },
      {
        type: "SLOT",
        id: "1:4",
        name: "Content Slot",
        visible: true,
        absoluteBoundingBox: bounds(10, 140, 100, 40),
        children: [],
      },
      {
        type: "SLICE",
        id: "1:5",
        name: "Export @2x",
        visible: true,
        absoluteBoundingBox: bounds(0, 0, 400, 300),
      },
      {
        type: "STICKY",
        id: "1:6",
        name: "Reviewer Note",
        visible: true,
        absoluteBoundingBox: bounds(200, 10, 120, 120),
      },
    ],
  } as unknown as SceneNode;
}

const byCode = (diags: ExtractedDiagnostic[], code: string) =>
  diags.filter((d) => d.code === code);

test("walk: SLICE and STICKY are skipped (no node) with unsupported_node diagnostics", async () => {
  const res = await extractScene([syntheticTree()], { faithfulVector: false });
  const root = res.roots[0];

  // The skipped families emit NO node — a slice paints nothing in Figma, and
  // a sticky is FigJam furniture. Before this contract both became empty
  // generic frames that looked successfully imported.
  assert.deepEqual(
    root.children.map((c) => c.figma_type),
    ["TEXT_PATH", "TRANSFORM_GROUP", "SLOT"],
  );

  const slice = byCode(res.diagnostics, "slice-skipped");
  assert.equal(slice.length, 1);
  assert.equal(slice[0].kind, "unsupported_node");
  assert.match(slice[0].message, /Export @2x/);

  const sticky = byCode(res.diagnostics, "unsupported-node");
  assert.equal(sticky.length, 1);
  assert.equal(sticky[0].kind, "unsupported_node");
  assert.match(sticky[0].message, /STICKY/);
});

test("walk: TEXT_PATH dispatches to `text`, keeps its characters, and diagnoses the flattened path", async () => {
  const res = await extractScene([syntheticTree()], { faithfulVector: false });
  const textPath = res.roots[0].children.find((c) => c.figma_type === "TEXT_PATH");

  assert.ok(textPath);
  assert.equal(textPath.type, "text");
  // The content is the point: TEXT_PATH carries real copy, and re-typing it
  // must not drop it.
  assert.equal(textPath.content, "WOW FACTOR");
  assert.equal(textPath.style.font_family, "Inter");

  const diag = byCode(res.diagnostics, "text-path-flattened");
  assert.equal(diag.length, 1);
  assert.equal(diag[0].kind, "capture_partial");
});

test("walk: TRANSFORM_GROUP is an explicit frame with no diagnostic; SLOT is a diagnosed frame", async () => {
  const res = await extractScene([syntheticTree()], { faithfulVector: false });
  const children = res.roots[0].children;

  const spun = children.find((c) => c.figma_type === "TRANSFORM_GROUP");
  assert.ok(spun);
  assert.equal(spun.type, "frame");
  // A transform group renders fine as a container — explicit dispatch, no noise.
  assert.equal(
    res.diagnostics.filter((d) => /Spun Group/.test(d.message)).length, 0);

  const slot = children.find((c) => c.figma_type === "SLOT");
  assert.ok(slot);
  assert.equal(slot.type, "frame");
  const slotDiag = byCode(res.diagnostics, "slot-placeholder");
  assert.equal(slotDiag.length, 1);
  assert.equal(slotDiag[0].kind, "unsupported_node");
});

test("walk: a genuinely unknown node type falls back to frame WITH a diagnostic", async () => {
  const tree = {
    type: "FRAME",
    id: "2:1",
    name: "Panel",
    visible: true,
    absoluteBoundingBox: bounds(0, 0, 100, 100),
    children: [
      {
        type: "HOLOGRAM_2027", // a family newer than the pinned typings
        id: "2:2",
        name: "Future Thing",
        visible: true,
        absoluteBoundingBox: bounds(0, 0, 50, 50),
      },
    ],
  } as unknown as SceneNode;

  const res = await extractScene([tree], { faithfulVector: false });
  // Never crash on new families — the node survives as a frame...
  assert.equal(res.roots[0].children.length, 1);
  assert.equal(res.roots[0].children[0].type, "frame");
  // ...but the fallback is stated, not silent.
  const diag = byCode(res.diagnostics, "unknown-node-type");
  assert.equal(diag.length, 1);
  assert.equal(diag[0].kind, "unsupported_node");
  assert.match(diag[0].message, /HOLOGRAM_2027/);
});

test("dispatchNodeType: the full decision table, one family per row", () => {
  const emit = (t: string) => {
    const d = dispatchNodeType(t, "n");
    assert.equal(d.action, "emit", `${t} should emit`);
    return d.action === "emit" ? d.type : "";
  };
  // Explicit containers/geometry — no silent default involved.
  for (const t of ["FRAME", "GROUP", "SECTION", "TRANSFORM_GROUP", "COMPONENT",
                   "COMPONENT_SET", "INSTANCE", "RECTANGLE", "POLYGON",
                   "REGULAR_POLYGON", "STAR", "LINE"]) {
    assert.equal(emit(t), "frame");
  }
  assert.equal(emit("ELLIPSE"), "ellipse");
  assert.equal(emit("TEXT"), "text");
  assert.equal(emit("TEXT_PATH"), "text");
  assert.equal(emit("VECTOR"), "vector");
  assert.equal(emit("BOOLEAN_OPERATION"), "vector");
  assert.equal(emit("SLOT"), "frame");

  // Skips: SLICE plus every editor/Slides family in the pinned SceneNode union.
  for (const t of ["SLICE", "STICKY", "CONNECTOR", "SHAPE_WITH_TEXT",
                   "CODE_BLOCK", "STAMP", "WIDGET", "EMBED", "LINK_UNFURL",
                   "MEDIA", "HIGHLIGHT", "WASHI_TAPE", "TABLE", "TABLE_CELL",
                   "SLIDE", "SLIDE_ROW", "SLIDE_GRID",
                   "INTERACTIVE_SLIDE_ELEMENT"]) {
    const d = dispatchNodeType(t, "n");
    assert.equal(d.action, "skip", `${t} should skip`);
    assert.equal(d.diagnostic.kind, "unsupported_node");
  }
});
