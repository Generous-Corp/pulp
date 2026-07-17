// Shape-primitive typing for the Figma plugin lane. A shape leaf must reach the
// IR as the shape it is: an ELLIPSE typed `frame` renders as a SQUARE, because a
// frame paints its own background box and codegen has no painter for a circle,
// so the fill has no way to become round. Kept in lockstep with the REST lane
// (tools/import-design/figma_rest_export.py + test/test_figma_rest_export.py)
// and the .fig lane (tools/import-design/fig/scene.mjs::envelopeType).

import { test } from "node:test";
import assert from "node:assert/strict";

import { mapNodeType } from "../src/extract-pure";
import { serializeExport } from "../src/serialize";
import type { ExtractedFigmaNode } from "../src/extract-model";
import { AssetCache } from "../src/assets";

// mapNodeType reads only `type`; the cast keeps the fixture to the field under
// test rather than standing up a full SceneNode the function never looks at.
const sceneNode = (type: string) => ({ type }) as unknown as SceneNode;

const ellipseNode = (): ExtractedFigmaNode => ({
  type: mapNodeType(sceneNode("ELLIPSE")),
  figma_type: "ELLIPSE",
  name: "knob base",
  figma_node_id: "3:1",
  parent_id: null,
  z_order: 0,
  absolute_bounds: { x: 0, y: 0, w: 40, h: 40 },
  relative_transform: [
    [1, 0, 0],
    [0, 1, 0],
  ],
  visible: true,
  locked: false,
  opacity: 1,
  blend_mode: "NORMAL",
  style: { background_color: "rgba(255, 0, 0, 1)" },
  layout: {},
  children: [],
});

test("mapNodeType: a filled ELLIPSE is typed `ellipse`, not `frame`", () => {
  // `ellipse` is what the C++ side has accepted all along — parse_ir_node passes
  // the string through, is_synthesizable_primitive covers it, and synthesize_node
  // lowers it to a real circular SVG path (synth_ellipse_path).
  assert.equal(mapNodeType(sceneNode("ELLIPSE")), "ellipse");
});

test("mapNodeType: a RECTANGLE stays `frame`", () => {
  // The control for the rule above: a rect IS a box, so a frame's own background
  // paints it correctly and it must NOT be re-typed.
  assert.equal(mapNodeType(sceneNode("RECTANGLE")), "frame");
});

test("mapNodeType: STAR / POLYGON stay `frame` (extract.ts captures them as images)", () => {
  // Pins the reason ELLIPSE needs a fix and these do not: extract.ts's
  // isVectorLike exports them as PNG assets (type `image`) before this typing can
  // matter — they land here only when that export fails, which diagnoses itself.
  // If that capture is ever removed, these become squares exactly like the
  // ellipse did, and this test is the breadcrumb saying so.
  assert.equal(mapNodeType(sceneNode("STAR")), "frame");
  assert.equal(mapNodeType(sceneNode("POLYGON")), "frame");
});

test("serializeExport: an ellipse reaches the wire as type `ellipse` with its fill", () => {
  // The end the C++ importer actually reads. The fill must ride along:
  // synthesize_node moves background_color onto the synthesized path, so a typed
  // ellipse that lost its fill would paint nothing at all.
  const out = serializeExport([ellipseNode()], [], {
    fileKey: "abc123",
    rootNodeId: "3:1",
    pluginVersion: "0.1.0",
    assets: new AssetCache(),
    tokens: { colors: {}, dimensions: {}, strings: {} },
  });
  const root = (out as Record<string, any>).root as Record<string, any>;

  assert.equal(root.type, "ellipse");
  assert.equal(root.style.background_color, "rgba(255, 0, 0, 1)");
});
